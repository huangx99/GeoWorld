#pragma once

#include "geoworld/gateway/types.hpp"
#include "geoworld/simulation/command_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geoworld::gateway {

// durable admission 记录格式版本（具名常量；演进只能追加版本分支）。
inline constexpr std::uint16_t kDurableRecordFormatVersion = 1;

// 记录种类镜像 persistence::WalRecordKind 数值；桥接层 static_assert 校验一致，
// gateway 本身不依赖 persistence 头文件（M5.md：gateway -> 窄接口 -> persistence）。
enum class DurableRecordKind : std::uint8_t {
    external_command = 0,
    command_outcome = 4,
};

struct DurableRecord {
    DurableRecordKind kind{DurableRecordKind::external_command};
    std::uint64_t target_tick{};
    std::vector<std::byte> payload;
};

struct DurableAppendOutcome {
    std::uint64_t lsn{};
    bool ok{};
};

// 一次 append 的异步完成句柄；try_outcome 非阻塞，返回 false 表示仍在写入。
class DurableTicket {
public:
    virtual ~DurableTicket() = default;
    [[nodiscard]] virtual bool try_outcome(DurableAppendOutcome& outcome) = 0;
};

// durable 接纳日志窄接口，由组合根注入 persistence 桥接。注入为空时 durable
// 请求以 GWG206 拒绝，M4 进程内去重路径完全不变。
class DurableAdmissionLog {
public:
    virtual ~DurableAdmissionLog() = default;

    // 队列满、故障态、关闭中返回 nullptr（立即拒绝，映射 GWG206）。
    [[nodiscard]] virtual std::unique_ptr<DurableTicket> append(
        const DurableRecord& record) = 0;
};

// ---- 持久幂等索引 ----

enum class DurableEntryState : std::uint8_t {
    pending,           // 已登记，WAL 写入中
    durable_accepted,  // WAL 已持久化（LSN 有效），等待执行终态
    applied,
    rejected,
};

struct DurableIdempotencyEntry {
    DurableEntryState state{DurableEntryState::pending};
    std::uint64_t lsn{};
    std::uint64_t client_sequence{};
    std::uint64_t ingress_sequence{};
    // rejected 终态的稳定拒绝原因；其余状态为 none。
    GatewayError error{GatewayError::none};
};

// (principal_id, request_id) -> 幂等结果。内存索引 + WAL scan 重建；
// 相同键相同内容返回既有结果，相同键不同内容稳定拒绝（GWG205）。
class DurableIdempotencyIndex {
public:
    struct Key {
        std::string principal_id;
        DurableRequestId request_id{};

        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept;
    };

    struct LookupResult {
        const DurableIdempotencyEntry* entry{};
        // 键命中时命令内容指纹是否一致；未命中时无意义。
        bool content_match{};
    };

    // fingerprint 为 make_command_fingerprint 的输出（命令内容规范化字节）。
    [[nodiscard]] LookupResult lookup(const Key& key,
                                      const std::vector<std::byte>& fingerprint) const;
    void register_pending(Key key, std::vector<std::byte> fingerprint,
                          std::uint64_t client_sequence);
    void mark_durable_accepted(const Key& key, std::uint64_t lsn,
                               std::uint64_t ingress_sequence);
    void mark_final(const Key& key, bool applied, GatewayError error);
    void erase(const Key& key);
    [[nodiscard]] std::size_t size() const noexcept;

    // 重启重建：输入扫描到的 WAL 记录（gateway 自有编码载荷），返回 false 表示
    // 记录无法解析，调用方必须 fail-closed。未知键的 outcome 记录跳过（容许
    // 未来保留期裁剪后的残余终态）。
    [[nodiscard]] bool restore(DurableRecordKind kind, std::uint64_t lsn,
                               std::span<const std::byte> payload);

private:
    struct Slot {
        std::vector<std::byte> fingerprint;
        DurableIdempotencyEntry entry;
    };

    std::unordered_map<Key, Slot, KeyHash> slots_;
};

// ---- 规范化编码 ----
// 同内容必同字节（确定性要求：指纹比较与 WAL 回放依赖逐字节一致）。
// 整数小端定长，字符串 u16 长度前缀，double 取位模式，PropertyBag 按 std::map
// 键序遍历。

// 命令内容指纹：operation、target_wid、client_sequence、expected_version、
// target_tick_hint 与参数的规范化字节；不含 principal 与 request_id。
[[nodiscard]] std::vector<std::byte> make_command_fingerprint(const ExternalCommand& command);

// external_command 记录载荷：format_version + principal + request_id
// + 已决定 target_tick + 指纹（即命令内容）。
[[nodiscard]] std::vector<std::byte> encode_external_command_record(
    std::string_view principal_id, const DurableRequestId& request_id,
    std::uint64_t target_tick, const ExternalCommand& command);

struct RecoveredDurableCommand {
    std::string principal_id;
    DurableRequestId request_id{};
    std::uint64_t target_tick{};
    std::uint64_t client_sequence{};
    std::uint64_t expected_object_version{};
    simulation::CommandPayload payload;
};

[[nodiscard]] std::optional<RecoveredDurableCommand> decode_external_command_record(
    std::span<const std::byte> payload);

struct RecoveredDurableOutcome {
    std::string principal_id;
    DurableRequestId request_id{};
    std::uint64_t client_sequence{};
    std::uint64_t ingress_sequence{};
    bool applied{};
    GatewayError error{GatewayError::none};
};

[[nodiscard]] std::optional<RecoveredDurableOutcome> decode_command_outcome_record(
    std::span<const std::byte> payload);

// command_outcome 记录载荷：format_version + principal + request_id
// + client_sequence + ingress_sequence + 终态与拒绝原因。
[[nodiscard]] std::vector<std::byte> encode_command_outcome_record(
    std::string_view principal_id, const DurableRequestId& request_id,
    std::uint64_t client_sequence, std::uint64_t ingress_sequence,
    bool applied, GatewayError error);

} // namespace geoworld::gateway
