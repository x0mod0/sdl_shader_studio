// Graph documents on disk: graphs/<shader id>.toml.
//
// Node positions are stored so a graph reopens exactly as it was left; that is
// the difference between a tool people trust and one they re-arrange every time.
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "ssstudio/graph.h"

namespace ssstudio {
namespace {

Diagnostic error(std::string message, std::string file = {}, int line = 0) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-GRAPH";
    d.message = std::move(message);
    d.file = std::move(file);
    d.line = line;
    return d;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::vector<double> read_values(const toml::node& node) {
    std::vector<double> out;
    if (const auto* array = node.as_array()) {
        for (const auto& element : *array) out.push_back(element.value_or(0.0));
    } else {
        out.push_back(node.value_or(0.0));
    }
    return out;
}

}  // namespace

bool load_graph(const std::filesystem::path& path, Graph& out, Diagnostics& out_diags) {
    toml::table table;
    try {
        table = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        out_diags.push_back(error(std::string(e.description()), path.string(),
                                  static_cast<int>(e.source().begin.line)));
        return false;
    }

    out = Graph{};
    out.shader_id = table["graph"]["shader"].value_or(path.stem().string());
    out.detached = table["graph"]["detached"].value_or(false);
    out.formatted_output = table["graph"]["formatted"].value_or(true);

    if (auto stage = stage_from_string(table["graph"]["stage"].value_or(std::string("fragment")))) {
        out.stage = *stage;
    }
    if (auto language =
            language_from_string(table["graph"]["language"].value_or(std::string("hlsl")))) {
        out.language = *language;
    }
    if (auto coercion =
            coercion_from_string(table["graph"]["coercion"].value_or(std::string("widening")))) {
        out.coercion = *coercion;
    }

    if (const auto* nodes = table["nodes"].as_array()) {
        for (const auto& element : *nodes) {
            const auto* t = element.as_table();
            if (!t) continue;
            Node node;
            node.id = (*t)["id"].value_or(std::string());
            node.type = (*t)["type"].value_or(std::string());
            node.name = (*t)["name"].value_or(node.id);
            node.x = static_cast<float>((*t)["x"].value_or(0.0));
            node.y = static_cast<float>((*t)["y"].value_or(0.0));

            if (const auto* values = (*t)["values"].as_table()) {
                for (const auto& [key, value] : *values) {
                    node.values[std::string(key.str())] = read_values(value);
                }
            }
            if (const auto* properties = (*t)["properties"].as_table()) {
                for (const auto& [key, value] : *properties) {
                    node.properties[std::string(key.str())] = value.value_or(std::string());
                }
            }
            if (node.id.empty() || node.type.empty()) {
                out_diags.push_back(error("a node is missing its id or type", path.string()));
                continue;
            }
            out.nodes.push_back(std::move(node));
        }
    }

    if (const auto* links = table["links"].as_array()) {
        for (const auto& element : *links) {
            const auto* t = element.as_table();
            if (!t) continue;
            Link link;
            link.from_node = (*t)["from_node"].value_or(std::string());
            link.from_pin = (*t)["from_pin"].value_or(std::string());
            link.to_node = (*t)["to_node"].value_or(std::string());
            link.to_pin = (*t)["to_pin"].value_or(std::string());
            out.links.push_back(std::move(link));
        }
    }

    // Project-local custom code nodes travel with the graph, so a graph file is
    // self-contained and can be shared without its library.
    if (const auto* customs = table["custom_nodes"].as_array()) {
        for (const auto& element : *customs) {
            const auto* t = element.as_table();
            if (!t) continue;
            NodeDef def;
            def.type = (*t)["type"].value_or(std::string());
            def.label = (*t)["label"].value_or(def.type);
            def.expression = (*t)["expression"].value_or(std::string());
            def.documentation = (*t)["documentation"].value_or(std::string());
            def.category = NodeCategory::Custom;
            def.generic = (*t)["generic"].value_or(true);

            auto read_pins = [&](const char* key, std::vector<PinDef>& pins) {
                const auto* array = (*t)[key].as_array();
                if (!array) return;
                for (const auto& pin_node : *array) {
                    const auto* pin_table = pin_node.as_table();
                    if (!pin_table) continue;
                    PinDef pin;
                    pin.name = (*pin_table)["name"].value_or(std::string());
                    pin.type = pin_type_from_string(
                                   (*pin_table)["type"].value_or(std::string("float")))
                                   .value_or(PinType::Float);
                    if (const auto* value = (*pin_table)["default"].as_array()) {
                        for (const auto& v : *value) pin.default_value.push_back(v.value_or(0.0));
                    }
                    pins.push_back(std::move(pin));
                }
            };
            read_pins("inputs", def.inputs);
            read_pins("outputs", def.outputs);
            if (!def.type.empty()) out.custom_nodes.push_back(std::move(def));
        }
    }

    Diagnostics validation = validate_graph(out);
    out_diags.insert(out_diags.end(), validation.begin(), validation.end());
    return !has_errors(validation);
}

bool save_graph(const std::filesystem::path& path, const Graph& graph, Diagnostics& out_diags) {
    std::ostringstream os;
    os << "# Node graph for " << graph.shader_id << ".\n"
       << "# The shader source is generated from this file unless 'detached' is true.\n\n";

    os << "[graph]\n";
    os << "shader = \"" << escape(graph.shader_id) << "\"\n";
    os << "stage = \"" << to_string(graph.stage) << "\"\n";
    os << "language = \"" << to_string(graph.language) << "\"\n";
    os << "coercion = \"" << to_string(graph.coercion) << "\"\n";
    os << "formatted = " << (graph.formatted_output ? "true" : "false") << "\n";
    os << "detached = " << (graph.detached ? "true" : "false") << "\n";

    for (const auto& node : graph.nodes) {
        os << "\n[[nodes]]\n";
        os << "id = \"" << escape(node.id) << "\"\n";
        os << "type = \"" << escape(node.type) << "\"\n";
        os << "name = \"" << escape(node.name) << "\"\n";
        os << "x = " << node.x << "\n";
        os << "y = " << node.y << "\n";

        if (!node.values.empty()) {
            os << "\n[nodes.values]\n";
            for (const auto& [key, values] : node.values) {
                os << "\"" << escape(key) << "\" = [";
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if (i) os << ", ";
                    os << values[i];
                }
                os << "]\n";
            }
        }
        if (!node.properties.empty()) {
            os << "\n[nodes.properties]\n";
            for (const auto& [key, value] : node.properties) {
                os << "\"" << escape(key) << "\" = \"" << escape(value) << "\"\n";
            }
        }
    }

