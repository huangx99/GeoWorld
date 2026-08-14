#pragma once

#include "geoworld/foundation/ids.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace geoworld::protocol {

// 传输无关的内部线结构，字段与 schemas/data/world_stream.fbs、client_stream.fbs 对齐。
// 公共 API 不出现 flatbuffers::/protobuf 类型；编解码在 protocol 实现内完成。

// schema::Value 的线映射，variant 顺序固定为 int64/double/bool/string，
// 与规范化投影 hash 的 value tag 一致。
using WireValue = std::variant<std::int64_t, double, bool, std::string>;
using WireAttributes = std::map<std::string, WireValue>; // 有序 map，保证规范化顺序

enum class Lifecycle : std::uint8_t { staged = 0, active = 1, suspended = 2, retired = 3 };

enum class FrequencyClass : std::uint8_t { on_change = 0, slow = 1, fast = 2 };

// 离开原因只用于诊断，不改变客户端删除语义。
enum class LeaveReason : std::uint8_t { out_of_area = 0, not_relevant = 1, destroyed = 2, policy_changed = 3 };

enum class ReceiptStatus : std::uint8_t {
    unspecified = 0,
    accepted = 1,
    applied = 2,
    rejected = 3,
    duplicate = 4,
    durable_accepted = 5,
};

enum class ReliableKind : std::uint8_t {
    unspecified = 0,
    command_receipt = 1,
    protocol_error = 2,
    event = 3,
};

struct WireVec3 {
    double x{};
    double y{};
    double z{};

    bool operator==(const WireVec3&) const = default;
};

// fbs RelationEntry 只携带 type 与 target_wid，不携带 relation attributes。
struct WireRelation {
    std::string type;
    std::uint64_t target_wid{};

    bool operator==(const WireRelation&) const = default;
};

struct WireEntity {
    foundation::WorldId wid{};
    foundation::FeatureId fid{};
    std::uint64_t version{};
    WireVec3 position;
    std::string semantic_type;
    std::string geometry_ref;
    Lifecycle lifecycle{Lifecycle::staged};
    WireAttributes properties;
    WireAttributes state;
    std::vector<WireRelation> relations;
    std::vector<std::string> capabilities;
    FrequencyClass frequency{FrequencyClass::on_change};
    std::uint32_t priority{};
    std::vector<std::string> visibility_tags;

    bool operator==(const WireEntity&) const = default;
};

struct WireEnter {
    foundation::WorldId wid{};
    foundation::FeatureId fid{};
    WireEntity entity;

    bool operator==(const WireEnter&) const = default;
};

// update 携带该实体完整的最新投影，不做字段级 patch。
struct WireUpdate {
    foundation::WorldId wid{};
    foundation::FeatureId fid{};
    WireEntity entity;

    bool operator==(const WireUpdate&) const = default;
};

struct WireLeave {
    foundation::WorldId wid{};
    foundation::FeatureId fid{};
    LeaveReason reason{LeaveReason::out_of_area};

    bool operator==(const WireLeave&) const = default;
};

// Keyframe：当前连接完整可见集及完整 FID 映射，baseline_snapshot_id = 0。
struct WireKeyframe {
    std::uint64_t stream_epoch{};
    std::uint64_t stream_sequence{};
    std::uint64_t snapshot_id{};
    std::uint64_t baseline_snapshot_id{};
    std::uint64_t tick{};
    std::int64_t simulation_time_us{};
    std::vector<WireEnter> entities;

    bool operator==(const WireKeyframe&) const = default;
};

// Delta：leave 按 FID 升序、enter 按 WID 升序、update 按 FID 升序输出。
struct WireDelta {
    std::uint64_t stream_epoch{};
    std::uint64_t stream_sequence{};
    std::uint64_t snapshot_id{};
    std::uint64_t baseline_snapshot_id{};
    std::uint64_t tick{};
    std::int64_t simulation_time_us{};
    std::vector<WireEnter> enters;
    std::vector<WireUpdate> updates;
    std::vector<WireLeave> leaves;

    bool operator==(const WireDelta&) const = default;
};

struct WireCommandReceipt {
    std::uint64_t client_sequence{};
    ReceiptStatus status{ReceiptStatus::unspecified};
    std::string error_code;

    bool operator==(const WireCommandReceipt&) const = default;
};

struct WireProtocolError {
    std::string code;
    std::string message;

    bool operator==(const WireProtocolError&) const = default;
};

// ReliableEvent：不可被状态帧合并丢弃的事件、命令终态回执和协议错误；
// kind 指示 receipt 或 error 哪个有效。
struct WireReliable {
    ReliableKind kind{ReliableKind::unspecified};
    WireCommandReceipt receipt;
    WireProtocolError error;

    bool operator==(const WireReliable&) const = default;
};

// Heartbeat：连接活性及最新服务器 tick，不参与 replica 状态。
struct WireHeartbeat {
    std::uint64_t server_tick{};
    std::uint64_t latest_snapshot_id{};

    bool operator==(const WireHeartbeat&) const = default;
};

using WireFrame = std::variant<WireKeyframe, WireDelta, WireReliable, WireHeartbeat>;

struct WireAck {
    std::uint64_t stream_epoch{};
    std::uint64_t snapshot_id{};

    bool operator==(const WireAck&) const = default;
};

struct WireKeyframeRequest {
    std::string reason;

    bool operator==(const WireKeyframeRequest&) const = default;
};

using WireClientControl = std::variant<WireAck, WireKeyframeRequest>;

} // namespace geoworld::protocol
