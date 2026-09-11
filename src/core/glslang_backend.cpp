// GLSL front end.
//
// glslang compiles Vulkan-dialect GLSL to SPIR-V; everything downstream
// (reflection, packer, docs, loader) is unchanged, because it only ever sees
// SPIR-V. The pack's language byte records provenance and nothing else.
#include <algorithm>
#include <chrono>
#include <sstream>

#include "ssstudio/compiler.h"
#include "ssstudio/spirv_reflect.h"

#if defined(SSSTUDIO_HAVE_GLSLANG)
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
// An installed glslang puts the SPIR-V generator under glslang/, while a source
// tree consumed straight from the build (FetchContent) keeps it at the top
// level. Both layouts are supported so either way of resolving the dependency
// compiles.
#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>
#else
#include <SPIRV/GlslangToSpv.h>
#endif
#endif

namespace ssstudio {
namespace {

Diagnostic make_diag(Severity severity, std::string message, std::string file = {}, int line = 0,
                     std::string code = {}) {
    Diagnostic d;
    d.severity = severity;
    d.message = std::move(message);
    d.file = std::move(file);
    d.line = line;
    d.code = std::move(code);
    return d;
}

// glslang writes "ERROR: 0:12: 'x' : undeclared identifier". Parsed into the
// same Diagnostic shape DXC output produces, so the editor gutter does not care
// which compiler ran.
[[maybe_unused]] Diagnostics parse_glslang_log(const std::string& log,
                                              const std::string& file) {
    Diagnostics out;
    std::istringstream stream(log);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        if (line.rfind("ERROR: ", 0) != 0 && line.rfind("WARNING: ", 0) != 0) {
            if (line.find("compilation errors") != std::string::npos) continue;
            continue;
        }
        const bool is_error = line.rfind("ERROR: ", 0) == 0;
        std::string rest = line.substr(is_error ? 7 : 9);

        int line_number = 0;
        // "0:12:" - the first number is the string index, the second the line.
        const std::size_t first = rest.find(':');
        if (first != std::string::npos) {
            const std::size_t second = rest.find(':', first + 1);
            if (second != std::string::npos) {
                const std::string number = rest.substr(first + 1, second - first - 1);
                if (!number.empty() &&
                    number.find_first_not_of("0123456789") == std::string::npos) {
                    line_number = std::stoi(number);
                    rest = rest.substr(second + 1);
                }
            }
        }
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        out.push_back(make_diag(is_error ? Severity::Error : Severity::Warning, rest, file,
                                line_number, "GLSLANG"));
    }
    return out;
}

#if defined(SSSTUDIO_HAVE_GLSLANG)

class GlslangBackend final : public ICompilerBackend {
public:
    GlslangBackend() { initialized_ = glslang::InitializeProcess(); }
    ~GlslangBackend() override {
        if (initialized_) glslang::FinalizeProcess();
    }

    std::string name() const override { return "glslang"; }
    bool supports(Language l) const override { return l == Language::GLSL; }
    std::uint32_t supported_formats() const override { return FORMAT_SPIRV; }

    Diagnostics check(const CompileRequest& req) override { return compile(req).diagnostics; }

    CompileResult compile(const CompileRequest& req) override {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();

        CompileResult result;
        if (!initialized_) {
            result.diagnostics.push_back(
                make_diag(Severity::Error, "glslang failed to initialize", req.path.string()));
            return result;
        }

        const EShLanguage stage = req.stage == Stage::Vertex     ? EShLangVertex
                                  : req.stage == Stage::Fragment ? EShLangFragment
                                                                 : EShLangCompute;

        // Defines are prepended rather than passed as options so line numbers in
        // diagnostics still line up with what the user sees, thanks to #line.
        std::string preamble;
        for (const auto& [key, value] : req.defines) {
            preamble += "#define " + key + " " + (value.empty() ? "1" : value) + "\n";
        }

        glslang::TShader shader(stage);
        // glslang keeps both pointers until the parse, so the name has to
        // outlive the call rather than dangle off a temporary.
        const std::string source_name = req.path.string();
        const char* sources[] = {req.source.c_str()};
        const char* names[] = {source_name.c_str()};
        shader.setStringsWithLengthsAndNames(sources, nullptr, names, 1);
        shader.setEntryPoint(req.entry_point.c_str());
        shader.setSourceEntryPoint(req.entry_point.c_str());
        if (!preamble.empty()) shader.setPreamble(preamble.c_str());
        shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

        EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        if (req.debug_info) {
            messages = static_cast<EShMessages>(messages | EShMsgDebugInfo);
        }

        if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
            result.diagnostics = parse_glslang_log(shader.getInfoLog(), req.path.string());
            if (result.diagnostics.empty()) {
                result.diagnostics.push_back(
                    make_diag(Severity::Error, "GLSL compilation failed", req.path.string()));
            }
            return result;
        }

        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages)) {
            result.diagnostics = parse_glslang_log(program.getInfoLog(), req.path.string());
            if (result.diagnostics.empty()) {
                result.diagnostics.push_back(
                    make_diag(Severity::Error, "GLSL link failed", req.path.string()));
            }
            return result;
        }

        Diagnostics warnings = parse_glslang_log(shader.getInfoLog(), req.path.string());
        result.diagnostics.insert(result.diagnostics.end(), warnings.begin(), warnings.end());

        std::vector<unsigned int> words;
        glslang::SpvOptions spv_options;
        spv_options.generateDebugInfo = req.debug_info;
        spv_options.disableOptimizer = req.optimization == 0;
        spv_options.optimizeSize = req.optimization >= 3;
        glslang::GlslangToSpv(*program.getIntermediate(stage), words, &spv_options);

        std::vector<std::uint8_t> spirv(words.size() * sizeof(unsigned int));
        std::memcpy(spirv.data(), words.data(), spirv.size());

        result.reflection = reflect_spirv(spirv, req.stage, req.entry_point, result.diagnostics);
        result.reflection.entry_point = req.entry_point;

        Diagnostics binding = result.reflection.validate_binding_model(req.path.string());
        result.diagnostics.insert(result.diagnostics.end(), binding.begin(), binding.end());

        if (req.formats & FORMAT_SPIRV) result.blobs[FORMAT_SPIRV] = std::move(spirv);

        // Other formats come from the SPIR-V translation step, which the HLSL
        // backend already owns; the composite backend below wires that up.
        if (req.warnings_as_errors) {
            for (auto& d : result.diagnostics) {
                if (d.severity == Severity::Warning) d.severity = Severity::Error;
            }
        }
        result.ok = !has_errors(result.diagnostics) && !result.blobs.empty();
        result.milliseconds = std::chrono::duration<double, std::milli>(clock::now() - start).count();
        return result;
    }

