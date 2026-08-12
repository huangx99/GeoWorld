#include "geoworld/tooling/behavior_document.hpp"

#include "behavior_artifact_generated.h"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace geoworld::tooling {
namespace {

using Json = nlohmann::json;

constexpr std::string_view diagnostic_json_parse = "GWTD001";
constexpr std::string_view diagnostic_json_schema = "GWTD002";
constexpr std::string_view diagnostic_schema_version = "GWTD003";
constexpr std::string_view diagnostic_duplicate_id = "GWTD101";
constexpr std::string_view diagnostic_missing_reference = "GWTD102";
constexpr std::string_view diagnostic_cycle = "GWTD103";
constexpr std::string_view diagnostic_unreachable = "GWTD104";
constexpr std::string_view diagnostic_node_arity = "GWTD105";
constexpr std::string_view diagnostic_node_type = "GWTD106";
constexpr std::string_view diagnostic_duplicate_edge = "GWTD107";
constexpr std::string_view diagnostic_artifact = "GWTD201";

void add_error(DocumentValidation& validation, std::string_view code, std::string object_id,
               std::string message) {
    validation.diagnostics.push_back({std::string{code}, DiagnosticSeverity::error,
                                      std::move(object_id), std::move(message)});
}

void add_warning(DocumentValidation& validation, std::string_view code, std::string object_id,
                 std::string message) {
    validation.diagnostics.push_back({std::string{code}, DiagnosticSeverity::warning,
                                      std::move(object_id), std::move(message)});
}

std::string escape_dot(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto character : value) {
        if (character == '"' || character == '\\') escaped.push_back('\\');
        if (character == '\n' || character == '\r') {
            escaped += "\\n";
        } else {
            escaped.push_back(character);
        }
    }
    return escaped;
}

std::string escape_xml(std::string_view value) {
    std::string escaped;
    for (const auto character : value) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

class SchemaErrorHandler final : public nlohmann::json_schema::error_handler {
public:
    void error(const Json::json_pointer& pointer, const Json&, const std::string& message) override {
        errors.emplace_back(pointer.to_string() + ": " + message);
    }

    std::vector<std::string> errors;
};

DocumentValidation validate_json_schema(const Json& instance, std::string_view schema_source) {
    DocumentValidation validation;
    try {
        auto schema = Json::parse(schema_source);
        nlohmann::json_schema::json_validator validator{std::move(schema)};
        SchemaErrorHandler handler;
        static_cast<void>(validator.validate(instance, handler));
        for (auto& error : handler.errors) {
            add_error(validation, diagnostic_json_schema, {}, std::move(error));
        }
    } catch (const std::exception& exception) {
        add_error(validation, diagnostic_json_schema, {}, exception.what());
    }
    return validation;
}

EditorPosition parse_position(const Json& object) {
    const auto& position = object.at("position");
    return {position.at("x").get<double>(), position.at("y").get<double>()};
}

Json position_json(EditorPosition position) {
    return Json{{"x", position.x}, {"y", position.y}};
}

void append(DocumentValidation& target, DocumentValidation source) {
    target.diagnostics.insert(target.diagnostics.end(),
                              std::make_move_iterator(source.diagnostics.begin()),
                              std::make_move_iterator(source.diagnostics.end()));
}

template <typename IdRange, typename EdgeFunction>
std::unordered_set<std::string> reachable_from(std::string_view root, const IdRange& ids,
                                                EdgeFunction edges) {
    std::unordered_set<std::string> known{ids.begin(), ids.end()};
    std::unordered_set<std::string> reached;
    std::vector<std::string> pending{std::string{root}};
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (!known.contains(current) || !reached.insert(current).second) continue;
        for (const auto& target : edges(current)) pending.push_back(target);
    }
    return reached;
}

std::string behavior_xml_node(const BehaviorTreeDocument& document, std::string_view id,
                              std::unordered_set<std::string>& stack, int depth) {
    const auto iterator = std::find_if(document.nodes.begin(), document.nodes.end(), [id](const auto& node) {
        return node.id == id;
    });
    if (iterator == document.nodes.end() || !stack.insert(iterator->id).second) return {};
    std::ostringstream output;
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    output << indent << '<' << iterator->type << " name=\"" << escape_xml(iterator->name) << '"';
    for (const auto& [key, value] : iterator->parameters) {
        output << ' ' << key << "=\"" << escape_xml(value) << '"';
    }
    if (iterator->children.empty()) {
        output << "/>\n";
    } else {
        output << ">\n";
        for (const auto& child : iterator->children) {
            output << behavior_xml_node(document, child, stack, depth + 1);
        }
        output << indent << "</" << iterator->type << ">\n";
    }
    stack.erase(iterator->id);
    return output.str();
}

