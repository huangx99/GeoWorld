#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/gateway/frame_codec.hpp"
#include "geoworld/projection/canonical.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using geoworld::foundation::WorldId;
using geoworld::gateway::CommandReceipt;
using geoworld::gateway::ExternalCommand;
using geoworld::gateway::FixtureAuthentication;
using geoworld::gateway::FixtureAuthorization;
using geoworld::gateway::GatewayConfig;
using geoworld::gateway::GatewayCore;
using geoworld::gateway::GatewayError;
using geoworld::gateway::ReceiptStatus;
using geoworld::gateway::ReliableQueue;
using geoworld::gateway::SessionId;
using geoworld::gateway::SetPropertyParams;
using geoworld::gateway::StateQueue;
using geoworld::projection::ConnectionId;
using geoworld::projection::ProjectionConfig;
using geoworld::projection::ProjectionEngine;
using geoworld::projection::ProjectionPolicy;

std::chrono::steady_clock::time_point g_now{};

[[nodiscard]] std::chrono::steady_clock::time_point fake_clock() {
    return g_now;
}

void advance_time(std::chrono::seconds delta) {
    g_now += delta;
}

std::uint64_t g_token_counter{};

[[nodiscard]] std::string fake_tokens() {
    return "ticket-" + std::to_string(++g_token_counter);
}

struct TestRig {
    ProjectionEngine engine;
    std::shared_ptr<FixtureAuthentication> authentication{
        std::make_shared<FixtureAuthentication>()};
    std::shared_ptr<FixtureAuthorization> authorization{
        std::make_shared<FixtureAuthorization>()};
    GatewayCore core;

    explicit TestRig(GatewayConfig gateway_config)
        : engine(make_projection_config(), make_policy()),
          core(std::move(gateway_config), engine, authentication, authorization,
               fake_clock, fake_tokens, 1, 1) {
        authentication->add_credential("observer-token", {"viewer", false});
        authentication->add_credential("admin-token", {"admin", true});
        authorization->allow_writable_property("speed");
    }

    static ProjectionConfig make_projection_config() {
        ProjectionConfig config;
        config.enu_origin = geoworld::spatial::geodetic_to_ecef({31.0, 121.0, 0.0});
        return config;
    }

    static ProjectionPolicy make_policy() {
        ProjectionPolicy policy;
        policy.allow_property("speed");
        return policy;
    }
};

[[nodiscard]] ExternalCommand set_speed(SessionId session, std::uint64_t sequence,
                                        WorldId target, double speed,
                                        std::uint64_t expected_version) {
    ExternalCommand command;
    command.session = session;
    command.client_sequence = sequence;
    command.target_wid = target;
    command.params = SetPropertyParams{"speed", speed};
    command.expected_object_version = expected_version;
    return command;
}

[[nodiscard]] bool session_ticket_and_negotiation() {
    TestRig rig{GatewayConfig{}};

    auto bad = rig.core.open_session("wrong-token", 1, 1, 1, 1, 0);
    if (bad.error != GatewayError::auth_failed) {
        return false;
    }
    auto incompatible = rig.core.open_session("observer-token", 2, 3, 1, 1, 0);
    if (incompatible.error != GatewayError::protocol_incompatible) {
        return false;
    }
    auto opened = rig.core.open_session("observer-token", 1, 1, 1, 1, 0);
    if (opened.error != GatewayError::none || !opened.session.session.id.valid()
        || opened.session.stream_ticket.empty()) {
        return false;
    }
    const std::string ticket = opened.session.stream_ticket;
    if (!rig.core.attach_stream(ConnectionId{1}, ticket).has_value()) {
        return false;
    }
    // ticket 一次性：第二次使用必须拒绝。
    return !rig.core.attach_stream(ConnectionId{2}, ticket).has_value();
}

[[nodiscard]] bool ticket_expires() {
    GatewayConfig config;
    config.stream_ticket_ttl_seconds = 30;
    TestRig rig{config};
    auto opened = rig.core.open_session("observer-token", 1, 1, 1, 1, 0);
    advance_time(std::chrono::seconds{31});
    return !rig.core.attach_stream(ConnectionId{1},
                                   opened.session.stream_ticket).has_value();
}

