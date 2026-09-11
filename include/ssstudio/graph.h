// Node graph documents.
//
// A graph generates shader source; it never compiles anything itself. That keeps
// the whole downstream pipeline (compile, reflect, pack, docs, loader) unaware
// that graphs exist, and it makes graph code generation testable without a GPU.
//
// The generated file is marked read-only in the text editor until the user
// detaches, at which point the graph becomes a snapshot and the text is
// authoritative (see Graph::detached).
#ifndef SSSTUDIO_GRAPH_H
#define SSSTUDIO_GRAPH_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ssstudio/types.h"

namespace ssstudio {

// ---------------------------------------------------------------------------
// Pin types
// ---------------------------------------------------------------------------
enum class PinType : std::uint8_t {
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    Bool,
    Matrix4,
    Texture2D,
    Sampler,
    Any,  // custom-code pins and generic math nodes resolve this at validation
};

std::string_view to_string(PinType t);
std::optional<PinType> pin_type_from_string(std::string_view s);
std::uint32_t component_count(PinType t);
PinType vector_of(std::uint32_t components);

/// Which component a name selects: x/r is 0 through w/a is 3. Empty when the
/// name is not one, so a graph file carrying a typo is rejected rather than
/// silently read as x. Shared by the node library, code generation, validation
/// and the inspector, so all four agree on what "y" means.
std::optional<std::uint32_t> component_index(std::string_view name);

/// The canonical spelling of a component index, always the xyzw set: it is the
/// one both dialects accept on every vector type, colour or not.
std::string_view component_name(std::uint32_t index);

/// Why `spelling` is not a usable swizzle of a value with `available`
/// components, or empty when it is one. One function rather than a bool so the
/// graph can say which of the four ways it is wrong, since "invalid swizzle" is
/// exactly the message that leaves someone guessing.
std::string swizzle_error(std::string_view spelling, std::uint32_t available);

/// A swizzle rewritten into the xyzw naming set. Only meaningful for a spelling
/// swizzle_error has already accepted.
std::string swizzle_xyzw(std::string_view spelling);

/// A node's rectangle on the canvas. The editor owns the sizes - how wide a node
/// is drawn is a question about drawing - so they are passed in rather than
/// worked out here.
struct NodeRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

/// Moves `candidate` to the nearest position, on a `step` grid, where it comes
/// no closer than `gap` to anything in `occupied`. Leaves it exactly where it is
/// when that position is already free.
///
/// Nearest rather than "the next slot along": a node has to appear where it was
/// asked for, and only give way as far as it must. When nothing within a
/// sensible radius is free the position is left alone, because a node that
/// vanishes to the far side of the graph is worse than one that overlaps.
void place_without_overlap(const std::vector<NodeRect>& occupied, NodeRect& candidate,
                           float step, float gap);

struct Graph;
struct Node;
struct NodeDef;

/// What type a pin actually carries, once a generic `Any` has been resolved from
/// whatever is wired into it. Used by validation, by code generation and by the
/// inspector - all three need the answer and none of them can work it out from
/// the pin alone.
PinType resolved_pin_type(const Graph& graph, const Node& node, const NodeDef& def,
                          const std::string& pin_name, bool is_output);

// HLSL and GLSL spellings of a pin type.
std::string pin_type_name(PinType t, Language language);

// How strictly a link may change type. Configurable because a permissive graph
// is pleasant to sketch with and a strict one catches mistakes.
enum class Coercion : std::uint8_t {
    Strict,    // types must match exactly
    Widening,  // float -> float2/3/4 splat, smaller vector -> larger with zeros
    Loose,     // any numeric to any numeric, truncating
};

std::string_view to_string(Coercion c);
std::optional<Coercion> coercion_from_string(std::string_view s);

// ---------------------------------------------------------------------------
// Node definitions
// ---------------------------------------------------------------------------
struct PinDef {
    std::string name;
    PinType type = PinType::Float;
    std::vector<double> default_value;  // used when an input pin is unconnected
};

enum class NodeCategory : std::uint8_t {
    Input,
    Math,
    Utility,
    Color,
    Control,
    Custom,
    Output,
};

std::string_view to_string(NodeCategory c);

struct NodeDef {
    std::string type;       // stable identifier, e.g. "math.lerp"
    std::string label;      // display name
    NodeCategory category = NodeCategory::Math;
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    // Expression template. $0, $1 ... are input expressions; $T is the resolved
    // generic type name; $self is the node's variable name.
    std::string expression;
    std::string documentation;
    bool generic = false;  // inputs typed Any resolve from what is connected
};

// The built-in library. Registered once and shared; a project may add custom
// code nodes on top (see Graph::custom_nodes).
const std::vector<NodeDef>& builtin_nodes();
const NodeDef* find_node_def(const std::string& type);

// ---------------------------------------------------------------------------
// Graph documents
// ---------------------------------------------------------------------------
struct Node {
    std::string id;    // unique within the graph
    std::string type;  // NodeDef::type
    std::string name;  // user label, becomes a comment in generated source
    float x = 0.0f;
    float y = 0.0f;

