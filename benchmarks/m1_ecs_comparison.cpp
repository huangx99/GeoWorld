#include "geoworld/ecs/runtime.hpp"

#include <entt/entt.hpp>
#include <flecs.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Position {
    double x;
    double y;
    double z;
};

struct Identity {
    std::uint64_t value;
};

struct SemanticType {
    std::string value;
};

struct Velocity {
    double x;
    double y;
    double z;
};

template <typename Function>
std::int64_t elapsed_microseconds(Function&& function) {
    const auto start = std::chrono::steady_clock::now();
    function();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main() {
    constexpr std::uint64_t count = 100'000;

    geoworld::ecs::Runtime flecs_runtime;
    const auto flecs_create = elapsed_microseconds([&] {
        for (std::uint64_t index = 1; index <= count; ++index) {
            geoworld::world::WorldObject object;
            object.id = geoworld::foundation::WorldId{index};
            object.semantic_type = "benchmark.entity";
            object.position = {static_cast<double>(index), 0.0, 0.0};
            static_cast<void>(flecs_runtime.activate(object));
        }
    });
    const auto flecs_iterate = elapsed_microseconds([&] {
        static_cast<void>(flecs_runtime.snapshot());
    });

    entt::registry registry;
    const auto entt_create = elapsed_microseconds([&] {
        for (std::uint64_t index = 1; index <= count; ++index) {
            const auto entity = registry.create();
            registry.emplace<Identity>(entity, index);
            registry.emplace<Position>(entity, static_cast<double>(index), 0.0, 0.0);
            registry.emplace<SemanticType>(entity, "benchmark.entity");
        }
    });
    std::uint64_t visited = 0;
    std::vector<geoworld::ecs::RuntimeEntitySnapshot> entt_snapshot;
    entt_snapshot.reserve(count);
    const auto entt_iterate = elapsed_microseconds([&] {
        registry.view<Identity, Position, SemanticType>().each(
            [&visited, &entt_snapshot](const Identity& identity,
                                      const Position& position,
                                      const SemanticType& semantic_type) {
            ++visited;
            entt_snapshot.push_back({
                geoworld::foundation::WorldId{identity.value},
                {position.x, position.y, position.z},
                semantic_type.value
            });
        });
        std::sort(entt_snapshot.begin(), entt_snapshot.end(), [](const auto& left, const auto& right) {
            return left.world_id < right.world_id;
        });
    });

    std::cout << "entities=" << count
              << " flecs_create_us=" << flecs_create
              << " flecs_snapshot_us=" << flecs_iterate
              << " entt_create_us=" << entt_create
              << " entt_snapshot_us=" << entt_iterate
              << " visited=" << visited << '\n';

    flecs::world flecs_system_world;
    for (std::uint64_t index = 1; index <= count; ++index) {
        flecs_system_world.entity()
            .set<Position>({static_cast<double>(index), 0.0, 0.0})
            .set<Velocity>({1.0, 0.5, 0.25});
    }
    flecs_system_world.system<Position, const Velocity>()
        .each([](Position& position, const Velocity& velocity) {
            position.x += velocity.x;
            position.y += velocity.y;
            position.z += velocity.z;
        });
    const auto flecs_system = elapsed_microseconds([&] {
        for (int step = 0; step < 10; ++step) {
            flecs_system_world.progress();
        }
    });

    entt::registry entt_system_registry;
    for (std::uint64_t index = 1; index <= count; ++index) {
        const auto entity = entt_system_registry.create();
        entt_system_registry.emplace<Position>(entity, static_cast<double>(index), 0.0, 0.0);
        entt_system_registry.emplace<Velocity>(entity, 1.0, 0.5, 0.25);
    }
    const auto entt_system = elapsed_microseconds([&] {
        auto view = entt_system_registry.view<Position, const Velocity>();
        for (int step = 0; step < 10; ++step) {
            view.each([](Position& position, const Velocity& velocity) {
                position.x += velocity.x;
                position.y += velocity.y;
                position.z += velocity.z;
            });
        }
    });

    double flecs_checksum = 0.0;
    flecs_system_world.each<Position>([&flecs_checksum](const Position& position) {
        flecs_checksum += position.x + position.y + position.z;
    });
    double entt_checksum = 0.0;
    entt_system_registry.view<const Position>().each(
        [&entt_checksum](const Position& position) {
            entt_checksum += position.x + position.y + position.z;
        });

    std::cout << "system_steps=10"
              << " flecs_system_us=" << flecs_system
              << " entt_system_us=" << entt_system
              << " checksum=" << flecs_checksum << '\n';
    return visited == count && std::abs(flecs_checksum - entt_checksum) < 0.001 ? 0 : 1;
}
