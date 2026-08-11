#include "geoworld/debug/state_hash.hpp"
#include "geoworld/foundation/random.hpp"
#include "geoworld/simulation/command_buffer.hpp"

#include <cstdint>
#include <utility>

namespace {

std::uint64_t run_scenario() {
    geoworld::world::World world;
    geoworld::simulation::CommandBuffer commands;
    geoworld::foundation::DeterministicRng random{20260811};

    for (std::uint64_t index = 1; index <= 32; ++index) {
        geoworld::world::WorldObject object;
        object.id = geoworld::foundation::WorldId{index};
        object.semantic_type = "test.agent";
        object.position = {random.next_unit(), random.next_unit(), random.next_unit()};
        object.state["energy"] = static_cast<std::int64_t>(random.next_u64() % 100);
        static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{
            std::move(object)
        }));
    }

    const auto report = commands.apply(world, 0);
    return report.applied == 32 ? geoworld::debug::world_state_hash(world) : 0;
}

} // namespace

int main() {
    const auto first = run_scenario();
    const auto second = run_scenario();
    return first != 0 && first == second ? 0 : 1;
}
