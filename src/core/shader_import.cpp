// Fullscreen fragment shader import: scan a pasted body, then wrap it.
//
// The scanner never rejects and the wrapper never rewrites. Between them they
// hold one rule: the imported body is copied byte for byte, so a diagnostic the
// compiler reports about it points at code the user recognises. The only edit
// made to the body is removing version directives, because the generated prelude
// supplies its own and two of them do not compile.
#include "ssstudio/shader_import.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>

#include "ssstudio/json.h"
#include "ssstudio/templates.h"

namespace ssstudio {
namespace {

/// The id given to the fullscreen vertex shader an import writes for itself.
/// A later import recognises it by this name and pairs with it rather than
/// writing a second identical one.
constexpr std::string_view kFullscreenVertexId = "fullscreen_vert";

constexpr std::string_view kBodyMarker = "// --- imported source below this line ---";
constexpr std::string_view kEpilogueMarker = "// --- generated epilogue ---";

bool is_ident_char(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Turns CRLF and lone CR into LF. Pasted source routinely arrives with Windows
/// endings, and every offset this file computes assumes one byte per newline.
std::string normalize_newlines(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\r') {
            if (i + 1 < source.size() && source[i + 1] == '\n') continue;
            out.push_back('\n');
            continue;
        }
        out.push_back(source[i]);
    }
    return out;
}

/// A copy of the source with every comment byte replaced by a space, newlines
/// left alone. Searching this instead of the original is what keeps an entry
/// point named inside a comment from being mistaken for a real one, while
/// offsets still line up with the original text.
std::string mask_comments(const std::string& source) {
    std::string masked = source;
    enum class State { Code, Line, Block } state = State::Code;
    for (std::size_t i = 0; i < masked.size(); ++i) {
        const char c = masked[i];
        const char next = i + 1 < masked.size() ? masked[i + 1] : '\0';
        switch (state) {
            case State::Code:
                if (c == '/' && next == '/') {
                    state = State::Line;
                    masked[i] = ' ';
                    masked[i + 1] = ' ';
                    ++i;
                } else if (c == '/' && next == '*') {
                    state = State::Block;
                    masked[i] = ' ';
                    masked[i + 1] = ' ';
                    ++i;
                }
                break;
            case State::Line:
                if (c == '\n') {
                    state = State::Code;
                } else {
                    masked[i] = ' ';
                }
                break;
            case State::Block:
                // An unterminated block comment runs to the end of the source
                // rather than throwing the scan off; the compiler will complain
                // about it soon enough, and with a better message.
                if (c == '*' && next == '/') {
                    masked[i] = ' ';
                    masked[i + 1] = ' ';
                    ++i;
                    state = State::Code;
                } else if (c != '\n') {
                    masked[i] = ' ';
                }
                break;
        }
    }
    return masked;
}

/// Offset of `token` in `masked` as a whole identifier, or npos. Requiring both
/// neighbours to be non-identifier characters is what keeps `iChannel0` from
/// matching inside `iChannel0Blurred`.
std::size_t find_token(const std::string& masked, std::string_view token, std::size_t from = 0) {
    for (std::size_t at = masked.find(token, from); at != std::string::npos;
         at = masked.find(token, at + 1)) {
        const bool left_ok =
            at == 0 || !is_ident_char(static_cast<unsigned char>(masked[at - 1]));
        const std::size_t after = at + token.size();
        const bool right_ok =
            after >= masked.size() || !is_ident_char(static_cast<unsigned char>(masked[after]));
        if (left_ok && right_ok) return at;
    }
    return std::string::npos;
}

bool has_token(const std::string& masked, std::string_view token) {
    return find_token(masked, token) != std::string::npos;
}

/// True when `token` appears as an identifier immediately followed - across any
/// whitespace - by an opening parenthesis. A declaration or a call both count;
/// what matters is that the name is used as a function rather than mentioned.
bool has_function(const std::string& masked, std::string_view token) {
    for (std::size_t at = find_token(masked, token); at != std::string::npos;
         at = find_token(masked, token, at + 1)) {
        std::size_t i = at + token.size();
        while (i < masked.size() && std::isspace(static_cast<unsigned char>(masked[i]))) ++i;
        if (i < masked.size() && masked[i] == '(') return true;
    }
    return false;
}

/// The leading comment block, when the source opens with one. Read from the
/// original text rather than the masked copy, because the point is to keep it.
std::string leading_comment_block(const std::string& source) {
    std::size_t i = 0;
    while (i < source.size() && std::isspace(static_cast<unsigned char>(source[i]))) ++i;
    if (i + 1 >= source.size() || source[i] != '/') return {};

    if (source[i + 1] == '*') {
        const std::size_t end = source.find("*/", i + 2);
        return source.substr(i, end == std::string::npos ? std::string::npos : end + 2 - i);
    }
    if (source[i + 1] != '/') return {};

    // A run of consecutive `//` lines, which is how most notices are written.
    const std::size_t start = i;
    std::size_t end = i;
    while (end < source.size()) {
        if (source.compare(end, 2, "//") != 0) break;
        const std::size_t line_end = source.find('\n', end);
        if (line_end == std::string::npos) {
            end = source.size();
            break;
        }
        end = line_end + 1;
    }
    return source.substr(start, end - start);
}

/// Whether a comment block reads like a licence or attribution notice rather
/// than an ordinary explanatory comment. Deliberately generous: keeping one
/// comment too many costs nothing, losing an attribution is not recoverable.
bool looks_like_notice(std::string_view comment) {
    static constexpr std::string_view kSignals[] = {
        "licen", "copyright", "(c)", "©",  "cc0",       "cc by",
        "mit",   "gpl",       "bsd", "apache",  "zlib",      "public domain",
        "attribution", "author", "created by", "shared by", "all rights reserved",
    };
    const std::string lowered = to_lower(comment);
    return std::any_of(std::begin(kSignals), std::end(kSignals),
                       [&](std::string_view signal) {
                           return lowered.find(signal) != std::string::npos;
                       });
}

/// Removes every `#version` line. The prelude supplies its own, and a second one
/// anywhere in the translation unit is an error.
std::string strip_version_directives(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        const bool last = line_end == std::string::npos;
        if (last) line_end = source.size();

        std::string_view line(source.data() + line_start, line_end - line_start);
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const bool is_version = line.compare(i, 8, "#version") == 0;

        if (!is_version) {
            out.append(line);
            if (!last) out.push_back('\n');
        } else if (!last) {
            // Keep the line count stable so anything the user counted by eye in
            // the paste still matches the generated file.
            out.push_back('\n');
        }

        if (last) break;
        line_start = line_end + 1;
    }
    return out;
}

