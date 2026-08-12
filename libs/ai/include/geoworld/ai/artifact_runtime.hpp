#pragma once

#include "geoworld/ai/bt_runtime.hpp"
#include "geoworld/ai/hfsm.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace geoworld::ai {

struct ArtifactLoadResult {
    bool valid{};
    std::string message;
};

[[nodiscard]] ArtifactLoadResult load_behavior_tree_artifact(
    std::span<const std::uint8_t> bytes, std::optional<BehaviorTreeRuntime>& runtime);
[[nodiscard]] ArtifactLoadResult load_hfsm_artifact(
    std::span<const std::uint8_t> bytes, HierarchicalStateMachine& runtime);

} // namespace geoworld::ai
