#include "geoworld/tooling/artifact.hpp"
#include "geoworld/tooling/behavior_document.hpp"

#include <algorithm>
#include <string>

namespace {

bool has_code(const geoworld::tooling::DocumentValidation& validation, std::string_view code) {
    return std::any_of(validation.diagnostics.begin(), validation.diagnostics.end(), [code](const auto& item) {
        return item.code == code;
    });
}

} // namespace

int main() {
    using geoworld::tooling::ArtifactKind;
    const auto first = geoworld::tooling::make_header(ArtifactKind::behavior_tree, 1, "<tree/>");
    const auto second = geoworld::tooling::make_header(ArtifactKind::behavior_tree, 1, "<tree/>");
    if (!first.valid() || first.source_hash != second.source_hash
        || !geoworld::tooling::validate(first, ArtifactKind::behavior_tree).valid
        || geoworld::tooling::validate(first, ArtifactKind::rule).valid
        || geoworld::tooling::validate(first, ArtifactKind::behavior_tree, 2).valid) {
        return 1;
    }
    auto invalid_header = first;
    invalid_header.compiler_version = 0;
    if (geoworld::tooling::validate(invalid_header, ArtifactKind::behavior_tree).valid) return 1;

#if GW_HAS_BEHAVIOR_TOOLING
    geoworld::tooling::BehaviorTreeDocument tree;
    tree.tree_id = "Response";
    tree.root = "sequence";
    tree.nodes = {
        {"sequence", "Sequence", "处置", {100.0, 50.0}, {"success"}, {}},
        {"success", "AlwaysSuccess", "完成", {100.0, 160.0}, {}, {}}
    };
    const auto tree_validation = geoworld::tooling::validate(tree);
    const auto tree_json = geoworld::tooling::canonical_json(tree);
    const auto tree_dot = geoworld::tooling::export_dot(tree);
    const auto tree_xml = geoworld::tooling::export_behavior_tree_xml(tree);
    const auto tree_artifact_a = geoworld::tooling::compile(tree);
    const auto tree_artifact_b = geoworld::tooling::compile(tree);
    if (!tree_validation.valid() || tree_json.empty() || tree_dot.find("sequence") == std::string::npos
        || tree_xml.find("AlwaysSuccess") == std::string::npos || tree_artifact_a.empty()
        || tree_artifact_a != tree_artifact_b) {
        return 1;
    }
#if GW_HAS_GRAPHVIZ
    auto laid_out_tree = tree;
    const auto layout_validation = geoworld::tooling::apply_graphviz_layout(laid_out_tree);
    auto laid_out_tree_again = tree;
    const auto second_layout_validation = geoworld::tooling::apply_graphviz_layout(laid_out_tree_again);
    if (!layout_validation.valid()
        || !second_layout_validation.valid()
        || geoworld::tooling::canonical_json(laid_out_tree)
            != geoworld::tooling::canonical_json(laid_out_tree_again)
        || (laid_out_tree.nodes.front().position.x == tree.nodes.front().position.x
            && laid_out_tree.nodes.front().position.y == tree.nodes.front().position.y)) {
        return 1;
    }
#endif
    geoworld::tooling::CompiledBehaviorArtifact loaded_tree;
    if (!geoworld::tooling::load_behavior_artifact(
            tree_artifact_a, ArtifactKind::behavior_tree, loaded_tree).valid()
        || loaded_tree.behavior_tree.nodes.size() != 2 || loaded_tree.runtime_xml != tree_xml) {
        return 1;
    }
    auto corrupt = tree_artifact_a;
    corrupt[corrupt.size() / 2] ^= 0x5a;
    geoworld::tooling::CompiledBehaviorArtifact rejected;
    if (geoworld::tooling::load_behavior_artifact(corrupt, ArtifactKind::behavior_tree, rejected).valid()) {
        return 1;
    }

    auto duplicate = tree;
    duplicate.nodes.push_back(duplicate.nodes.front());
    if (!has_code(geoworld::tooling::validate(duplicate), "GWTD101")) return 1;
    auto missing = tree;
    missing.nodes.front().children = {"missing"};
    if (!has_code(geoworld::tooling::validate(missing), "GWTD102")) return 1;
    auto cycle = tree;
    cycle.nodes.back().type = "Sequence";
    cycle.nodes.back().children = {"sequence"};
    if (!has_code(geoworld::tooling::validate(cycle), "GWTD103")) return 1;
    auto unreachable = tree;
    unreachable.nodes.push_back({"orphan", "AlwaysFailure", "孤立", {}, {}, {}});
    if (!has_code(geoworld::tooling::validate(unreachable), "GWTD104")) return 1;
    auto registry = geoworld::tooling::BehaviorNodeRegistry::defaults();
    if (registry.find("Sequence") == nullptr
        || !registry.register_node({"DomainAction", "领域动作",
                                     geoworld::tooling::BehaviorNodeArity::leaf})
        || registry.register_node({"DomainAction", "重复领域动作",
                                   geoworld::tooling::BehaviorNodeArity::leaf})) {
        return 1;
    }
    auto custom_tree = tree;
    custom_tree.nodes.back().type = "DomainAction";
    if (!geoworld::tooling::validate(custom_tree, registry).valid()
        || geoworld::tooling::validate(custom_tree).valid()) {
        return 1;
    }

    geoworld::tooling::HfsmDocument hfsm;
    hfsm.machine_id = "Agent";
    hfsm.initial_state = "normal/idle";
    hfsm.states = {
        {"normal", "正常", "", {0.0, 0.0}},
        {"normal/idle", "待机", "normal", {0.0, 100.0}},
        {"alert", "告警", "", {300.0, 0.0}}
    };
    hfsm.transitions = {{"normal", "alarm", "alert", 100}};
    const auto hfsm_artifact = geoworld::tooling::compile(hfsm);
    geoworld::tooling::CompiledBehaviorArtifact loaded_hfsm;
    if (!geoworld::tooling::validate(hfsm).valid() || hfsm_artifact.empty()
        || !geoworld::tooling::load_behavior_artifact(
            hfsm_artifact, ArtifactKind::state_machine, loaded_hfsm).valid()
        || loaded_hfsm.hfsm.states.size() != 3) {
        return 1;
    }
    auto parent_cycle = hfsm;
    parent_cycle.states.front().parent = "normal/idle";
    if (!has_code(geoworld::tooling::validate(parent_cycle), "GWTD103")) return 1;
    auto bad_transition = hfsm;
    bad_transition.transitions.front().target = "missing";
    if (!has_code(geoworld::tooling::validate(bad_transition), "GWTD102")) return 1;
#endif
    return 0;
}