geoworld::tooling::fb::ArtifactKind flatbuffer_kind(ArtifactKind kind) {
    return kind == ArtifactKind::behavior_tree
        ? geoworld::tooling::fb::ArtifactKind::BehaviorTree
        : geoworld::tooling::fb::ArtifactKind::StateMachine;
}

ArtifactKind public_kind(geoworld::tooling::fb::ArtifactKind kind) {
    return kind == geoworld::tooling::fb::ArtifactKind::BehaviorTree
        ? ArtifactKind::behavior_tree : ArtifactKind::state_machine;
}

std::vector<std::uint8_t> compile_document(const BehaviorTreeDocument* behavior_tree,
                                           const HfsmDocument* hfsm) {
    const auto kind = behavior_tree != nullptr ? ArtifactKind::behavior_tree : ArtifactKind::state_machine;
    const auto source = behavior_tree != nullptr ? canonical_json(*behavior_tree) : canonical_json(*hfsm);
    const auto header = make_header(kind, behavior_document_schema_version, source);
    flatbuffers::FlatBufferBuilder builder;

    std::vector<flatbuffers::Offset<fb::BehaviorNode>> behavior_nodes;
    std::vector<flatbuffers::Offset<fb::HfsmState>> hfsm_states;
    std::vector<flatbuffers::Offset<fb::HfsmTransition>> hfsm_transitions;
    std::string document_id;
    std::string entry_id;
    std::string runtime_xml;
    if (behavior_tree != nullptr) {
        document_id = behavior_tree->tree_id;
        entry_id = behavior_tree->root;
        runtime_xml = export_behavior_tree_xml(*behavior_tree);
        behavior_nodes.reserve(behavior_tree->nodes.size());
        for (const auto& node : behavior_tree->nodes) {
            std::vector<flatbuffers::Offset<flatbuffers::String>> children;
            for (const auto& child : node.children) children.push_back(builder.CreateString(child));
            std::vector<flatbuffers::Offset<fb::Parameter>> parameters;
            for (const auto& [key, value] : node.parameters) {
                parameters.push_back(fb::CreateParameter(builder, builder.CreateString(key),
                                                         builder.CreateString(value)));
            }
            const auto position = fb::CreatePosition(builder, node.position.x, node.position.y);
            behavior_nodes.push_back(fb::CreateBehaviorNode(
                builder, builder.CreateString(node.id), builder.CreateString(node.type),
                builder.CreateString(node.name), position, builder.CreateVector(children),
                builder.CreateVector(parameters)));
        }
    } else {
        document_id = hfsm->machine_id;
        entry_id = hfsm->initial_state;
        hfsm_states.reserve(hfsm->states.size());
        for (const auto& state : hfsm->states) {
            hfsm_states.push_back(fb::CreateHfsmState(
                builder, builder.CreateString(state.id), builder.CreateString(state.name),
                builder.CreateString(state.parent),
                fb::CreatePosition(builder, state.position.x, state.position.y)));
        }
        hfsm_transitions.reserve(hfsm->transitions.size());
        for (const auto& transition : hfsm->transitions) {
            hfsm_transitions.push_back(fb::CreateHfsmTransition(
                builder, builder.CreateString(transition.source), builder.CreateString(transition.event),
                builder.CreateString(transition.target), transition.priority));
        }
    }

    const auto artifact = fb::CreateBehaviorArtifact(
        builder, behavior_artifact_format_version, behavior_document_schema_version,
        behavior_compiler_version, flatbuffer_kind(kind), builder.CreateString(header.source_hash),
        builder.CreateString(document_id), builder.CreateString(entry_id),
        builder.CreateVector(behavior_nodes), builder.CreateVector(hfsm_states),
        builder.CreateVector(hfsm_transitions), builder.CreateString(runtime_xml));
    fb::FinishBehaviorArtifactBuffer(builder, artifact);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

} // namespace