Diagnostic note(Severity severity, std::string message) {
    Diagnostic d;
    d.severity = severity;
    d.code = "SSSTUDIO-IMPORT";
    d.message = std::move(message);
    return d;
}

/// Comment lines for the generated file header, one per non-empty field.
void append_attribution(std::ostringstream& os, const ImportOptions& options) {
    if (!options.source_url.empty()) os << "// Source:  " << options.source_url << "\n";
    if (!options.author.empty()) os << "// Author:  " << options.author << "\n";
    if (!options.licence.empty()) os << "// Licence: " << options.licence << "\n";
}

}  // namespace

std::string_view to_string(ImportEntry e) {
    switch (e) {
        case ImportEntry::None: return "none";
        case ImportEntry::Image: return "mainImage";
        case ImportEntry::Sound: return "mainSound";
        case ImportEntry::VR: return "mainVR";
        case ImportEntry::Cubemap: return "mainCubemap";
    }
    return "none";
}

bool is_importable(ImportEntry e) { return e == ImportEntry::Image; }

std::string_view imported_body_marker() { return kBodyMarker; }

ImportScan scan_fullscreen_source(std::string_view source) {
    ImportScan scan;
    const std::string text = normalize_newlines(source);
    const std::string masked = mask_comments(text);

    // Order matters only in that Image wins when several are present: a paste
    // taken from a multi-tab shader often carries more than one.
    struct EntryProbe {
        std::string_view token;
        ImportEntry entry;
    };
    static constexpr EntryProbe kProbes[] = {
        {"mainImage", ImportEntry::Image},
        {"mainSound", ImportEntry::Sound},
        {"mainVR", ImportEntry::VR},
        {"mainCubemap", ImportEntry::Cubemap},
    };
    for (const EntryProbe& probe : kProbes) {
        if (!has_function(masked, probe.token)) continue;
        scan.all_entries.push_back(probe.entry);
        if (scan.entry == ImportEntry::None || probe.entry == ImportEntry::Image) {
            if (scan.entry != ImportEntry::Image) scan.entry = probe.entry;
        }
    }

    for (std::size_t i = 0; i < scan.channels_used.size(); ++i) {
        const std::string name = "iChannel" + std::to_string(i);
        scan.channels_used[i] = has_token(masked, name);
    }

    scan.uses_texture2d = has_token(masked, "texture2D");
    scan.uses_texturecube = has_token(masked, "textureCube");
    scan.uses_legacy_time = has_token(masked, "iGlobalTime");

    if (const std::size_t at = masked.find("#version"); at != std::string::npos) {
        std::size_t i = at + 8;
        while (i < masked.size() && (masked[i] == ' ' || masked[i] == '\t')) ++i;
        int value = 0;
        bool any = false;
        while (i < masked.size() && std::isdigit(static_cast<unsigned char>(masked[i]))) {
            value = value * 10 + (masked[i] - '0');
            ++i;
            any = true;
        }
        if (any) scan.version_directive = value;
    }

    const std::string comment = leading_comment_block(text);
    if (!comment.empty() && looks_like_notice(comment)) scan.leading_comment = comment;

    // --- notes ------------------------------------------------------------
    if (scan.entry == ImportEntry::None) {
        scan.notes.push_back(note(Severity::Error,
                                  "no entry point found; a fullscreen shader is expected to "
                                  "declare void mainImage(out vec4, in vec2)"));
    } else if (!is_importable(scan.entry)) {
        scan.notes.push_back(
            note(Severity::Error, std::string("this is a ") + std::string(to_string(scan.entry)) +
                                      " shader, which the preview cannot run"));
    }
    if (scan.all_entries.size() > 1) {
        std::string list;
        for (ImportEntry e : scan.all_entries) {
            if (!list.empty()) list += ", ";
            list += to_string(e);
        }
        scan.notes.push_back(note(Severity::Warning,
                                  "several entry points found (" + list +
                                      "); importing " + std::string(to_string(scan.entry))));
    }
    if (scan.version_directive) {
        scan.notes.push_back(note(Severity::Info,
                                  "the #version " + std::to_string(*scan.version_directive) +
                                      " directive is replaced by the generated prelude"));
    }
    std::string channels;
    for (std::size_t i = 0; i < scan.channels_used.size(); ++i) {
        if (!scan.channels_used[i]) continue;
        if (!channels.empty()) channels += ", ";
        channels += "iChannel" + std::to_string(i);
    }
    if (!channels.empty()) {
        scan.notes.push_back(note(Severity::Info,
                                  "this shader samples " + channels +
                                      "; bind them in the Inputs & Outputs panel, or they read "
                                      "as solid white"));
    }
    if (!scan.leading_comment.empty()) {
        scan.notes.push_back(
            note(Severity::Info, "a licence or attribution notice was found and will be kept"));
    }
    return scan;
}

