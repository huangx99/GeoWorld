#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/error.hpp"
#include "geoworld/protocol/limits.hpp"
#include "geoworld/protocol/replica.hpp"
#include "geoworld/protocol/version.hpp"
#include "geoworld/protocol/wire.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using geoworld::foundation::FeatureId;
using geoworld::foundation::WorldId;
using geoworld::protocol::DecodeFailure;
using geoworld::protocol::ProtocolLimits;
using geoworld::protocol::WireAck;
using geoworld::protocol::WireClientControl;
using geoworld::protocol::WireDelta;
using geoworld::protocol::WireEnter;
using geoworld::protocol::WireEntity;
using geoworld::protocol::WireFrame;
using geoworld::protocol::WireHeartbeat;
using geoworld::protocol::WireKeyframe;
using geoworld::protocol::WireKeyframeRequest;
using geoworld::protocol::WireLeave;
using geoworld::protocol::WireReliable;
using geoworld::protocol::WireUpdate;
using geoworld::protocol::WireValue;
using geoworld::protocol::WireVec3;

// ---- 测试内独立的 FNV-1a 参考实现（从 docs/M4.md 与 canonical.cpp 的字段序列化规范手写）----

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void ref_u8(std::uint64_t& hash, std::uint8_t value) {
    hash = (hash ^ value) * kFnvPrime;
}