[[nodiscard]] bool ownership_rules() {
    TestRig rig{GatewayConfig{}};
    auto first = rig.core.open_session("observer-token", 1, 1, 1, 1, 0);
    auto second = rig.core.open_session("admin-token", 1, 1, 1, 1, 0);
    const SessionId viewer = first.session.session.id;
    const SessionId admin = second.session.session.id;

    if (rig.core.acquire_ownership(viewer, WorldId{5}, {"speed"}, 10, 0)
        != GatewayError::none) {
        return false;
    }
    // 租约未到期，其他会话冲突拒绝。
    if (rig.core.acquire_ownership(admin, WorldId{5}, {"speed"}, 10, 0)
        != GatewayError::permission_denied) {
        return false;
    }
    // 未授权属性键拒绝。
    if (rig.core.acquire_ownership(viewer, WorldId{5}, {"secret"}, 10, 0)
        != GatewayError::permission_denied) {
        return false;
    }
    // 租约到期后可重新申请。
    if (rig.core.acquire_ownership(admin, WorldId{5}, {"speed"}, 20, 15)
        != GatewayError::none) {
        return false;
    }
    // 幂等释放。
    if (rig.core.release_ownership(admin, WorldId{5}, {"speed"}) != GatewayError::none
        || rig.core.release_ownership(admin, WorldId{5}, {"speed"})
            != GatewayError::none) {
        return false;
    }
    return true;
}

[[nodiscard]] bool command_admission_and_receipts() {
    TestRig rig{GatewayConfig{}};

    std::optional<geoworld::simulation::CommandMeta> captured_meta;
    std::uint64_t captured_tick = 0;
    rig.core.set_command_submitter(
        [&captured_meta, &captured_tick](std::uint64_t target_tick,
                                         geoworld::simulation::CommandPayload payload,
                                         geoworld::simulation::CommandMeta meta) {
            static_cast<void>(payload);
            captured_tick = target_tick;
            captured_meta = meta;
            return meta.ingress_sequence;
        });
    rig.core.set_receipt_encoder([](const CommandReceipt& receipt) {
        geoworld::gateway::FrameBytes bytes(sizeof(std::uint64_t));
        const std::uint64_t sequence = receipt.client_sequence;
        std::memcpy(bytes.data(), &sequence, sizeof(sequence));
        return bytes;
    });

    auto opened = rig.core.open_session("observer-token", 1, 1, 1, 1, 0);
    const SessionId session = opened.session.session.id;
    if (rig.core.acquire_ownership(session, WorldId{5}, {"speed"}, 100, 0)
        != GatewayError::none) {
        return false;
    }

    // 未持有所有权的属性拒绝。
    auto denied = rig.core.submit_command(session, set_speed(session, 1, WorldId{5}, 1.0, 0), 0);
    if (denied.first == GatewayError::none) {
        // speed 已授权且持有所有权，应成功；此分支不应走到。
    }
    if (denied.first != GatewayError::none) {
        return false;
    }
    if (denied.second.status != ReceiptStatus::accepted
        || denied.second.ingress_sequence == 0 || !captured_meta.has_value()
        || captured_tick != 1) {
        return false;
    }

    // 未授权键直接拒绝。
    ExternalCommand secret;
    secret.session = session;
    secret.client_sequence = 2;
    secret.target_wid = WorldId{5};
    secret.params = SetPropertyParams{"secret", std::int64_t{1}};
    auto rejected = rig.core.submit_command(session, secret, 0);
    if (rejected.first != GatewayError::permission_denied) {
        return false;
    }

    // 目标 tick 超出窗口拒绝。
    auto far = set_speed(session, 3, WorldId{5}, 2.0, 0);
    far.target_tick_hint = 100;
    if (rig.core.submit_command(session, far, 0).first
        != GatewayError::tick_out_of_window) {
        return false;
    }

    // 同一 (session, client_sequence) 重试返回 duplicate 及原结果。
    auto retry = rig.core.submit_command(session, set_speed(session, 1, WorldId{5}, 1.0, 0), 0);
    if (retry.second.status != ReceiptStatus::duplicate) {
        return false;
    }

    // 终态回执：版本冲突映射为 GWG201。
    geoworld::simulation::ApplyReport report;
    report.outcomes.push_back(geoworld::simulation::CommandOutcome{
        1, captured_meta->ingress_sequence, false,
        geoworld::simulation::CommandRejectReason::version_conflict});
    rig.core.on_commands_applied(report);
    const auto cached = rig.core.sessions().find_receipt(session, 1);
    if (!cached.has_value() || cached->status != ReceiptStatus::rejected
        || cached->error != GatewayError::version_conflict) {
        return false;
    }
    return true;
}

[[nodiscard]] bool command_rate_limit() {
    GatewayConfig config;
    config.command_rate_per_session = 2;
    TestRig rig{config};
    rig.core.set_command_submitter(
        [](std::uint64_t, geoworld::simulation::CommandPayload,
           geoworld::simulation::CommandMeta meta) {
            return meta.ingress_sequence;
        });
    auto opened = rig.core.open_session("admin-token", 1, 1, 1, 1, 0);
    const SessionId session = opened.session.session.id;
    static_cast<void>(rig.core.acquire_ownership(session, WorldId{5}, {"speed"}, 100, 0));

    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        if (rig.core.submit_command(
                session, set_speed(session, sequence, WorldId{5}, 1.0, 0), 0).first
            != GatewayError::none) {
            return false;
        }
    }
    if (rig.core.submit_command(session, set_speed(session, 3, WorldId{5}, 1.0, 0), 0)
            .first != GatewayError::rate_limited) {
        return false;
    }
    // 时间推进后令牌恢复。
    advance_time(std::chrono::seconds{1});
    return rig.core.submit_command(session, set_speed(session, 4, WorldId{5}, 1.0, 0), 0)
        .first == GatewayError::none;
}