std::optional<std::string> wrap_fullscreen_source(std::string_view source, const ImportScan& scan,
                                                  const ImportOptions& options,
                                                  Diagnostics& out_diags) {
    if (!is_importable(scan.entry)) {
        out_diags.push_back(
            note(Severity::Error,
                 scan.entry == ImportEntry::None
                     ? "nothing to import: no mainImage entry point was found"
                     : std::string("cannot import a ") + std::string(to_string(scan.entry)) +
                           " shader"));
        return std::nullopt;
    }

    const std::string body = strip_version_directives(normalize_newlines(source));

    std::ostringstream os;
    os << "#version 450\n";
    os << "// Imported into SDL Shader Studio as a fullscreen fragment shader.\n";
    append_attribution(os, options);
    os << "//\n";
    os << "// The prelude below and the epilogue at the end are generated: they declare\n";
    os << "// what the imported source expects to find and call mainImage once per pixel.\n";
    os << "// Everything between the markers is the imported source, kept byte for byte.\n";
    os << "// The file is yours to edit - but keep mainImage's signature, because the\n";
    os << "// epilogue calls it.\n";

    if (!scan.leading_comment.empty()) {
        os << "//\n// The imported source carried this notice:\n";
        std::istringstream lines(scan.leading_comment);
        std::string line;
        while (std::getline(lines, line)) os << "//   " << line << "\n";
    }

    os << "\nlayout(location = 0) out vec4 ssstudio_frag_color;\n\n";

    // Always the whole block, never only the members the body mentions. Pruning
    // would make the scanner load-bearing for whether the shader compiles at
    // all, and one missed reference - a name reached through a #define, say -
    // would surface as an undeclared identifier in code the user did not write.
    // The block costs 192 bytes pushed once per frame.
    os << "layout(set = " << options.uniform_set << ", binding = 0) uniform Frame {\n";
    os << "    vec3  iResolution;\n";
    os << "    float iTime;\n";
    os << "    float iTimeDelta;\n";
    os << "    float iFrameRate;\n";
    os << "    int   iFrame;\n";
    os << "    float iSampleRate;\n";
    os << "    vec4  iMouse;\n";
    os << "    vec4  iDate;\n";
    os << "    float iChannelTime[4];\n";
    os << "    vec3  iChannelResolution[4];\n";
    os << "};\n\n";

    // All four channels, for the same reason. An unused sampler is eliminated
    // from the compiled module, so declaring them costs nothing that survives.
    for (std::uint32_t i = 0; i < 4; ++i) {
        os << "layout(set = " << options.resource_set << ", binding = " << i
           << ") uniform sampler2D iChannel" << i << ";\n";
    }

    if (options.compat_defines) {
        os << "\n// Spellings from older revisions of the convention.\n";
        os << "#define texture2D   texture\n";
        os << "#define textureCube texture\n";
        os << "#define iGlobalTime iTime\n";
    }

    if (!options.common.empty()) {
        os << "\n// --- shared code, imported alongside the body ---\n";
        os << normalize_newlines(options.common);
        if (options.common.back() != '\n') os << "\n";
    }

    os << "\n" << kBodyMarker << "\n";
    os << body;
    if (!body.empty() && body.back() != '\n') os << "\n";

    os << "\n" << kEpilogueMarker << "\n";
    os << "void main() {\n";
    os << "    // The convention puts the origin at the bottom left, and Vulkan puts\n";
    os << "    // gl_FragCoord's at the top left. gl_FragCoord.xy is already at the pixel\n";
    os << "    // centre, so this flip lands exactly on the coordinate the shader expects.\n";
    os << "    vec2 fragCoord = vec2(gl_FragCoord.x, iResolution.y - gl_FragCoord.y);\n";
    os << "    vec4 color = vec4(0.0, 0.0, 0.0, 1.0);\n";
    os << "    mainImage(color, fragCoord);\n";
    if (options.force_opaque) {
        os << "    ssstudio_frag_color = vec4(color.rgb, 1.0);\n";
    } else {
        os << "    ssstudio_frag_color = color;\n";
    }
    os << "}\n";

    for (const Diagnostic& d : scan.notes) {
        if (d.severity == Severity::Error) continue;
        out_diags.push_back(d);
    }
    return os.str();
}