BehaviorNodeRegistry BehaviorNodeRegistry::defaults() {
    BehaviorNodeRegistry registry;
    const auto add = [&registry](std::string type, std::string display_name, BehaviorNodeArity arity) {
        static_cast<void>(registry.register_node(
            {std::move(type), std::move(display_name), arity}));
    };
    add("Sequence", "顺序", BehaviorNodeArity::control);
    add("SequenceWithMemory", "记忆顺序", BehaviorNodeArity::control);
    add("ReactiveSequence", "响应顺序", BehaviorNodeArity::control);
    add("Fallback", "回退", BehaviorNodeArity::control);
    add("ReactiveFallback", "响应回退", BehaviorNodeArity::control);
    add("Parallel", "并行", BehaviorNodeArity::control);
    add("ParallelAll", "全部并行", BehaviorNodeArity::control);
    add("RetryUntilSuccessful", "重试直至成功", BehaviorNodeArity::decorator);
    add("Repeat", "重复", BehaviorNodeArity::decorator);
    add("Inverter", "结果取反", BehaviorNodeArity::decorator);
    add("ForceSuccess", "强制成功", BehaviorNodeArity::decorator);
    add("ForceFailure", "强制失败", BehaviorNodeArity::decorator);
    add("Delay", "延迟", BehaviorNodeArity::decorator);
    add("Timeout", "超时", BehaviorNodeArity::decorator);
    add("AlwaysSuccess", "始终成功", BehaviorNodeArity::leaf);
    add("AlwaysFailure", "始终失败", BehaviorNodeArity::leaf);
    add("Script", "脚本动作", BehaviorNodeArity::leaf);
    add("ScriptCondition", "脚本条件", BehaviorNodeArity::leaf);
    add("Sleep", "等待", BehaviorNodeArity::leaf);
    add("SetBlackboard", "设置黑板", BehaviorNodeArity::leaf);
    add("UnsetBlackboard", "清除黑板", BehaviorNodeArity::leaf);
    return registry;
}

bool BehaviorNodeRegistry::register_node(BehaviorNodeDescriptor descriptor) {
    if (descriptor.type.empty() || descriptor.display_name.empty() || find(descriptor.type) != nullptr) {
        return false;
    }
    descriptors_.push_back(std::move(descriptor));
    return true;
}

const BehaviorNodeDescriptor* BehaviorNodeRegistry::find(std::string_view type) const {
    const auto iterator = std::find_if(descriptors_.begin(), descriptors_.end(), [type](const auto& item) {
        return item.type == type;
    });
    return iterator == descriptors_.end() ? nullptr : &*iterator;
}

const std::vector<BehaviorNodeDescriptor>& BehaviorNodeRegistry::descriptors() const noexcept {
    return descriptors_;
}

bool DocumentValidation::valid() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error;
    });
}

DocumentParseResult parse_behavior_tree_json(std::string_view source, std::string_view schema_source) {
    DocumentParseResult result;
    try {
        const auto json = Json::parse(source);
        append(result.validation, validate_json_schema(json, schema_source));
        if (!result.validation.valid()) return result;
        result.behavior_tree.schema_version = json.at("schema_version").get<std::uint32_t>();
        result.behavior_tree.tree_id = json.at("tree_id").get<std::string>();
        result.behavior_tree.root = json.at("root").get<std::string>();
        for (const auto& item : json.at("nodes")) {
            BehaviorNodeDocument node;
            node.id = item.at("id").get<std::string>();
            node.type = item.at("type").get<std::string>();
            node.name = item.at("name").get<std::string>();
            node.position = parse_position(item);
            node.children = item.at("children").get<std::vector<std::string>>();
            node.parameters = item.at("parameters").get<std::map<std::string, std::string>>();
            result.behavior_tree.nodes.push_back(std::move(node));
        }
        append(result.validation, validate(result.behavior_tree));
    } catch (const std::exception& exception) {
        add_error(result.validation, diagnostic_json_parse, {}, exception.what());
    }
    return result;
}