void ref_u32(std::uint64_t& hash, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        ref_u8(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void ref_u64(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        ref_u8(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void ref_f64(std::uint64_t& hash, double value) {
    ref_u64(hash, std::bit_cast<std::uint64_t>(value));
}

void ref_text(std::uint64_t& hash, std::string_view value) {
    ref_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const char character : value) {
        ref_u8(hash, static_cast<std::uint8_t>(character));
    }
}

void ref_value(std::uint64_t& hash, const WireValue& value) {
    ref_u8(hash, static_cast<std::uint8_t>(value.index()));
    switch (value.index()) {
    case 0:
        ref_u64(hash, std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value)));
        break;
    case 1:
        ref_f64(hash, std::get<double>(value));
        break;
    case 2:
        ref_u8(hash, std::get<bool>(value) ? 1U : 0U);
        break;
    case 3:
        ref_text(hash, std::get<std::string>(value));
        break;
    default:
        break;
    }
}

void ref_bag(std::uint64_t& hash, const geoworld::protocol::WireAttributes& bag) {
    ref_u64(hash, static_cast<std::uint64_t>(bag.size()));
    for (const auto& [key, value] : bag) {
        ref_text(hash, key);
        ref_value(hash, value);
    }
}

[[nodiscard]] std::uint64_t ref_entity_hash(const WireEntity& entity) {
    std::uint64_t hash = kFnvOffset;
    ref_u64(hash, entity.wid.value);
    ref_u64(hash, entity.version);
    ref_f64(hash, entity.position.x);
    ref_f64(hash, entity.position.y);
    ref_f64(hash, entity.position.z);
    ref_text(hash, entity.semantic_type);
    ref_text(hash, entity.geometry_ref);
    ref_u8(hash, static_cast<std::uint8_t>(entity.lifecycle));
    ref_bag(hash, entity.properties);
    ref_bag(hash, entity.state);
    ref_u64(hash, static_cast<std::uint64_t>(entity.relations.size()));
    for (const auto& relation : entity.relations) {
        ref_text(hash, relation.type);
        ref_u64(hash, relation.target_wid);
        ref_u64(hash, 0); // 空 attributes bag
    }
    ref_u64(hash, static_cast<std::uint64_t>(entity.capabilities.size()));
    for (const auto& capability : entity.capabilities) {
        ref_text(hash, capability);
    }
    ref_u8(hash, static_cast<std::uint8_t>(entity.frequency));
    ref_u32(hash, entity.priority);
    ref_u64(hash, static_cast<std::uint64_t>(entity.visibility_tags.size()));
    for (const auto& tag : entity.visibility_tags) {
        ref_text(hash, tag);
    }
    return hash;
}

// ---- 样例数据 ----

[[nodiscard]] WireEntity make_entity(std::uint64_t wid, std::uint32_t fid, double x) {
    WireEntity entity;
    entity.wid = WorldId{wid};
    entity.fid = FeatureId{fid};
    entity.version = 7;
    entity.position = WireVec3{x, 2.5, -3.75};
    entity.semantic_type = "test.entity";
    entity.geometry_ref = "asset/test";
    entity.lifecycle = geoworld::protocol::Lifecycle::active;
    entity.properties.emplace("count", std::int64_t{42});
    entity.properties.emplace("speed", 12.5);
    entity.state.emplace("mode", std::string{"idle"});
    entity.state.emplace("armed", true);
    entity.relations.push_back({"parent", 100});
    entity.capabilities.push_back("move");
    entity.frequency = geoworld::protocol::FrequencyClass::fast;
    entity.priority = 3;
    entity.visibility_tags.push_back("public");
    return entity;
}

[[nodiscard]] WireFrame make_keyframe() {
    WireKeyframe keyframe;
    keyframe.stream_epoch = 3;
    keyframe.stream_sequence = 1;
    keyframe.snapshot_id = 100;
    keyframe.baseline_snapshot_id = 0;
    keyframe.tick = 55;
    keyframe.simulation_time_us = 1'100'000;
    keyframe.entities.push_back({WorldId{1}, FeatureId{1}, make_entity(1, 1, 10.0)});
    keyframe.entities.push_back({WorldId{2}, FeatureId{2}, make_entity(2, 2, 20.0)});
    return keyframe;
}

[[nodiscard]] WireFrame make_delta() {
    WireDelta delta;
    delta.stream_epoch = 3;
    delta.stream_sequence = 2;
    delta.snapshot_id = 101;
    delta.baseline_snapshot_id = 100;
    delta.tick = 56;
    delta.simulation_time_us = 1'120'000;
    delta.leaves.push_back({WorldId{1}, FeatureId{1},
                            geoworld::protocol::LeaveReason::out_of_area});
    delta.enters.push_back({WorldId{3}, FeatureId{3}, make_entity(3, 3, 30.0)});
    WireEntity moved = make_entity(2, 2, 25.0);
    delta.updates.push_back({WorldId{2}, FeatureId{2}, std::move(moved)});
    return delta;
}

// ---- 用例 ----

[[nodiscard]] bool version_negotiation() {
    using geoworld::protocol::VersionRange;
    using geoworld::protocol::negotiate_version;
    if (negotiate_version(VersionRange{1, 3}, VersionRange{1, 1}) != std::optional{1U}) {
        return false;
    }
    if (negotiate_version(VersionRange{1, 5}, VersionRange{3, 4}) != std::optional{4U}) {
        return false;
    }
    if (negotiate_version(VersionRange{2, 4}, VersionRange{1, 1}).has_value()) {
        return false;
    }
    if (negotiate_version(VersionRange{4, 2}, VersionRange{1, 9}).has_value()) {
        return false;
    }
    return geoworld::protocol::control_api_version == 1
        && geoworld::protocol::data_schema_version == 1
        && geoworld::protocol::projection_schema_version == 1
        && geoworld::protocol::websocket_subprotocol == "geoworld.stream.v1"
        && geoworld::protocol::server_frame_identifier == "GWSF"
        && geoworld::protocol::client_frame_identifier == "GWCF";
}

[[nodiscard]] bool server_frame_round_trip() {
    DecodeFailure failure;

    const WireFrame keyframe = make_keyframe();
    const auto keyframe_bytes = geoworld::protocol::encode_server_frame(keyframe);
    if (keyframe_bytes.empty()) {
        return false;
    }
    const auto decoded_keyframe = geoworld::protocol::decode_server_frame(keyframe_bytes, failure);
    if (!decoded_keyframe.has_value() || *decoded_keyframe != keyframe) {
        return false;
    }

    const WireFrame delta = make_delta();
    const auto delta_bytes = geoworld::protocol::encode_server_frame(delta);
    const auto decoded_delta = geoworld::protocol::decode_server_frame(delta_bytes, failure);
    if (!decoded_delta.has_value() || *decoded_delta != delta) {
        return false;
    }

    WireReliable reliable;
    reliable.kind = geoworld::protocol::ReliableKind::command_receipt;
    reliable.receipt.client_sequence = 9;
    reliable.receipt.status = geoworld::protocol::ReceiptStatus::applied;
    reliable.receipt.error_code = "";
    const auto reliable_bytes = geoworld::protocol::encode_server_frame(WireFrame{reliable});
    const auto decoded_reliable =
        geoworld::protocol::decode_server_frame(reliable_bytes, failure);
    if (!decoded_reliable.has_value() || *decoded_reliable != WireFrame{reliable}) {
        return false;
    }

    WireReliable error_event;
    error_event.kind = geoworld::protocol::ReliableKind::protocol_error;
    error_event.error.code = "GWG104";
    error_event.error.message = "baseline evicted";
    const auto error_bytes = geoworld::protocol::encode_server_frame(WireFrame{error_event});
    const auto decoded_error = geoworld::protocol::decode_server_frame(error_bytes, failure);
    if (!decoded_error.has_value() || *decoded_error != WireFrame{error_event}) {
        return false;
    }

    const WireFrame heartbeat = WireHeartbeat{77, 101};
    const auto heartbeat_bytes = geoworld::protocol::encode_server_frame(heartbeat);
    const auto decoded_heartbeat =
        geoworld::protocol::decode_server_frame(heartbeat_bytes, failure);
    return decoded_heartbeat.has_value() && *decoded_heartbeat == heartbeat;
}

[[nodiscard]] bool client_control_round_trip() {
    DecodeFailure failure;
    const WireClientControl ack = WireAck{7, 42};
    const auto ack_bytes = geoworld::protocol::encode_client_control(ack);
    const auto decoded_ack = geoworld::protocol::decode_client_control(ack_bytes, failure);
    if (!decoded_ack.has_value() || *decoded_ack != ack) {
        return false;
    }
    const WireClientControl request = WireKeyframeRequest{"baseline lost"};
    const auto request_bytes = geoworld::protocol::encode_client_control(request);
    const auto decoded_request = geoworld::protocol::decode_client_control(request_bytes, failure);
    return decoded_request.has_value() && *decoded_request == request;
}

[[nodiscard]] bool encode_is_deterministic() {
    const WireFrame keyframe = make_keyframe();
    const auto first = geoworld::protocol::encode_server_frame(keyframe);
    const auto second = geoworld::protocol::encode_server_frame(keyframe);
    if (first.empty() || first != second) {
        return false;
    }
    const WireClientControl ack = WireAck{7, 42};
    return geoworld::protocol::encode_client_control(ack)
        == geoworld::protocol::encode_client_control(ack);
}

[[nodiscard]] bool malformed_frames_are_rejected() {
    DecodeFailure failure;
    const auto valid = geoworld::protocol::encode_server_frame(make_keyframe());
    if (valid.empty()) {
        return false;
    }

    // 空 buffer。
    failure = {};
    if (geoworld::protocol::decode_server_frame({}, failure).has_value()
        || failure.error_code != geoworld::protocol::error_invalid_request) {
        return false;
    }
    // 过短 buffer。
    failure = {};
    const std::vector<std::uint8_t> tiny{1, 2, 3};
    if (geoworld::protocol::decode_server_frame(tiny, failure).has_value()
        || failure.error_code != geoworld::protocol::error_invalid_request) {
        return false;
    }
    // 超过 max_frame_bytes。
    ProtocolLimits tight;
    tight.max_frame_bytes = 16;
    failure = {};
    if (geoworld::protocol::decode_server_frame(valid, failure, tight).has_value()
        || failure.error_code != geoworld::protocol::error_limit_exceeded) {
        return false;
    }
    // file identifier 不匹配。
    failure = {};
    auto wrong_identifier = valid;
    wrong_identifier[4] = 'X';
    if (geoworld::protocol::decode_server_frame(wrong_identifier, failure).has_value()
        || failure.error_code != geoworld::protocol::error_version_incompatible) {
        return false;
    }
    // schema_version != 1（手工构造仅含 schema_version=2 的最小 GWSF 帧）。
    failure = {};
    const std::vector<std::uint8_t> wrong_version = {
        0x08, 0x00, 0x00, 0x00, // root table 偏移
        'G',  'W',  'S',  'F',  // file identifier
        0xF8, 0xFF, 0xFF, 0xFF, // table: soffset 指向后方 vtable（-8）
        0x02, 0x00, 0x00, 0x00, // schema_version = 2
        0x06, 0x00, // vtable 大小
        0x08, 0x00, // table inline 大小
        0x04, 0x00, // field 0（schema_version）偏移
    };
    if (geoworld::protocol::decode_server_frame(wrong_version, failure).has_value()
        || failure.error_code != geoworld::protocol::error_version_incompatible) {
        return false;
    }
    // 截断 buffer：verifier 必须稳定拒绝。
    failure = {};
    const std::vector<std::uint8_t> truncated(valid.begin(), valid.end() - 3);
    if (geoworld::protocol::decode_server_frame(truncated, failure).has_value()
        || failure.error_code != geoworld::protocol::error_invalid_request) {
        return false;
    }
    // 垃圾字节但 identifier 正确：verifier 必须稳定拒绝。
    failure = {};
    std::vector<std::uint8_t> garbage(64, 0xAB);
    garbage[4] = 'G';
    garbage[5] = 'W';
    garbage[6] = 'S';
    garbage[7] = 'F';
    if (geoworld::protocol::decode_server_frame(garbage, failure).has_value()
        || failure.error_code != geoworld::protocol::error_invalid_request) {
        return false;
    }

    // 客户端帧：错误 identifier 与截断同样稳定拒绝。
    const auto ack = geoworld::protocol::encode_client_control(WireClientControl{WireAck{7, 42}});
    failure = {};
    auto wrong_client_identifier = ack;
    wrong_client_identifier[4] = 'X';
    if (geoworld::protocol::decode_client_control(wrong_client_identifier, failure).has_value()
        || failure.error_code != geoworld::protocol::error_version_incompatible) {
        return false;
    }
    failure = {};
    const std::vector<std::uint8_t> truncated_client(ack.begin(), ack.end() - 2);
    return !geoworld::protocol::decode_client_control(truncated_client, failure).has_value()
        && failure.error_code == geoworld::protocol::error_invalid_request;
}

[[nodiscard]] bool limits_are_enforced() {
    // 非法配置（0 值）必须被拒绝并给出诊断。
    ProtocolLimits limits;
    std::string diagnostic;
    if (!geoworld::protocol::validate(limits, diagnostic)) {
        return false;
    }
    limits.max_frame_bytes = 0;
    if (geoworld::protocol::validate(limits, diagnostic) || diagnostic.empty()) {
        return false;
    }
    limits = ProtocolLimits{};
    limits.max_entities_per_frame = 0;
    if (geoworld::protocol::validate(limits, diagnostic) || diagnostic.empty()) {
        return false;
    }

    // 编码侧：超过上限的输入返回空 vector。
    limits = ProtocolLimits{};
    limits.max_entities_per_frame = 1;
    if (!geoworld::protocol::encode_server_frame(make_keyframe(), limits).empty()) {
        return false;
    }
    limits = ProtocolLimits{};
    limits.max_string_bytes = 4;
    if (!geoworld::protocol::encode_server_frame(make_keyframe(), limits).empty()) {
        return false;
    }
    // 解码侧：收紧上限后合法帧被拒绝为 GWG006。
    limits = ProtocolLimits{};
    limits.max_entities_per_frame = 1;
    DecodeFailure failure;
    const auto valid = geoworld::protocol::encode_server_frame(make_keyframe());
    return !geoworld::protocol::decode_server_frame(valid, failure, limits).has_value()
        && failure.error_code == geoworld::protocol::error_limit_exceeded;
}

[[nodiscard]] bool replica_hash_matches_reference() {
    // fid 不参与实体 hash。
    WireEntity entity_a = make_entity(1, 1, 10.0);
    WireEntity entity_b = entity_a;
    entity_b.fid = FeatureId{9};
    if (geoworld::protocol::wire_entity_hash(entity_a)
        != geoworld::protocol::wire_entity_hash(entity_b)) {
        return false;
    }
    // 实体 hash 与测试内独立参考实现一致。
    if (geoworld::protocol::wire_entity_hash(entity_a) != ref_entity_hash(entity_a)) {
        return false;
    }

    geoworld::protocol::ReplicaAccumulator replica;
    const WireFrame keyframe = make_keyframe();
    replica.apply(keyframe);
    const WireFrame delta = make_delta();
    replica.apply(delta);
    if (replica.size() != 2) {
        return false;
    }

    // 手算期望 hash：keyframe {fid1 wid1, fid2 wid2} 经 delta 后剩
    // {fid2 wid2 moved, fid3 wid3}，按 FID 升序折叠 (fid, wid, entity_hash)。
    const auto& delta_frame = std::get<WireDelta>(delta);
    const auto& keyframe_frame = std::get<WireKeyframe>(keyframe);
    const std::uint64_t hash_wid2 = ref_entity_hash(delta_frame.updates[0].entity);
    const std::uint64_t hash_wid3 = ref_entity_hash(delta_frame.enters[0].entity);
    static_cast<void>(keyframe_frame);
    std::uint64_t expected = kFnvOffset;
    ref_u64(expected, 2); // fid
    ref_u64(expected, 2); // wid
    ref_u64(expected, hash_wid2);
    ref_u64(expected, 3);
    ref_u64(expected, 3);
    ref_u64(expected, hash_wid3);
    if (replica.hash() != expected) {
        return false;
    }

    // heartbeat 与 reliable 不改变 replica。
    replica.apply(WireFrame{WireHeartbeat{56, 101}});
    if (replica.hash() != expected || replica.size() != 2) {
        return false;
    }

    // 新 keyframe 整表替换。
    geoworld::protocol::ReplicaAccumulator fresh;
    fresh.apply(delta); // delta 对空 replica 也应得到同一状态
    return fresh.hash() == expected;
}

[[nodiscard]] bool interpolation_sampling() {
    const WorldId wid{1};
    geoworld::protocol::InterpolationBuffer buffer; // 默认延迟 100 ms

    WireKeyframe first;
    first.tick = 0;
    first.simulation_time_us = 0;
    WireEntity at_zero;
    at_zero.wid = wid;
    at_zero.position = WireVec3{0.0, 0.0, 0.0};
    first.entities.push_back({wid, FeatureId{1}, at_zero});
    buffer.observe(first);

    WireDelta second;
    second.tick = 1;
    second.simulation_time_us = 1'000'000;
    WireEntity moved;
    moved.wid = wid;
    moved.position = WireVec3{100.0, 200.0, 300.0};
    second.updates.push_back({wid, FeatureId{1}, moved});
    buffer.observe(second);

    if (buffer.render_time_us() != 900'000) {
        return false;
    }
    const auto midpoint = buffer.sample(wid, 500'000);
    if (!midpoint.has_value() || *midpoint != WireVec3{50.0, 100.0, 150.0}) {
        return false;
    }
    // 早于首个快照钳制到首快照。
    const auto clamped_low = buffer.sample(wid, -5);
    if (!clamped_low.has_value() || *clamped_low != WireVec3{0.0, 0.0, 0.0}) {
        return false;
    }
    // 晚于最新快照钳制到最新快照。
    const auto clamped_high = buffer.sample(wid, 2'000'000);
    if (!clamped_high.has_value() || *clamped_high != WireVec3{100.0, 200.0, 300.0}) {
        return false;
    }
    // 未知实体返回空。
    if (buffer.sample(WorldId{99}, 500'000).has_value()) {
        return false;
    }

    // 第三帧到达后，早于渲染时刻且不再构成 bracket 的快照被淘汰。
    WireDelta third;
    third.tick = 2;
    third.simulation_time_us = 2'000'000;
    third.updates.push_back({wid, FeatureId{1}, moved});
    buffer.observe(third);
    if (buffer.render_time_us() != 1'900'000 || buffer.snapshot_count() != 2) {
        return false;
    }
    const auto late = buffer.sample(wid, 1'900'000);
    return late.has_value() && *late == WireVec3{100.0, 200.0, 300.0};
}

} // namespace

int main() {
    if (!version_negotiation()) {
        return 1;
    }
    if (!server_frame_round_trip()) {
        return 2;
    }
    if (!client_control_round_trip()) {
        return 3;
    }
    if (!encode_is_deterministic()) {
        return 4;
    }
    if (!malformed_frames_are_rejected()) {
        return 5;
    }
    if (!limits_are_enforced()) {
        return 6;
    }
    if (!replica_hash_matches_reference()) {
        return 7;
    }
    if (!interpolation_sampling()) {
        return 8;
    }
    return 0;
}