[[nodiscard]] bool queue_backpressure() {
    // state queue：同基线的较新 delta 覆盖较旧，超限置重同步标志。
    StateQueue state{128};
    const auto make_frame = [](std::uint64_t snapshot, std::uint64_t baseline,
                               std::size_t bytes) {
        return geoworld::gateway::QueuedFrame{snapshot, baseline, false,
                                              geoworld::gateway::FrameBytes(bytes)};
    };
    state.push(make_frame(1, 0, 32));
    state.push(make_frame(2, 0, 32));
    if (state.size() != 1 || state.bytes() != 32) {
        return false;
    }
    // 基线不同的 delta 无法合并，累计超限丢弃最旧并置重同步标志。
    state.push(make_frame(3, 0, 64));
    state.push(make_frame(4, 3, 96));
    if (state.bytes() > 128 || !state.resync_required()) {
        return false;
    }
    // keyframe 清空既有状态。
    state.push(geoworld::gateway::QueuedFrame{5, 0, true,
                                              geoworld::gateway::FrameBytes(16)});
    if (state.size() != 1) {
        return false;
    }

    // reliable queue：超限返回 false，不丢消息。
    ReliableQueue reliable{8};
    if (!reliable.push(geoworld::gateway::FrameBytes(8))) {
        return false;
    }
    if (reliable.push(geoworld::gateway::FrameBytes(1))) {
        return false;
    }
    return reliable.size() == 1;
}

[[nodiscard]] bool ack_epoch_guard() {
    TestRig rig{GatewayConfig{}};
    rig.core.set_frame_encoder([](const geoworld::projection::StateFrame&) {
        return geoworld::gateway::FrameBytes(8);
    });
    auto opened = rig.core.open_session("observer-token", 1, 1, 1, 1, 0);
    if (!rig.core.attach_stream(ConnectionId{1}, opened.session.stream_ticket)
             .has_value()) {
        return false;
    }
    // epoch 不匹配立即标记断开。
    if (rig.core.inbound_ack(ConnectionId{1}, 99, 1)) {
        return false;
    }
    if (!rig.core.must_disconnect(ConnectionId{1})
        || rig.core.disconnect_reason(ConnectionId{1})
            != GatewayError::epoch_mismatch) {
        return false;
    }
    return true;
}

[[nodiscard]] geoworld::projection::ProjectedEntity make_wire_probe_entity(
    std::uint64_t wid_value, std::uint64_t fid_value) {
    geoworld::projection::ProjectedEntity entity;
    entity.wid = WorldId{wid_value};
    entity.fid = geoworld::foundation::FeatureId{
        static_cast<std::uint32_t>(fid_value)};
    entity.version = wid_value * 7U + 1U;
    entity.position = geoworld::world::PositionEcef{
        static_cast<double>(wid_value) * 1.5, -2.0, 3.25};
    entity.semantic_type = wid_value % 2U == 0U ? "asset/语义/重复" : "test.entity";
    entity.geometry_ref = "asset/geo/" + std::to_string(wid_value);
    entity.lifecycle = geoworld::world::LifecycleState::active;
    entity.properties.emplace("speed", 12.5 + static_cast<double>(wid_value));
    entity.properties.emplace("count", static_cast<std::int64_t>(wid_value));
    entity.properties.emplace("flag", wid_value % 2U == 0U);
    entity.properties.emplace("label",
                              std::string("标签-") + std::to_string(wid_value % 3U));
    entity.state.emplace("mode", std::string{"idle"});
    entity.relations.push_back(geoworld::world::Relation{
        WorldId{wid_value + 1U}, "link-" + std::to_string(wid_value % 2U), {}});
    entity.capabilities = {"move", "sense"};
    entity.metadata.frequency = wid_value % 3U == 0U
        ? geoworld::projection::FrequencyClass::slow
        : geoworld::projection::FrequencyClass::fast;
    entity.metadata.priority = static_cast<std::uint32_t>(wid_value);
    entity.metadata.visibility_tags = {"public",
                                       "zone-" + std::to_string(wid_value % 2U)};
    return entity;
}

