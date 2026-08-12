#include "geoworld/ai/artifact_runtime.hpp"

#include "geoworld/tooling/behavior_document.hpp"

#include <algorithm>
#include <unordered_set>

namespace geoworld::ai {

ArtifactLoadResult load_behavior_tree_artifact(std::span<const std::uint8_t> bytes,
                                               std::optional<BehaviorTreeRuntime>& runtime) {
#if GW_HAS_BEHAVIOR_TOOLING
    tooling::CompiledBehaviorArtifact artifact;
    const auto validation = tooling::load_behavior_artifact(
        bytes, tooling::ArtifactKind::behavior_tree, artifact);
    if (!validation.valid()) {
        return {false, validation.diagnostics.empty() ? "行为树制品校验失败"
                                                       : validation.diagnostics.front().message};
    }
    runtime.emplace(artifact.runtime_xml);
    return runtime->valid() ? ArtifactLoadResult{true, {}}
                            : ArtifactLoadResult{false, "BehaviorTree.CPP 拒绝加载制品 XML"};
#else
    static_cast<void>(bytes);
    static_cast<void>(runtime);
    return {false, "当前构建未启用 M3 行为工具链依赖"};
#endif
}

ArtifactLoadResult load_hfsm_artifact(std::span<const std::uint8_t> bytes,
                                      HierarchicalStateMachine& runtime) {
#if GW_HAS_BEHAVIOR_TOOLING
    tooling::CompiledBehaviorArtifact artifact;
    const auto validation = tooling::load_behavior_artifact(
        bytes, tooling::ArtifactKind::state_machine, artifact);
    if (!validation.valid()) {
        return {false, validation.diagnostics.empty() ? "HFSM 制品校验失败"
                                                       : validation.diagnostics.front().message};
    }
    HierarchicalStateMachine loaded;
    std::unordered_set<std::string> defined;
    std::vector<const tooling::HfsmStateDocument*> pending;
    pending.reserve(artifact.hfsm.states.size());
    for (const auto& state : artifact.hfsm.states) pending.push_back(&state);
    while (!pending.empty()) {
        const auto before = pending.size();
        std::erase_if(pending, [&loaded, &defined](const auto* state) {
            if (!state->parent.empty() && !defined.contains(state->parent)) return false;
            if (!loaded.define_state({state->id, state->parent})) return false;
            defined.insert(state->id);
            return true;
        });
        if (pending.size() == before) {
            return {false, "HFSM 状态层级无法构建: " + pending.front()->id};
        }
    }
    for (const auto& transition : artifact.hfsm.transitions) {
        if (!loaded.add_transition({transition.source,
                                    {transition.event, transition.target, transition.priority}})) {
            return {false, "HFSM 转换无法构建: " + transition.source + "/" + transition.event};
        }
    }
    if (!loaded.set_initial(artifact.hfsm.initial_state)) {
        return {false, "HFSM 初始状态无法设置"};
    }
    runtime = std::move(loaded);
    return {true, {}};
#else
    static_cast<void>(bytes);
    static_cast<void>(runtime);
    return {false, "当前构建未启用 M3 行为工具链依赖"};
#endif
}

} // namespace geoworld::ai