namespace {

/// Today, as ISO 8601. Local date rather than UTC: the value is read by a human
/// wondering when they imported something.
std::string today_iso() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday);
    return buffer;
}

/// Writes `text` to `path`, creating parent directories. Refuses to replace an
/// existing file: an import that silently overwrote a shader someone had edited
/// would be the worst kind of data loss, because the source it replaced was the
/// only copy.
bool write_new_file(const std::filesystem::path& path, const std::string& text,
                    Diagnostics& out_diags) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        out_diags.push_back(note(Severity::Error,
                                 "refusing to overwrite " + path.string() +
                                     "; choose a different shader id"));
        return false;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        out_diags.push_back(note(Severity::Error, "could not write " + path.string()));
        return false;
    }
    out << text;
    if (!out) {
        out_diags.push_back(note(Severity::Error, "could not finish writing " + path.string()));
        return false;
    }
    return true;
}

}  // namespace

bool read_imported_chain(std::string_view json, ImportedChain& out, Diagnostics& out_diags) {
    out = ImportedChain{};

    JsonValue document;
    std::string parse_error;
    if (!parse_json(json, document, parse_error)) {
        out_diags.push_back(note(Severity::Error, "could not read the description: " + parse_error));
        return false;
    }

    // The passes sit under a wrapper object in an exported file and at the top
    // level in one that has been unwrapped by hand. Both are accepted rather
    // than making somebody guess which they have.
    const JsonValue& root = document.has("Shader") ? document["Shader"] : document;
    const JsonValue& renderpasses = root["renderpass"];
    if (!renderpasses.is_array() || renderpasses.elements().empty()) {
        out_diags.push_back(
            note(Severity::Error, "this description has no render passes in it"));
        return false;
    }

    out.name = root["info"]["name"].as_string();
    out.author = root["info"]["username"].as_string();

    // Which pass each declared output id belongs to. Inputs reference a pass by
    // this id and not by its name, so the table is what has to be followed -
    // matching on names would go wrong the moment two passes shared one.
    std::map<double, std::string> pass_of_output;
    for (const JsonValue& entry : renderpasses.elements()) {
        const std::string name = entry["name"].as_string();
        for (const JsonValue& output : entry["outputs"].elements()) {
            if (output.has("id")) pass_of_output[output["id"].as_number(-1.0)] = name;
        }
    }

    std::vector<ImportedPass> buffers;
    ImportedPass image;
    bool have_image = false;

    for (const JsonValue& entry : renderpasses.elements()) {
        const std::string type = entry["type"].as_string();
        const std::string name = entry["name"].as_string();

        if (type == "common") {
            out.common = entry["code"].as_string();
            continue;
        }
        if (type != "image" && type != "buffer") {
            // Sound and cubemap passes need machinery the preview does not have.
            out.notes.push_back(note(Severity::Warning, "the " + type + " pass '" + name +
                                                            "' was left out"));
            continue;
        }

        ImportedPass pass;
        pass.name = name;
        pass.source = entry["code"].as_string();
        pass.is_image = type == "image";

        for (const JsonValue& input : entry["inputs"].elements()) {
            const auto channel = static_cast<std::uint32_t>(input["channel"].as_number(0.0));
            const std::string ctype = input["ctype"].as_string();

            if (ctype == "buffer") {
                const auto it = pass_of_output.find(input["id"].as_number(-1.0));
                if (it == pass_of_output.end()) {
                    out.notes.push_back(note(Severity::Warning,
                                             "'" + name + "' channel " + std::to_string(channel) +
                                                 " reads a pass that is not in this description"));
                    continue;
                }
                pass.channel_passes[channel] = it->second;
            } else if (ctype == "texture") {
                const std::string src = input["src"].as_string();
                // A description usually names its images by a path relative to
                // wherever it came from, and this has no way of knowing where
                // that was. Kept as it stands so the name is not lost, and said
                // out loud so it is not mistaken for something that will work.
                if (src.rfind("http://", 0) != 0 && src.rfind("https://", 0) != 0) {
                    out.notes.push_back(note(Severity::Warning,
                                             "'" + name + "' channel " + std::to_string(channel) +
                                                 " names '" + src +
                                                 "', which is not a full address - point it at "
                                                 "the image yourself"));
                }
                pass.channel_urls[channel] = src;
            } else {
                // Keyboard, microphone, webcam, video, a cubemap: nothing here
                // can produce any of them, and binding something else would be
                // worse than saying so.
                out.notes.push_back(note(Severity::Warning,
                                         "'" + name + "' channel " + std::to_string(channel) +
                                             " uses " + (ctype.empty() ? "an unknown source"
                                                                       : ctype) +
                                             ", which the preview cannot provide"));
                continue;
            }

            // Written as strings rather than as booleans by the exporters that
            // produce these, which is why they are read through as_bool.
            const JsonValue& sampler = input["sampler"];
            std::map<std::string, std::string> extra;
            const std::string filter = sampler["filter"].as_string();
            const std::string wrap = sampler["wrap"].as_string();
            if (filter == "nearest" || filter == "linear" || filter == "mipmap") {
                extra["filter"] = filter;
            }
            if (wrap == "clamp" || wrap == "repeat") extra["wrap"] = wrap;
            if (sampler.has("vflip")) extra["vflip"] = sampler["vflip"].as_bool() ? "true"
                                                                                 : "false";
            if (sampler.has("srgb")) extra["srgb"] = sampler["srgb"].as_bool() ? "true" : "false";
            if (!extra.empty()) pass.channel_sampler[channel] = std::move(extra);
        }

        if (pass.is_image) {
            if (have_image) {
                out.notes.push_back(note(Severity::Warning, "'" + name +
                                                                "' is a second image pass and was "
                                                                "left out"));
                continue;
            }
            image = std::move(pass);
            have_image = true;
        } else {
            buffers.push_back(std::move(pass));
        }
    }

    if (!have_image) {
        out_diags.push_back(
            note(Severity::Error, "this description has no pass that draws the image"));
        return false;
    }

    // The order in the file is not the running order. Buffers run in the order
    // their names sort in - which is what "Buf A" through "Buf D" is - and the
    // image runs last.
    std::sort(buffers.begin(), buffers.end(),
              [](const ImportedPass& a, const ImportedPass& b) { return a.name < b.name; });

    out.passes = std::move(buffers);
    out.passes.push_back(std::move(image));
    return true;
}

