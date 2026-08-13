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

// 传输无关的外部命令 metadata；不包含协议、连接或身份信息类型。
struct CommandMeta {
    std::uint64_t ingress_sequence{};
    // 乐观并发前置条件；0 表示不检查。
    std::uint64_t expected_object_version{};
};

enum class CommandRejectReason {
    none,
    missing_object,
    version_conflict,
    apply_failed,
};

struct CommandOutcome {
    std::uint64_t sequence{};
    std::uint64_t ingress_sequence{};
    bool applied{};
    CommandRejectReason reason{CommandRejectReason::none};
};

struct Command {
    std::uint64_t sequence{};
    std::uint64_t target_tick{};
    CommandPayload payload;
    CommandMeta meta{};
};

struct ApplyReport {
    std::size_t applied{};
    std::size_t rejected{};
    std::size_t deferred{};
    std::vector<CommandOutcome> outcomes;
};

class CommandBuffer {
public:
    [[nodiscard]] std::uint64_t enqueue(std::uint64_t target_tick, CommandPayload payload);
    [[nodiscard]] std::uint64_t enqueue(std::uint64_t target_tick, CommandPayload payload,
                                        CommandMeta meta);
    [[nodiscard]] ApplyReport apply(world::World& world, std::uint64_t tick);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::uint64_t next_sequence_{1};
    std::vector<Command> pending_;
};

} // namespace geoworld::simulation
