#include "geoworld/runtime/world_runtime.hpp"

#include <vector>

int main() {
    geoworld::runtime::WorldRuntime runtime;
    std::vector<geoworld::runtime::TickPhase> phases;
    runtime.add_phase_callback(geoworld::runtime::TickPhase::track_a,
        [&phases](auto&, auto&, auto) { phases.push_back(geoworld::runtime::TickPhase::track_a); });
    runtime.add_phase_callback(geoworld::runtime::TickPhase::track_b,
        [&phases](auto&, auto&, auto) { phases.push_back(geoworld::runtime::TickPhase::track_b); });
    runtime.add_phase_callback(geoworld::runtime::TickPhase::track_c,
        [&phases](auto&, auto&, auto) { phases.push_back(geoworld::runtime::TickPhase::track_c); });
    runtime.add_ai_decision_callback([](const auto& snapshot, auto& intents) {
        if (snapshot.find({500}) != nullptr) {
            static_cast<void>(intents.submit(snapshot.tick(), {{500}, "decision", true}));
        }
    });
    geoworld::world::WorldObject object;
    object.id = geoworld::foundation::WorldId{500};
    object.semantic_type = "test.runtime";

    static_cast<void>(runtime.submit(0, geoworld::simulation::CreateObjectCommand{object}));
    const auto result = runtime.step();
    if (result.tick != 0 || result.commands.applied != 1 || runtime.clock().tick() != 1) {
        return 1;
    }
    if (phases != std::vector<geoworld::runtime::TickPhase>{
            geoworld::runtime::TickPhase::track_a,
            geoworld::runtime::TickPhase::track_b,
            geoworld::runtime::TickPhase::track_c}) {
        return 1;
    }
    if (runtime.schemas().size() != 5 || runtime.world().size() != 1) {
        return 1;
    }
    if (!runtime.activate(object.id) || runtime.ecs().size() != 1) {
        return 1;
    }
    const auto next = runtime.step();
    const auto* decided = runtime.world().find(object.id);
    if (next.commands.applied != 1 || decided == nullptr
        || std::get<bool>(decided->properties.at("decision")) != true) {
        return 1;
    }
    const auto metrics = runtime.metrics().latest();
    return metrics.has_value() && metrics->tick == 1 && result.state_hash != 0
        && metrics->total_microseconds >= metrics->track_a_microseconds
        && metrics->total_microseconds >= metrics->track_b_microseconds
        && metrics->total_microseconds >= metrics->track_c_microseconds ? 0 : 1;
}