std::string unique_shader_id(const Project& project, std::string_view desired) {
    // Ids become enumerator names in generated headers, so anything that is not
    // an identifier character is folded to an underscore and a leading digit is
    // pushed out of the way.
    std::string base;
    for (const char c : desired) {
        base.push_back(is_ident_char(static_cast<unsigned char>(c)) ? c : '_');
    }
    while (!base.empty() && base.front() == '_') base.erase(base.begin());
    while (!base.empty() && base.back() == '_') base.pop_back();
    if (base.empty()) base = "imported";
    if (std::isdigit(static_cast<unsigned char>(base.front()))) base.insert(base.begin(), 's');
    base = to_lower(base);

    if (!project.find_shader(base)) return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string candidate = base + "_" + std::to_string(suffix);
        if (!project.find_shader(candidate)) return candidate;
    }
    return base;
}

bool write_import_files(const Project& project, const ImportRequest& request, ImportResult& out,
                        Diagnostics& out_diags) {
    out.scan = scan_fullscreen_source(request.source);

    const auto wrapped =
        wrap_fullscreen_source(request.source, out.scan, request.options, out_diags);
    if (!wrapped) return false;

    out.shader_id = unique_shader_id(project, request.id.empty() ? "imported" : request.id);
    out.shader_path =
        std::filesystem::path("shaders") / (out.shader_id + default_extension(Stage::Fragment, Language::GLSL));

    // The preview draws a vertex and a fragment shader together, and an imported
    // shader covers the whole screen - so it needs a partner that covers the
    // whole screen too.
    //
    // Any vertex shader already in the project is not a substitute. A typical one
    // expects vertex data, and the preview binds none, so the pair would draw
    // something meaningless and the shader would get the blame. Only the
    // fullscreen vertex shader a previous import left behind is reused; anything
    // else means writing a new one.
    const ShaderDesc* reusable = nullptr;
    for (const ShaderDesc& s : project.shaders) {
        if (s.stage != Stage::Vertex) continue;
        if (s.id.rfind(kFullscreenVertexId, 0) != 0) continue;
        reusable = &s;
        break;
    }
    if (reusable) {
        out.vertex_id = reusable->id;
    } else {
        out.vertex_id = unique_shader_id(project, kFullscreenVertexId);
        out.vertex_path = std::filesystem::path("shaders") /
                          (out.vertex_id + default_extension(Stage::Vertex, Language::GLSL));
    }
    const bool needs_vertex = !out.vertex_path.empty();

    if (!write_new_file(project.absolute(out.shader_path), *wrapped, out_diags)) return false;
    if (needs_vertex) {
        if (!write_new_file(project.absolute(out.vertex_path),
                            fullscreen_vertex_source(Language::GLSL), out_diags)) {
            // The fragment shader is already on disk. Take it back out rather
            // than leaving the project half imported.
            std::error_code ec;
            std::filesystem::remove(project.absolute(out.shader_path), ec);
            return false;
        }
    }

    out.pipeline.name = out.shader_id;
    out.pipeline.vertex = out.vertex_id;
    out.pipeline.fragment = out.shader_id;

    out.provenance.url = request.options.source_url;
    out.provenance.author = request.options.author;
    out.provenance.licence = request.options.licence;
    // A notice found in the source is better than nothing when the caller did
    // not supply terms of its own.
    if (out.provenance.licence.empty() && !out.scan.leading_comment.empty()) {
        out.provenance.licence = "see the notice at the top of the source";
    }
    out.provenance.imported = today_iso();
    return true;
}

