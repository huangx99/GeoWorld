#include "geoworld/simulation/tick.hpp"
#include "geoworld/debug/state_hash.hpp"
#include "geoworld/observability/logger.hpp"

#if GW_HAS_ECS_RUNTIME
#include "geoworld/ecs/runtime.hpp"
#endif

#include <iostream>

int main() {
    geoworld::world::World world;
    geoworld::simulation::CommandBuffer commands;
    geoworld::simulation::TickClock clock;

    geoworld::world::WorldObject object;
    object.id = geoworld::foundation::WorldId{1};
    object.semantic_type = "core.example";
    object.lifecycle = geoworld::world::LifecycleState::active;

    static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{object}));
    const auto report = commands.apply(world, 0);
    clock.advance();

    std::size_t runtime_entities = 0;
#if GW_HAS_ECS_RUNTIME
    geoworld::ecs::Runtime ecs;
    const auto* created = world.find(object.id);
    if (created != nullptr) {
        static_cast<void>(ecs.activate(*created));
    }
    runtime_entities = ecs.size();
#endif

    geoworld::observability::Logger logger;
    logger.write(geoworld::observability::LogLevel::info, "runtime", "world started",
                 geoworld::observability::LogContext{static_cast<std::uint64_t>(clock.tick()), object.id});

    std::cout << "geoworldd: tick=" << clock.tick()
              << " objects=" << world.size()
              << " runtime_entities=" << runtime_entities
              << " applied=" << report.applied
              << " hash=" << geoworld::debug::world_state_hash(world) << '\n';
    return 0;
}
