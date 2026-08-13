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
    if (before == geoworld::debug::world_state_hash(world)) {
        return 1;
    }

    // 外部命令 metadata：版本前置检查在 apply 时执行，逐命令结果可回执。
    using geoworld::simulation::CommandMeta;
    using geoworld::simulation::CommandRejectReason;
    const std::uint64_t current_version = world.find(WorldId{11})->version;
    static_cast<void>(commands.enqueue(3, geoworld::simulation::SetPropertyCommand{
        WorldId{11}, "speed", 20.0
    }, CommandMeta{7, current_version + 1}));
    auto report = commands.apply(world, 3);
    if (report.rejected != 1 || report.outcomes.size() != 1
        || report.outcomes[0].reason != CommandRejectReason::version_conflict
        || report.outcomes[0].ingress_sequence != 7) {
        return 1;
    }
    static_cast<void>(commands.enqueue(3, geoworld::simulation::SetPropertyCommand{
        WorldId{99}, "speed", 1.0
    }, CommandMeta{8, 1}));
    report = commands.apply(world, 3);
    if (report.rejected != 1
        || report.outcomes[0].reason != CommandRejectReason::missing_object) {
        return 1;
    }
    static_cast<void>(commands.enqueue(3, geoworld::simulation::SetPropertyCommand{
        WorldId{11}, "speed", 21.0
    }, CommandMeta{9, current_version}));
    report = commands.apply(world, 3);
    if (report.applied != 1 || !report.outcomes[0].applied
        || report.outcomes[0].reason != CommandRejectReason::none) {
        return 1;
    }
    return 0;
}
