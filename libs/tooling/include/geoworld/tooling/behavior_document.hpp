#pragma once

#include "geoworld/tooling/artifact.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace geoworld::tooling {

inline constexpr std::uint32_t behavior_document_schema_version = 1;
inline constexpr std::uint32_t behavior_artifact_format_version = 1;
inline constexpr std::uint32_t behavior_compiler_version = 2;

struct EditorPosition {
    double x{};
    double y{};
};

struct BehaviorNodeDocument {
    std::string id;
    std::string type;
    std::string name;
    EditorPosition position;
    std::vector<std::string> children;
    std::map<std::string, std::string> parameters;
};

struct BehaviorTreeDocument {
    std::uint32_t schema_version{behavior_document_schema_version};
    std::string tree_id;
    std::string root;
    std::vector<BehaviorNodeDocument> nodes;
};

enum class BehaviorNodeArity {
    control,
    decorator,
    leaf
};

struct BehaviorNodeDescriptor {
    std::string type;
    std::string display_name;
    BehaviorNodeArity arity{BehaviorNodeArity::leaf};
};

class BehaviorNodeRegistry {
public:
    [[nodiscard]] static BehaviorNodeRegistry defaults();

    [[nodiscard]] bool register_node(BehaviorNodeDescriptor descriptor);
    [[nodiscard]] const BehaviorNodeDescriptor* find(std::string_view type) const;
    [[nodiscard]] const std::vector<BehaviorNodeDescriptor>& descriptors() const noexcept;

private:
    std::vector<BehaviorNodeDescriptor> descriptors_;
};

struct HfsmStateDocument {
    std::string id;
    std::string name;
    std::string parent;
    EditorPosition position;
};

struct HfsmTransitionDocument {
    std::string source;
    std::string event;
    std::string target;
    int priority{};
};

struct HfsmDocument {
    std::uint32_t schema_version{behavior_document_schema_version};
    std::string machine_id;
    std::string initial_state;
    std::vector<HfsmStateDocument> states;
    std::vector<HfsmTransitionDocument> transitions;
};

enum class DiagnosticSeverity { error, warning };

struct DocumentDiagnostic {
    std::string code;
    DiagnosticSeverity severity{DiagnosticSeverity::error};
    std::string object_id;
    std::string message;
};

struct DocumentValidation {
    std::vector<DocumentDiagnostic> diagnostics;

    [[nodiscard]] bool valid() const noexcept;
};

struct DocumentParseResult {
    DocumentValidation validation;
    BehaviorTreeDocument behavior_tree;
    HfsmDocument hfsm;
};

struct CompiledBehaviorArtifact {
    ArtifactHeader header;
    ArtifactKind kind{ArtifactKind::behavior_tree};
    std::string document_id;
    std::string entry_id;
    std::string runtime_xml;
    BehaviorTreeDocument behavior_tree;
    HfsmDocument hfsm;
};

[[nodiscard]] DocumentParseResult parse_behavior_tree_json(std::string_view source,
                                                            std::string_view schema_source);
[[nodiscard]] DocumentParseResult parse_hfsm_json(std::string_view source,
                                                   std::string_view schema_source);
[[nodiscard]] DocumentParseResult parse_behavior_tree_json(std::string_view source);
[[nodiscard]] DocumentParseResult parse_hfsm_json(std::string_view source);
[[nodiscard]] DocumentValidation validate(const BehaviorTreeDocument& document);
[[nodiscard]] DocumentValidation validate(const BehaviorTreeDocument& document,
                                          const BehaviorNodeRegistry& registry);
[[nodiscard]] DocumentValidation validate(const HfsmDocument& document);
[[nodiscard]] std::string canonical_json(const BehaviorTreeDocument& document);
[[nodiscard]] std::string canonical_json(const HfsmDocument& document);
[[nodiscard]] std::string export_dot(const BehaviorTreeDocument& document);
[[nodiscard]] std::string export_dot(const HfsmDocument& document);
[[nodiscard]] DocumentValidation apply_graphviz_layout(BehaviorTreeDocument& document,
                                                       std::string_view engine = "dot");
[[nodiscard]] DocumentValidation apply_graphviz_layout(HfsmDocument& document,
                                                       std::string_view engine = "dot");
[[nodiscard]] std::string export_behavior_tree_xml(const BehaviorTreeDocument& document);
[[nodiscard]] std::vector<std::uint8_t> compile(const BehaviorTreeDocument& document);
[[nodiscard]] std::vector<std::uint8_t> compile(const HfsmDocument& document);
[[nodiscard]] DocumentValidation load_behavior_artifact(std::span<const std::uint8_t> bytes,
                                                        ArtifactKind expected_kind,
                                                        CompiledBehaviorArtifact& artifact);

} // namespace geoworld::tooling