    for (const auto& link : graph.links) {
        os << "\n[[links]]\n";
        os << "from_node = \"" << escape(link.from_node) << "\"\n";
        os << "from_pin = \"" << escape(link.from_pin) << "\"\n";
        os << "to_node = \"" << escape(link.to_node) << "\"\n";
        os << "to_pin = \"" << escape(link.to_pin) << "\"\n";
    }

    for (const auto& def : graph.custom_nodes) {
        os << "\n[[custom_nodes]]\n";
        os << "type = \"" << escape(def.type) << "\"\n";
        os << "label = \"" << escape(def.label) << "\"\n";
        os << "expression = \"" << escape(def.expression) << "\"\n";
        if (!def.documentation.empty()) {
            os << "documentation = \"" << escape(def.documentation) << "\"\n";
        }
        os << "generic = " << (def.generic ? "true" : "false") << "\n";

        auto write_pins = [&](const char* key, const std::vector<PinDef>& pins) {
            for (const auto& pin : pins) {
                os << "\n[[custom_nodes." << key << "]]\n";
                os << "name = \"" << escape(pin.name) << "\"\n";
                os << "type = \"" << to_string(pin.type) << "\"\n";
                if (!pin.default_value.empty()) {
                    os << "default = [";
                    for (std::size_t i = 0; i < pin.default_value.size(); ++i) {
                        if (i) os << ", ";
                        os << pin.default_value[i];
                    }
                    os << "]\n";
                }
            }
        };
        write_pins("inputs", def.inputs);
        write_pins("outputs", def.outputs);
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Write to a temporary file and rename, so an interrupted save cannot leave a
    // half-written graph that fails to parse next time.
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            out_diags.push_back(error("cannot write " + temp.string()));
            return false;
        }
        const std::string text = os.str();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        out_diags.push_back(error("cannot replace " + path.string()));
        return false;
    }
    return true;
}

}  // namespace ssstudio
