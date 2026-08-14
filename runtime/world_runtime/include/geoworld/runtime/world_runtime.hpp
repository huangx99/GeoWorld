#pragma once

#include "geoworld/ai/decision.hpp"
#include "geoworld/debug/state_hash.hpp"
#include "geoworld/ecs/runtime.hpp"
#include "geoworld/foundation/random.hpp"
#include "geoworld/observability/metrics.hpp"
#include "geoworld/rules/event_bus.hpp"
#include "geoworld/schema/schema_registry.hpp"
#include "geoworld/simulation/command_buffer.hpp"
#include "geoworld/simulation/tick.hpp"
#include "geoworld/spatial/spatial_query.hpp"
#include "geoworld/tooling/artifact.hpp"

#include <cstdint>
#include <array>
#include <functional>
#include <string_view>
#include <vector>

namespace geoworld::runtime {

struct StepResult {
    std::uint64_t tick{};
    simulation::ApplyReport commands;
    std::uint64_t state_hash{};
};

enum class TickPhase { input, track_a, track_b, track_c, projection };
using PhaseCallback = std::function<void(world::World&, simulation::CommandBuffer&, std::uint64_t)>;
using AiDecisionCallback = std::function<void(const ai::DecisionSnapshot&,
                                               ai::DecisionIntentBuffer&)>;
// 专用只读投影观察入口：Track A/B/C 完成、状态 hash 已计算、clock.advance 之前触发。
// 投影不依赖可写 phase callback 的注册顺序，也不允许经此入口修改世界。
using ProjectionObserver = std::function<void(const world::World&, std::uint64_t,
                                              std::uint64_t,
                                              const spatial::SpatialQuery*)>;

// M5 稳定 tick 边界检查点锚点：Track A/B/C 完成、状态 hash 已计算之后、
// clock_.advance 之前触发。回调只读；completed_tick 即刚完成的 tick，
// resume_tick = completed_tick + 1。included_lsn 由 WAL 侧在回调内查询。
using CheckpointAnchorCallback = std::function<void(std::uint64_t completed_tick,
                                                    std::uint64_t state_hash)>;

class WorldRuntime {
public:
    WorldRuntime();

    [[nodiscard]] std::uint64_t submit(std::uint64_t target_tick,
                                       simulation::CommandPayload payload);
    [[nodiscard]] std::uint64_t submit(std::uint64_t target_tick,
                                       simulation::CommandPayload payload,
                                       simulation::CommandMeta meta);
    [[nodiscard]] StepResult step();
    void add_phase_callback(TickPhase phase, PhaseCallback callback);
    void add_ai_decision_callback(AiDecisionCallback callback);
    void add_projection_observer(ProjectionObserver observer);
    void add_checkpoint_anchor_callback(CheckpointAnchorCallback callback);
    // 本运行时持有的权威有状态模块 ID；durable 启动要求每个 ID 都有已注册的
    // CheckpointProvider，否则必须启动失败（docs/M5.md 恢复状态模型）。
    [[nodiscard]] static const std::vector<std::string_view>& authoritative_state_modules();
    // M5 恢复专用入口：仅供恢复编排在开放服务前重建权威状态。
    [[nodiscard]] world::World& world_for_restore() noexcept { return world_; }
    [[nodiscard]] simulation::TickClock& clock_for_restore() noexcept { return clock_; }
    [[nodiscard]] simulation::CommandBuffer& commands_for_restore() noexcept { return commands_; }
    [[nodiscard]] rules::EventBus& events_for_restore() noexcept { return events_; }
    [[nodiscard]] ai::DecisionIntentBuffer& ai_intents_for_restore() noexcept {
        return ai_intents_;
    }
    [[nodiscard]] foundation::NamedRandomStreams& random_streams_for_restore() noexcept {
        return random_streams_;
    }
    [[nodiscard]] tooling::ArtifactManifest& artifacts_for_restore() noexcept {
        return artifacts_;
    }
    [[nodiscard]] ecs::Runtime& ecs_for_restore() noexcept { return ecs_; }
    void set_spatial_query(const spatial::SpatialQuery* query) noexcept;
    [[nodiscard]] bool activate(foundation::WorldId id);
    [[nodiscard]] bool deactivate(foundation::RuntimeId id);

    [[nodiscard]] const world::World& world() const noexcept;
    [[nodiscard]] const ecs::Runtime& ecs() const noexcept;
    [[nodiscard]] const schema::SchemaRegistry& schemas() const noexcept;
    [[nodiscard]] const observability::MetricsRecorder& metrics() const noexcept;
    [[nodiscard]] const simulation::TickClock& clock() const noexcept;

private:
    world::World world_;
    ecs::Runtime ecs_;
    schema::SchemaRegistry schemas_;
    simulation::CommandBuffer commands_;
    simulation::TickClock clock_;
    rules::EventBus events_;
    observability::MetricsRecorder metrics_;
    std::array<std::vector<PhaseCallback>, 5> callbacks_;
    std::vector<AiDecisionCallback> ai_callbacks_;
    std::vector<ProjectionObserver> projection_observers_;
    std::vector<CheckpointAnchorCallback> checkpoint_anchor_callbacks_;
    const spatial::SpatialQuery* spatial_query_{};
    ai::DecisionIntentBuffer ai_intents_;
    foundation::NamedRandomStreams random_streams_;
    tooling::ArtifactManifest artifacts_;
};

} // namespace geoworld::runtime