bool write_chain_files(const Project& project, const ImportedChain& chain,
                       const ImportOptions& options, ChainImportResult& out,
                       Diagnostics& out_diags) {
    out = ChainImportResult{};
    if (chain.passes.empty()) {
        out_diags.push_back(note(Severity::Error, "there are no passes to import"));
        return false;
    }

    // Ids first, for every pass, before anything is written. A pass names
    // another by id in its bindings, so they all have to exist before the first
    // binding is made - and a half-written chain is worse than none.
    Project reserved = project;
    std::map<std::string, std::string> id_of_pass;
    for (const ImportedPass& pass : chain.passes) {
        const std::string base =
            pass.name.empty() ? (pass.is_image ? "image" : "buffer") : pass.name;
        const std::string id = unique_shader_id(reserved, base);
        id_of_pass[pass.name] = id;

        ShaderDesc placeholder;
        placeholder.id = id;
        reserved.shaders.push_back(std::move(placeholder));
    }

    const bool needs_vertex =
        std::none_of(project.shaders.begin(), project.shaders.end(), [](const ShaderDesc& s) {
            return s.stage == Stage::Vertex && s.id.rfind(kFullscreenVertexId, 0) == 0;
        });
    if (needs_vertex) {
        out.vertex_id = unique_shader_id(reserved, kFullscreenVertexId);
        out.vertex_path = std::filesystem::path("shaders") /
                          (out.vertex_id + default_extension(Stage::Vertex, Language::GLSL));
    } else {
        for (const ShaderDesc& shader : project.shaders) {
            if (shader.stage != Stage::Vertex) continue;
            if (shader.id.rfind(kFullscreenVertexId, 0) != 0) continue;
            out.vertex_id = shader.id;
            break;
        }
    }

    // Written to a list first and moved into place only once every one of them
    // succeeded, so a failure half way leaves the project as it was.
    std::vector<std::filesystem::path> written;
    const auto undo = [&written] {
        std::error_code ec;
        for (const auto& path : written) std::filesystem::remove(path, ec);
    };

    for (const ImportedPass& pass : chain.passes) {
        const std::string& id = id_of_pass.at(pass.name);

        ImportOptions pass_options = options;
        pass_options.common = chain.common;
        // A buffer's alpha is data rather than coverage, so only the pass that
        // is actually shown is forced opaque.
        pass_options.force_opaque = pass.is_image;

        const ImportScan scan = scan_fullscreen_source(pass.source);
        const auto wrapped = wrap_fullscreen_source(pass.source, scan, pass_options, out_diags);
        if (!wrapped) {
            out_diags.push_back(note(Severity::Error, "pass '" + pass.name + "' could not be "
                                                      "wrapped"));
            undo();
            return false;
        }

        const std::filesystem::path relative =
            std::filesystem::path("shaders") /
            (id + default_extension(Stage::Fragment, Language::GLSL));
        if (!write_new_file(project.absolute(relative), *wrapped, out_diags)) {
            undo();
            return false;
        }
        written.push_back(project.absolute(relative));
        out.shaders.emplace_back(id, relative);

        // Channels become bindings. A channel reading a pass that runs later,
        // or itself, is a previous-frame read - the scheduler works that out
        // again from the order, but saying it here keeps the manifest honest
        // about what was meant.
        std::map<std::string, Binding> bindings;
        for (const auto& [channel, source_pass] : pass.channel_passes) {
            const auto target = id_of_pass.find(source_pass);
            if (target == id_of_pass.end()) continue;

            Binding binding;
            binding.source = source_pass == pass.name ? BindingSource::PreviousFrame
                                                      : BindingSource::PassOutput;
            binding.text = target->second;
            if (const auto it = pass.channel_sampler.find(channel);
                it != pass.channel_sampler.end()) {
                binding.extra = it->second;
            }
            bindings["iChannel" + std::to_string(channel)] = std::move(binding);
        }
        for (const auto& [channel, url] : pass.channel_urls) {
            Binding binding;
            binding.source = BindingSource::Url;
            // Left unfetched on purpose. A description is data somebody else
            // wrote, and opening one must not make this machine start talking
            // to addresses they chose.
            binding.text = url;
            if (const auto it = pass.channel_sampler.find(channel);
                it != pass.channel_sampler.end()) {
                binding.extra = it->second;
            }
            bindings["iChannel" + std::to_string(channel)] = std::move(binding);
        }
        if (!bindings.empty()) out.bindings[id] = std::move(bindings);

        if (pass.is_image) {
            out.pipeline.fragment = id;
        } else {
            PassDesc desc;
            desc.shader_id = id;
            out.pipeline.passes.push_back(std::move(desc));
        }
    }

    if (needs_vertex) {
        if (!write_new_file(project.absolute(out.vertex_path),
                            fullscreen_vertex_source(Language::GLSL), out_diags)) {
            undo();
            return false;
        }
    }

    out.pipeline.name = unique_shader_id(project, chain.name.empty() ? "imported" : chain.name);
    out.pipeline.vertex = out.vertex_id;

    out.provenance.url = options.source_url;
    out.provenance.author = options.author.empty() ? chain.author : options.author;
    out.provenance.licence = options.licence;
    out.provenance.imported = today_iso();
    return true;
}

std::size_t imported_body_offset(std::string_view wrapped) {
    const std::size_t at = wrapped.find(kBodyMarker);
    if (at == std::string_view::npos) return std::string_view::npos;
    const std::size_t line_end = wrapped.find('\n', at);
    if (line_end == std::string_view::npos) return std::string_view::npos;
    return line_end + 1;
}

}  // namespace ssstudio