    // Literal values for unconnected inputs, by pin name.
    std::map<std::string, std::vector<double>> values;
    // Free-form settings: uniform block/member names, texture ids, custom code.
    std::map<std::string, std::string> properties;
};

struct Link {
    std::string from_node;
    std::string from_pin;
    std::string to_node;
    std::string to_pin;
};

struct Graph {
    std::string shader_id;             // the shader this graph generates
    Stage stage = Stage::Fragment;
    Language language = Language::HLSL;
    bool detached = false;             // text took over; graph is a snapshot
    Coercion coercion = Coercion::Widening;
    bool formatted_output = true;      // false emits compact source

    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<NodeDef> custom_nodes;  // project-local code nodes

    const Node* find_node(const std::string& id) const;
    Node* find_node(const std::string& id);
    // Every link that feeds `node.pin`, or null when nothing is connected.
    const Link* incoming(const std::string& node_id, const std::string& pin) const;

    static Graph create_default(std::string shader_id, Stage stage, Language language);
};

// Resolves node definitions including the graph's custom nodes.
const NodeDef* resolve_node_def(const Graph& graph, const std::string& type);

// ---------------------------------------------------------------------------
// Validation and generation
// ---------------------------------------------------------------------------
// Reports cycles, unknown node types, dangling links, missing outputs and type
// mismatches under the graph's coercion rule.
Diagnostics validate_graph(const Graph& graph);

// Evaluation order: inputs before the nodes that consume them. Empty when the
// graph contains a cycle.
std::vector<const Node*> topological_order(const Graph& graph, Diagnostics& out_diags);

struct GraphCodegenOptions {
    std::string generated_by = "SDL Shader Studio";
    bool include_node_comments = true;
    std::string entry_point = "main";
};

// Generates complete, compilable shader source for graph.language. The output is
// deliberately readable: a detached graph should leave behind source a person is
// happy to keep editing by hand.
std::string generate_graph_source(const Graph& graph, const GraphCodegenOptions& options,
                                  Diagnostics& out_diags);

// The uniforms/textures a graph declares, so the I/O panel can list them
// alongside reflected ones before the first successful compile.
struct GraphInput {
    std::string name;
    PinType type = PinType::Float;
    bool is_texture = false;
    std::vector<double> default_value;
};
std::vector<GraphInput> graph_inputs(const Graph& graph);

// TOML persistence (graphs/<shader id>.toml), implemented in graph_io.cpp.
bool load_graph(const std::filesystem::path& path, Graph& out, Diagnostics& out_diags);
bool save_graph(const std::filesystem::path& path, const Graph& graph, Diagnostics& out_diags);

}  // namespace ssstudio

#endif  // SSSTUDIO_GRAPH_H
