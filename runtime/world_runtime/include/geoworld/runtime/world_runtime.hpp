#pragma once

#include "geoworld/ai/decision.hpp"
#include "geoworld/debug/state_hash.hpp"
#include "geoworld/ecs/runtime.hpp"
#include "geoworld/observability/metrics.hpp"
#include "geoworld/schema/schema_registry.hpp"
#include "geoworld/simulation/command_buffer.hpp"
#include "geoworld/simulation/tick.hpp"

#include <cstdint>
#include <array>
#include <functional>
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

class WorldRuntime {
public:
    WorldRuntime();

    [[nodiscard]] std::uint64_t submit(std::uint64_t target_tick,
                                       simulation::CommandPayload payload);
    [[nodiscard]] StepResult step();
    void add_phase_callback(TickPhase phase, PhaseCallback callback);
    void add_ai_decision_callback(AiDecisionCallback callback);
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
    observability::MetricsRecorder metrics_;
    std::array<std::vector<PhaseCallback>, 5> callbacks_;
    std::vector<AiDecisionCallback> ai_callbacks_;
    ai::DecisionIntentBuffer ai_intents_;
};

} // namespace geoworld::runtime