[[nodiscard]] bool frame_bytes_equal(const geoworld::gateway::FrameBytes& lhs,
                                     const std::vector<std::uint8_t>& rhs) {
    return lhs.size() == rhs.size()
        && (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

// 直编路径（ProjectedEntity -> FlatBuffers）与 wire 路径（to_wire_frame ->
// encode_server_frame）逐字节一致，含 builder 复用与 limits 拒绝一致性。
[[nodiscard]] bool direct_encoder_matches_wire_path_bytes() {
    const geoworld::protocol::ProtocolLimits limits{};
    const auto encoder = geoworld::gateway::make_frame_encoder(limits);
    const auto reference_of = [&limits](const geoworld::projection::StateFrame& frame) {
        return geoworld::protocol::encode_server_frame(
            geoworld::gateway::to_wire_frame(frame), limits);
    };

    // keyframe：覆盖四种属性类型、跨实体字符串去重、关系、能力、可见性标签与 Unicode。
    geoworld::projection::Keyframe keyframe;
    keyframe.stream_epoch = 3;
    keyframe.stream_sequence = 42;
    keyframe.snapshot_id = 1000;
    keyframe.baseline_snapshot_id = 0;
    keyframe.tick = 77;
    keyframe.simulation_time_us = 7'700'000;
    for (std::uint64_t wid = 1; wid <= 8; ++wid) {
        keyframe.entities.push_back(geoworld::projection::EntityEnter{
            WorldId{wid}, geoworld::foundation::FeatureId{
                static_cast<std::uint32_t>(100U + wid)},
            make_wire_probe_entity(wid, 100U + wid)});
    }
    const geoworld::projection::StateFrame keyframe_frame{keyframe};
    if (!frame_bytes_equal(encoder(keyframe_frame), reference_of(keyframe_frame))) {
        return false;
    }

    // 同一 encoder 复用 builder 编码第二帧（delta：enters/updates/leaves 全枚举），
    // 字节仍与新建 builder 的 wire 路径一致。
    geoworld::projection::Delta delta;
    delta.stream_epoch = 3;
    delta.stream_sequence = 43;
    delta.snapshot_id = 1001;
    delta.baseline_snapshot_id = 1000;
    delta.tick = 78;
    delta.simulation_time_us = 7'800'000;
    for (std::uint64_t wid = 20; wid <= 21; ++wid) {
        delta.enters.push_back(geoworld::projection::EntityEnter{
            WorldId{wid}, geoworld::foundation::FeatureId{
                static_cast<std::uint32_t>(200U + wid)},
            make_wire_probe_entity(wid, 200U + wid)});
        delta.updates.push_back(geoworld::projection::EntityUpdate{
            WorldId{wid}, geoworld::foundation::FeatureId{
                static_cast<std::uint32_t>(100U + wid)},
            make_wire_probe_entity(wid, 100U + wid)});
    }
    const geoworld::projection::LeaveReason reasons[] = {
        geoworld::projection::LeaveReason::out_of_area,
        geoworld::projection::LeaveReason::not_relevant,
        geoworld::projection::LeaveReason::destroyed,
        geoworld::projection::LeaveReason::policy_changed};
    for (std::uint64_t index = 0; index < 4; ++index) {
        delta.leaves.push_back(geoworld::projection::EntityLeave{
            WorldId{30U + index}, geoworld::foundation::FeatureId{
                static_cast<std::uint32_t>(300U + index)},
            reasons[index]});
    }
    const geoworld::projection::StateFrame delta_frame{delta};
    if (!frame_bytes_equal(encoder(delta_frame), reference_of(delta_frame))) {
        return false;
    }

    // limits 拒绝一致：超过单帧实体上限时两边都返回空。
    geoworld::protocol::ProtocolLimits tight = limits;
    tight.max_entities_per_frame = 2;
    const auto tight_encoder = geoworld::gateway::make_frame_encoder(tight);
    if (!tight_encoder(keyframe_frame).empty()
        || !geoworld::protocol::encode_server_frame(
               geoworld::gateway::to_wire_frame(keyframe_frame), tight)
               .empty()) {
        return false;
    }

    // 超长字符串（含属性 key）拒绝一致。
    tight.max_entities_per_frame = limits.max_entities_per_frame;
    tight.max_string_bytes = 4;
    const auto tight_string_encoder = geoworld::gateway::make_frame_encoder(tight);
    if (!tight_string_encoder(delta_frame).empty()
        || !geoworld::protocol::encode_server_frame(
               geoworld::gateway::to_wire_frame(delta_frame), tight)
               .empty()) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!session_ticket_and_negotiation()) {
        return 1;
    }
    if (!ticket_expires()) {
        return 2;
    }
    if (!ownership_rules()) {
        return 3;
    }
    if (!command_admission_and_receipts()) {
        return 4;
    }
    if (!command_rate_limit()) {
        return 5;
    }
    if (!queue_backpressure()) {
        return 6;
    }
    if (!ack_epoch_guard()) {
        return 7;
    }
    if (!direct_encoder_matches_wire_path_bytes()) {
        return 8;
    }
    return 0;
}
