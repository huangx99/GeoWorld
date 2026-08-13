#include "geoworld/runtime/world_runtime.hpp"

#include "geoworld/ecs/schema.hpp"
#include "geoworld/world/schema.hpp"

#include <chrono>
#include <cstddef>
#include <utility>

namespace geoworld::runtime {

WorldRuntime::WorldRuntime() {
    world::register_world_schemas(schemas_);
    ecs::register_ecs_schemas(schemas_);
}

std::uint64_t WorldRuntime::submit(std::uint64_t target_tick,
                                   simulation::CommandPayload payload) {
    return commands_.enqueue(target_tick, std::move(payload));
}

std::uint64_t WorldRuntime::submit(std::uint64_t target_tick,
                                   simulation::CommandPayload payload,
                                   simulation::CommandMeta meta) {
    return commands_.enqueue(target_tick, std::move(payload), meta);
}

StepResult WorldRuntime::step() {
    const auto processed_tick = static_cast<std::uint64_t>(clock_.tick());
    const auto phase_start = std::chrono::steady_clock::now();
    const auto report = commands_.apply(world_, processed_tick);
    const auto input_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - phase_start).count();

    const auto run_phase = [this, processed_tick](TickPhase phase) {
        const auto index = static_cast<std::size_t>(phase);
        for (auto& callback : callbacks_[index]) {
            callback(world_, commands_, processed_tick);
        }
    };

    const auto track_a_start = std::chrono::steady_clock::now();
    run_phase(TickPhase::track_a);
    const auto track_a_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - track_a_start).count();

    const auto track_b_start = std::chrono::steady_clock::now();
    run_phase(TickPhase::track_b);
    if (!ai_callbacks_.empty()) {
        const auto snapshot = ai::DecisionSnapshot::capture(world_, processed_tick);
        for (auto& callback : ai_callbacks_) {
            callback(snapshot, ai_intents_);
        }
        static_cast<void>(ai_intents_.flush(processed_tick, commands_));
    }
    const auto track_b_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - track_b_start).count();

    const auto track_c_start = std::chrono::steady_clock::now();
    run_phase(TickPhase::track_c);
    const auto track_c_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - track_c_start).count();

    const auto hash = debug::world_state_hash(world_);
    for (auto& observer : projection_observers_) {
        observer(world_, processed_tick, hash, spatial_query_);
    }
    run_phase(TickPhase::projection);
    clock_.advance();
    const auto total_elapsed = input_elapsed + track_a_elapsed + track_b_elapsed + track_c_elapsed;

    metrics_.record({
        processed_tick,
        static_cast<std::uint64_t>(total_elapsed),
        static_cast<std::uint64_t>(track_a_elapsed),
        static_cast<std::uint64_t>(track_b_elapsed),
        static_cast<std::uint64_t>(track_c_elapsed),
        report.applied,
        report.rejected,
        0,
        false
    });
    return {processed_tick, report, hash};
}

void WorldRuntime::add_phase_callback(TickPhase phase, PhaseCallback callback) {
    if (callback) {
        callbacks_[static_cast<std::size_t>(phase)].push_back(std::move(callback));
    }
}

void WorldRuntime::add_ai_decision_callback(AiDecisionCallback callback) {
    if (callback) {
        ai_callbacks_.push_back(std::move(callback));
    }
}

void WorldRuntime::add_projection_observer(ProjectionObserver observer) {
    if (observer) {
        projection_observers_.push_back(std::move(observer));
    }
}

void WorldRuntime::set_spatial_query(const spatial::SpatialQuery* query) noexcept {
    spatial_query_ = query;
}

bool WorldRuntime::activate(foundation::WorldId id) {
    const auto* object = world_.find(id);
    return object != nullptr && ecs_.activate(*object).has_value();
}

bool WorldRuntime::deactivate(foundation::RuntimeId id) { return ecs_.destroy(id); }

const world::World& WorldRuntime::world() const noexcept { return world_; }
const ecs::Runtime& WorldRuntime::ecs() const noexcept { return ecs_; }
const schema::SchemaRegistry& WorldRuntime::schemas() const noexcept { return schemas_; }
const observability::MetricsRecorder& WorldRuntime::metrics() const noexcept { return metrics_; }
const simulation::TickClock& WorldRuntime::clock() const noexcept { return clock_; }

} // namespace geoworld::runtime
