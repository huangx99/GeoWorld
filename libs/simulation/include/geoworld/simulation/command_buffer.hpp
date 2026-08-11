#pragma once

#include "geoworld/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace geoworld::simulation {

struct CreateObjectCommand {
    world::WorldObject object;
};

struct DestroyObjectCommand {
    foundation::WorldId id;
};

struct SetPropertyCommand {
    foundation::WorldId id;
    std::string key;
    world::PropertyValue value;
};

using CommandPayload = std::variant<CreateObjectCommand, DestroyObjectCommand, SetPropertyCommand>;

struct Command {
    std::uint64_t sequence{};
    std::uint64_t target_tick{};
    CommandPayload payload;
};

struct ApplyReport {
    std::size_t applied{};
    std::size_t rejected{};
    std::size_t deferred{};
};

class CommandBuffer {
public:
    [[nodiscard]] std::uint64_t enqueue(std::uint64_t target_tick, CommandPayload payload);
    [[nodiscard]] ApplyReport apply(world::World& world, std::uint64_t tick);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::uint64_t next_sequence_{1};
    std::vector<Command> pending_;
};

} // namespace geoworld::simulation
