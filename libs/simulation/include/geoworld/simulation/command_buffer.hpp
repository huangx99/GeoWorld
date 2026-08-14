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
    // 0 表示非 durable 输入；非零值用于检查点证明该 WAL 输入已执行或进入未来队列。
    std::uint64_t durable_lsn{};
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
    std::uint64_t durable_lsn{};
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

// ingress 序列从 1 开始；0 保留为 enqueue 失败（序列空间回绕耗尽）的返回值。
inline constexpr std::uint64_t kFirstCommandSequence = 1;

struct CommandBufferSnapshot {
    std::uint64_t next_sequence{kFirstCommandSequence};
    std::uint64_t included_durable_lsn{};
    std::vector<Command> pending;
};

class CommandBuffer {
public:
    // first_sequence 仅用于测试注入回绕边界；生产一律默认。
    explicit CommandBuffer(std::uint64_t first_sequence = kFirstCommandSequence);

    // 返回分配的序列号；0 表示序列空间回绕耗尽，命令被拒绝且未入队。
    [[nodiscard]] std::uint64_t enqueue(std::uint64_t target_tick, CommandPayload payload);
    [[nodiscard]] std::uint64_t enqueue(std::uint64_t target_tick, CommandPayload payload,
                                        CommandMeta meta);
    [[nodiscard]] ApplyReport apply(world::World& world, std::uint64_t tick);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] CommandBufferSnapshot snapshot() const;
    [[nodiscard]] bool restore(CommandBufferSnapshot snapshot);

private:
    std::uint64_t next_sequence_{kFirstCommandSequence};
    std::uint64_t included_durable_lsn_{};
    std::vector<Command> pending_;
};

} // namespace geoworld::simulation