DocumentParseResult parse_hfsm_json(std::string_view source, std::string_view schema_source) {
    DocumentParseResult result;
    try {
        const auto json = Json::parse(source);
        append(result.validation, validate_json_schema(json, schema_source));
        if (!result.validation.valid()) return result;
        result.hfsm.schema_version = json.at("schema_version").get<std::uint32_t>();
        result.hfsm.machine_id = json.at("machine_id").get<std::string>();
        result.hfsm.initial_state = json.at("initial_state").get<std::string>();
        for (const auto& item : json.at("states")) {
            result.hfsm.states.push_back({item.at("id").get<std::string>(),
                item.at("name").get<std::string>(), item.at("parent").get<std::string>(),
                parse_position(item)});
        }
        for (const auto& item : json.at("transitions")) {
            result.hfsm.transitions.push_back({item.at("source").get<std::string>(),
                item.at("event").get<std::string>(), item.at("target").get<std::string>(),
                item.at("priority").get<int>()});
        }
        append(result.validation, validate(result.hfsm));
    } catch (const std::exception& exception) {
        add_error(result.validation, diagnostic_json_parse, {}, exception.what());
    }
    return result;
}

DocumentParseResult parse_behavior_tree_json(std::string_view source) {
    DocumentParseResult result;
    try {
        const auto json = Json::parse(source);
        result.behavior_tree.schema_version = json.at("schema_version").get<std::uint32_t>();
        result.behavior_tree.tree_id = json.at("tree_id").get<std::string>();
        result.behavior_tree.root = json.at("root").get<std::string>();
        for (const auto& item : json.at("nodes")) {
            BehaviorNodeDocument node;
            node.id = item.at("id").get<std::string>();
            node.type = item.at("type").get<std::string>();
            node.name = item.at("name").get<std::string>();
            node.position = parse_position(item);
            node.children = item.at("children").get<std::vector<std::string>>();
            node.parameters = item.at("parameters").get<std::map<std::string, std::string>>();
            result.behavior_tree.nodes.push_back(std::move(node));
        }
        append(result.validation, validate(result.behavior_tree));
    } catch (const std::exception& exception) {
        add_error(result.validation, diagnostic_json_parse, {}, exception.what());
    }
    return result;
}

DocumentParseResult parse_hfsm_json(std::string_view source) {
    DocumentParseResult result;
    try {
        const auto json = Json::parse(source);
        result.hfsm.schema_version = json.at("schema_version").get<std::uint32_t>();
        result.hfsm.machine_id = json.at("machine_id").get<std::string>();
        result.hfsm.initial_state = json.at("initial_state").get<std::string>();
        for (const auto& item : json.at("states")) {
            result.hfsm.states.push_back({item.at("id").get<std::string>(),
                item.at("name").get<std::string>(), item.at("parent").get<std::string>(),
                parse_position(item)});
        }
        for (const auto& item : json.at("transitions")) {
            result.hfsm.transitions.push_back({item.at("source").get<std::string>(),
                item.at("event").get<std::string>(), item.at("target").get<std::string>(),
                item.at("priority").get<int>()});
        }
        append(result.validation, validate(result.hfsm));
    } catch (const std::exception& exception) {
        add_error(result.validation, diagnostic_json_parse, {}, exception.what());
    }
    return result;
}

DocumentValidation validate(const BehaviorTreeDocument& document) {
    return validate(document, BehaviorNodeRegistry::defaults());
}