private:
    bool initialized_ = false;
};

#endif  // SSSTUDIO_HAVE_GLSLANG

}  // namespace

std::unique_ptr<ICompilerBackend> create_glslang_backend() {
#if defined(SSSTUDIO_HAVE_GLSLANG)
    return std::make_unique<GlslangBackend>();
#else
    return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Composite backend: routes by language, so a project can mix HLSL and GLSL.
// ---------------------------------------------------------------------------
CompositeBackend::CompositeBackend(std::unique_ptr<ICompilerBackend> hlsl,
                                   std::unique_ptr<ICompilerBackend> glsl)
    : hlsl_(std::move(hlsl)), glsl_(std::move(glsl)) {}

std::string CompositeBackend::name() const {
    std::string out;
    if (hlsl_) out += hlsl_->name();
    if (glsl_) out += (out.empty() ? "" : " + ") + glsl_->name();
    return out.empty() ? "none" : out;
}

bool CompositeBackend::supports(Language l) const {
    const ICompilerBackend* backend = pick(l);
    return backend && backend->supports(l);
}

std::uint32_t CompositeBackend::supported_formats() const {
    std::uint32_t mask = FORMAT_NONE;
    if (hlsl_) mask |= hlsl_->supported_formats();
    if (glsl_) mask |= glsl_->supported_formats();
    return mask;
}

const ICompilerBackend* CompositeBackend::pick(Language l) const {
    if (l == Language::GLSL) return glsl_ ? glsl_.get() : nullptr;
    return hlsl_ ? hlsl_.get() : nullptr;
}

ICompilerBackend* CompositeBackend::pick(Language l) {
    return const_cast<ICompilerBackend*>(std::as_const(*this).pick(l));
}

Diagnostics CompositeBackend::check(const CompileRequest& req) {
    ICompilerBackend* backend = pick(req.language);
    if (!backend) return missing(req);
    return backend->check(req);
}

CompileResult CompositeBackend::compile(const CompileRequest& req) {
    ICompilerBackend* backend = pick(req.language);
    if (!backend) {
        CompileResult result;
        result.diagnostics = missing(req);
        return result;
    }
    CompileResult result = backend->compile(req);

    // glslang only produces SPIR-V. When the profile wants DXIL/DXBC/MSL as well,
    // hand the SPIR-V to the HLSL backend's translator so a GLSL shader ships to
    // the same platforms as an HLSL one.
    const std::uint32_t translated_formats = req.formats & ~static_cast<std::uint32_t>(FORMAT_SPIRV);
    if (result.ok && req.language == Language::GLSL && translated_formats && hlsl_) {
        auto spirv = result.blobs.find(FORMAT_SPIRV);
        if (spirv != result.blobs.end()) {
            CompileResult translated = translate_spirv(*hlsl_, req, spirv->second);
            for (auto& [format, blob] : translated.blobs) {
                result.blobs[format] = std::move(blob);
            }
            result.diagnostics.insert(result.diagnostics.end(), translated.diagnostics.begin(),
                                      translated.diagnostics.end());
            result.ok = !has_errors(result.diagnostics);
        }
    }
    return result;
}

Diagnostics CompositeBackend::missing(const CompileRequest& req) const {
    Diagnostics out;
    const std::string language(to_string(req.language));
    out.push_back(make_diag(
        Severity::Error,
        "no compiler available for " + language +
            " in this build. Enable it in Settings > Languages, or reconfigure with " +
            (req.language == Language::GLSL ? "-DSSSTUDIO_WITH_GLSLANG=ON" : "-DSSSTUDIO_WITH_SHADERCROSS=ON"),
        req.path.string(), 0, "SSSTUDIO-TOOL"));
    return out;
}

}  // namespace ssstudio
