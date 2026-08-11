#include "geoworld/debug/state_hash.hpp"
#include "geoworld/simulation/command_buffer.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>

int main() {
    constexpr std::uint64_t entity_count = 10'000;
    geoworld::world::World world;
    geoworld::simulation::CommandBuffer commands;

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t index = 1; index <= entity_count; ++index) {
        geoworld::world::WorldObject object;
        object.id = geoworld::foundation::WorldId{index};
        object.semantic_type = "benchmark.entity";
        static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{
            std::move(object)
        }));
    }
    const auto report = commands.apply(world, 0);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "m1.create_apply_count=" << report.applied
              << " elapsed_us=" << elapsed
              << " state_hash=" << geoworld::debug::world_state_hash(world) << '\n';
    return report.applied == entity_count ? 0 : 1;
}