DocumentValidation validate(const BehaviorTreeDocument& document,
                            const BehaviorNodeRegistry& registry) {
    DocumentValidation validation;
    if (document.schema_version != behavior_document_schema_version) {
        add_error(validation, diagnostic_schema_version, document.tree_id, "不支持的行为树文档版本");
    }
    std::unordered_map<std::string, const BehaviorNodeDocument*> nodes;
    for (const auto& node : document.nodes) {
        if (!nodes.emplace(node.id, &node).second) {
            add_error(validation, diagnostic_duplicate_id, node.id, "行为树节点 ID 重复");
        }
        const auto* descriptor = registry.find(node.type);
        if (descriptor == nullptr) {
            add_error(validation, diagnostic_node_type, node.id, "未知的 BehaviorTree.CPP 节点类型: " + node.type);
        } else if (descriptor->arity == BehaviorNodeArity::control && node.children.empty()) {
            add_error(validation, diagnostic_node_arity, node.id, "控制节点至少需要一个子节点");
        } else if (descriptor->arity == BehaviorNodeArity::decorator && node.children.size() != 1) {
            add_error(validation, diagnostic_node_arity, node.id, "装饰节点必须且只能有一个子节点");
        } else if (descriptor->arity == BehaviorNodeArity::leaf && !node.children.empty()) {
            add_error(validation, diagnostic_node_arity, node.id, "叶节点不能包含子节点");
        }
        std::unordered_set<std::string> unique_children;
        for (const auto& child : node.children) {
            if (!unique_children.insert(child).second) {
                add_error(validation, diagnostic_duplicate_edge, node.id, "同一父节点重复引用子节点: " + child);
            }
        }
    }
    if (!nodes.contains(document.root)) {
        add_error(validation, diagnostic_missing_reference, document.root, "根节点不存在");
    }
    std::unordered_map<std::string, std::size_t> parent_counts;
    for (const auto& node : document.nodes) {
        for (const auto& child : node.children) {
            if (!nodes.contains(child)) {
                add_error(validation, diagnostic_missing_reference, node.id, "子节点不存在: " + child);
            } else if (++parent_counts[child] > 1) {
                add_error(validation, diagnostic_duplicate_edge, child, "行为树节点不能拥有多个父节点");
            }
        }
    }
    std::unordered_map<std::string, int> colors;
    std::function<void(std::string_view)> visit = [&](std::string_view id) {
        auto& color = colors[std::string{id}];
        if (color == 1) {
            add_error(validation, diagnostic_cycle, std::string{id}, "行为树存在环路");
            return;
        }
        if (color == 2 || !nodes.contains(std::string{id})) return;
        color = 1;
        for (const auto& child : nodes.at(std::string{id})->children) visit(child);
        color = 2;
    };
    visit(document.root);
    std::vector<std::string> ids;
    for (const auto& node : document.nodes) ids.push_back(node.id);
    const auto reached = reachable_from(document.root, ids, [&nodes](const std::string& id) {
        return nodes.at(id)->children;
    });
    for (const auto& node : document.nodes) {
        if (!reached.contains(node.id)) {
            add_warning(validation, diagnostic_unreachable, node.id, "节点无法从根节点到达");
        }
    }
    return validation;
}

DocumentValidation validate(const HfsmDocument& document) {
    DocumentValidation validation;
    if (document.schema_version != behavior_document_schema_version) {
        add_error(validation, diagnostic_schema_version, document.machine_id, "不支持的 HFSM 文档版本");
    }
    std::unordered_map<std::string, const HfsmStateDocument*> states;
    for (const auto& state : document.states) {
        if (!states.emplace(state.id, &state).second) {
            add_error(validation, diagnostic_duplicate_id, state.id, "HFSM 状态 ID 重复");
        }
    }
    if (!states.contains(document.initial_state)) {
        add_error(validation, diagnostic_missing_reference, document.initial_state, "初始状态不存在");
    }
    for (const auto& state : document.states) {
        if (!state.parent.empty() && !states.contains(state.parent)) {
            add_error(validation, diagnostic_missing_reference, state.id, "父状态不存在: " + state.parent);
        }
        std::unordered_set<std::string> ancestors;
        auto parent = state.parent;
        while (!parent.empty() && states.contains(parent)) {
            if (!ancestors.insert(parent).second || parent == state.id) {
                add_error(validation, diagnostic_cycle, state.id, "HFSM 父状态层级存在环路");
                break;
            }
            parent = states.at(parent)->parent;
        }
    }
    std::set<std::tuple<std::string, std::string, int>> transition_keys;
    for (const auto& transition : document.transitions) {
        if (!states.contains(transition.source)) {
            add_error(validation, diagnostic_missing_reference, transition.source, "转换源状态不存在");
        }
        if (!states.contains(transition.target)) {
            add_error(validation, diagnostic_missing_reference, transition.target, "转换目标状态不存在");
        }
        if (!transition_keys.emplace(transition.source, transition.event, transition.priority).second) {
            add_error(validation, diagnostic_duplicate_edge, transition.source,
                      "同一状态、事件和优先级存在多个转换");
        }
    }
    std::vector<std::string> ids;
    for (const auto& state : document.states) ids.push_back(state.id);
    const auto reached = reachable_from(document.initial_state, ids, [&document, &states](const std::string& id) {
        std::vector<std::string> targets;
        std::unordered_set<std::string> ancestors;
        auto candidate = id;
        while (!candidate.empty() && states.contains(candidate) && ancestors.insert(candidate).second) {
            for (const auto& transition : document.transitions) {
                if (transition.source == candidate) targets.push_back(transition.target);
            }
            candidate = states.at(candidate)->parent;
        }
        return targets;
    });
    for (const auto& state : document.states) {
        const auto contains_reached_descendant = std::any_of(reached.begin(), reached.end(), [&state, &states](const auto& id) {
            std::unordered_set<std::string> ancestors;
            auto candidate = id;
            while (!candidate.empty() && states.contains(candidate) && ancestors.insert(candidate).second) {
                if (candidate == state.id) return true;
                candidate = states.at(candidate)->parent;
            }
            return false;
        });
        if (!reached.contains(state.id) && !contains_reached_descendant) {
            add_warning(validation, diagnostic_unreachable, state.id, "状态无法从初始状态到达");
        }
    }
    return validation;
}

