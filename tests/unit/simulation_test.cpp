#include "geoworld/simulation/tick.hpp"
#include "geoworld/debug/state_hash.hpp"

int main() {
    using geoworld::foundation::WorldId;

    geoworld::simulation::TickClock clock;
    if (clock.dt_microseconds() != 20'000 || clock.simulation_time_microseconds() != 0) {
        return 1;
    }
    clock.advance();
    if (clock.tick() != 1 || clock.simulation_time_microseconds() != 20'000) {
        return 1;
    }

    geoworld::world::World world;
    geoworld::simulation::CommandBuffer commands;
    geoworld::world::WorldObject object;
    object.id = WorldId{11};
    object.semantic_type = "test.entity";

    static_cast<void>(commands.enqueue(2, geoworld::simulation::CreateObjectCommand{object}));
    if (commands.apply(world, 1).deferred != 1 || world.size() != 0) {
        return 1;
    }
    if (commands.apply(world, 2).applied != 1 || world.size() != 1) {
        return 1;
    }

    const auto before = geoworld::debug::world_state_hash(world);
    static_cast<void>(commands.enqueue(2, geoworld::simulation::SetPropertyCommand{
        WorldId{11}, "speed", 12.5
    }));
    if (commands.apply(world, 2).applied != 1) {
        return 1;
    }
    return before != geoworld::debug::world_state_hash(world) ? 0 : 1;
}