std::string canonical_json(const BehaviorTreeDocument& document) {
    Json nodes = Json::array();
    for (const auto& node : document.nodes) {
        nodes.push_back({{"id", node.id}, {"type", node.type}, {"name", node.name},
                         {"position", position_json(node.position)}, {"children", node.children},
                         {"parameters", node.parameters}});
    }
    return Json{{"schema_version", document.schema_version}, {"tree_id", document.tree_id},
                {"root", document.root}, {"nodes", std::move(nodes)}}.dump();
}

std::string canonical_json(const HfsmDocument& document) {
    Json states = Json::array();
    for (const auto& state : document.states) {
        states.push_back({{"id", state.id}, {"name", state.name}, {"parent", state.parent},
                          {"position", position_json(state.position)}});
    }
    Json transitions = Json::array();
    for (const auto& transition : document.transitions) {
        transitions.push_back({{"source", transition.source}, {"event", transition.event},
                               {"target", transition.target}, {"priority", transition.priority}});
    }
    return Json{{"schema_version", document.schema_version}, {"machine_id", document.machine_id},
                {"initial_state", document.initial_state}, {"states", std::move(states)},
                {"transitions", std::move(transitions)}}.dump();
}

std::string export_dot(const BehaviorTreeDocument& document) {
    std::ostringstream output;
    output << "digraph \"" << escape_dot(document.tree_id) << "\" {\n"
           << "  graph [rankdir=TB];\n  node [shape=box];\n";
    for (const auto& node : document.nodes) {
        output << "  \"" << escape_dot(node.id) << "\" [label=\"" << escape_dot(node.name)
               << "\\n" << escape_dot(node.type) << "\", pos=\"" << node.position.x << ','
               << -node.position.y << "!\"] ;\n";
    }
    for (const auto& node : document.nodes) {
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            output << "  \"" << escape_dot(node.id) << "\" -> \""
                   << escape_dot(node.children[index]) << "\" [label=\"" << index << "\"];\n";
        }
    }
    output << "}\n";
    return output.str();
}

std::string export_dot(const HfsmDocument& document) {
    std::ostringstream output;
    output << "digraph \"" << escape_dot(document.machine_id) << "\" {\n"
           << "  graph [rankdir=LR];\n  node [shape=ellipse];\n"
           << "  \"__initial\" [shape=point];\n  \"__initial\" -> \""
           << escape_dot(document.initial_state) << "\";\n";
    for (const auto& state : document.states) {
        output << "  \"" << escape_dot(state.id) << "\" [label=\"" << escape_dot(state.name)
               << "\", pos=\"" << state.position.x << ',' << -state.position.y << "!\"] ;\n";
        if (!state.parent.empty()) {
            output << "  \"" << escape_dot(state.parent) << "\" -> \"" << escape_dot(state.id)
                   << "\" [style=dashed, arrowhead=none, label=\"父状态\"];\n";
        }
    }
    for (const auto& transition : document.transitions) {
        output << "  \"" << escape_dot(transition.source) << "\" -> \""
               << escape_dot(transition.target) << "\" [label=\"" << escape_dot(transition.event)
               << " / " << transition.priority << "\"];\n";
    }
    output << "}\n";
    return output.str();
}

std::string export_behavior_tree_xml(const BehaviorTreeDocument& document) {
    std::unordered_set<std::string> stack;
    std::ostringstream output;
    output << "<root BTCPP_format=\"4\" main_tree_to_execute=\"" << escape_xml(document.tree_id)
           << "\">\n  <BehaviorTree ID=\"" << escape_xml(document.tree_id) << "\">\n"
           << behavior_xml_node(document, document.root, stack, 2)
           << "  </BehaviorTree>\n</root>\n";
    return output.str();
}

std::vector<std::uint8_t> compile(const BehaviorTreeDocument& document) {
    return validate(document).valid() ? compile_document(&document, nullptr) : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> compile(const HfsmDocument& document) {
    return validate(document).valid() ? compile_document(nullptr, &document) : std::vector<std::uint8_t>{};
}

DocumentValidation load_behavior_artifact(std::span<const std::uint8_t> bytes,
                                          ArtifactKind expected_kind,
                                          CompiledBehaviorArtifact& artifact) {
    DocumentValidation validation;
    if (bytes.empty() || !fb::BehaviorArtifactBufferHasIdentifier(bytes.data())
        || !flatbuffers::Verifier{bytes.data(), bytes.size()}.VerifyBuffer<fb::BehaviorArtifact>("GWBA")) {
        add_error(validation, diagnostic_artifact, {}, "制品不是有效的 GeoWorld 行为制品");
        return validation;
    }
    const auto* source = fb::GetBehaviorArtifact(bytes.data());
    if (source->format_version() != behavior_artifact_format_version
        || source->schema_version() != behavior_document_schema_version
        || source->compiler_version() != behavior_compiler_version
        || public_kind(source->kind()) != expected_kind) {
        add_error(validation, diagnostic_artifact, {}, "制品版本或类型不兼容");
        return validation;
    }
    artifact = {};
    artifact.kind = expected_kind;
    artifact.document_id = source->document_id()->str();
    artifact.entry_id = source->entry_id()->str();
    artifact.runtime_xml = source->runtime_xml()->str();
    artifact.header = {std::string{artifact_type(expected_kind)}, source->schema_version(),
                       source->source_hash()->str(), source->compiler_version()};
    if (expected_kind == ArtifactKind::behavior_tree) {
        artifact.behavior_tree.schema_version = source->schema_version();
        artifact.behavior_tree.tree_id = artifact.document_id;
        artifact.behavior_tree.root = artifact.entry_id;
        for (const auto* item : *source->behavior_nodes()) {
            BehaviorNodeDocument node;
            node.id = item->id()->str();
            node.type = item->type()->str();
            node.name = item->name()->str();
            node.position = {item->position()->x(), item->position()->y()};
            for (const auto* child : *item->children()) node.children.push_back(child->str());
            for (const auto* parameter : *item->parameters()) {
                node.parameters.emplace(parameter->key()->str(), parameter->value()->str());
            }
            artifact.behavior_tree.nodes.push_back(std::move(node));
        }
        append(validation, validate(artifact.behavior_tree));
        if (artifact.runtime_xml != export_behavior_tree_xml(artifact.behavior_tree)) {
            add_error(validation, diagnostic_artifact, artifact.document_id, "制品中的运行时 XML 与节点模型不一致");
        }
        if (artifact.header.source_hash != source_hash(canonical_json(artifact.behavior_tree))) {
            add_error(validation, diagnostic_artifact, artifact.document_id, "制品 source hash 校验失败");
        }
    } else {
        artifact.hfsm.schema_version = source->schema_version();
        artifact.hfsm.machine_id = artifact.document_id;
        artifact.hfsm.initial_state = artifact.entry_id;
        for (const auto* item : *source->hfsm_states()) {
            artifact.hfsm.states.push_back({item->id()->str(), item->name()->str(), item->parent()->str(),
                                            {item->position()->x(), item->position()->y()}});
        }
        for (const auto* item : *source->hfsm_transitions()) {
            artifact.hfsm.transitions.push_back({item->source()->str(), item->event()->str(),
                                                 item->target()->str(), item->priority()});
        }
        append(validation, validate(artifact.hfsm));
        if (artifact.header.source_hash != source_hash(canonical_json(artifact.hfsm))) {
            add_error(validation, diagnostic_artifact, artifact.document_id, "制品 source hash 校验失败");
        }
    }
    return validation;
}

} // namespace geoworld::tooling
