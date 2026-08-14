// M4-D 安全与恢复集成测试：ack 乱序/过旧/未知、基线淘汰 keyframe、慢客户端背压与断开、
// 畸形输入端到端、未授权与 ticket 重放/过期、TLS 策略与 loopback、geoworldd 日志脱敏。
// 组合方式与 m4_stream_test 一致：进程内 WorldRuntime + ProjectionEngine + GatewayCore
// + ControlServer + StreamTransport（loopback），客户端逻辑在独立线程做阻塞 IO。
#include "stream_client.hpp"

#include "geoworld/gateway/control_server.hpp"
#include "geoworld/gateway/errors.hpp"
#include "geoworld/gateway/frame_codec.hpp"
#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/gateway/stream_transport.hpp"
#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/replica.hpp"
#include "geoworld/protocol/version.hpp"
#include "geoworld/projection/engine.hpp"
#include "geoworld/runtime/world_runtime.hpp"

#include "geoworld_control.grpc.pb.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace pb = geoworld::control::v1;

using geoworld::client::StreamClient;
using geoworld::client::StreamClientConfig;
using geoworld::client::SubmitResult;
using geoworld::foundation::WorldId;
using geoworld::gateway::ControlServer;
using geoworld::gateway::ControlServerConfig;
using geoworld::gateway::FixtureAuthentication;
using geoworld::gateway::FixtureAuthorization;
using geoworld::gateway::GatewayConfig;
using geoworld::gateway::GatewayCore;
using geoworld::gateway::GatewayError;
using geoworld::gateway::StreamTransport;
using geoworld::gateway::TransportConfig;
using geoworld::projection::ConnectionId;
using geoworld::projection::ProjectionConfig;
using geoworld::projection::ProjectionEngine;
using geoworld::projection::ProjectionPolicy;

const geoworld::spatial::Geodetic kOriginGeodetic{31.0, 121.0, 0.0};
constexpr std::uint64_t kSeedWidBase = 7;
constexpr double kAoiExtentMeters = 1000.0;
constexpr std::uint64_t kOwnershipLeaseTicks = 100'000;
constexpr std::uint64_t kUnknownSnapshot = 9'999'999;
constexpr std::uint64_t kWrongEpochOffset = 1;
constexpr auto kTestDeadline = std::chrono::seconds{90};
constexpr auto kLoopSleep = std::chrono::milliseconds{1};
constexpr auto kClientIoTimeout = std::chrono::milliseconds{2'000};
constexpr auto kGrpcDeadline = std::chrono::seconds{5};
constexpr auto kConnectionCloseDeadline = std::chrono::seconds{10};
constexpr auto kDaemonStartupDeadline = std::chrono::seconds{15};
constexpr auto kDaemonRunDeadline = std::chrono::seconds{40};
constexpr auto kDaemonStopDeadline = std::chrono::seconds{5};
constexpr auto kSlowPause = std::chrono::milliseconds{1'000};
constexpr auto kTicketExpiryWait = std::chrono::milliseconds{1'200};
constexpr auto kDrainWait = std::chrono::milliseconds{100};
constexpr int kMaxReadAttempts = 64;
constexpr int kMaxCatchupReads = 512;
constexpr std::uint64_t kFloodCommandCount = 40;
constexpr std::uint64_t kRedactionRunTicks = 600;
constexpr std::string_view kCredentialToken = "observer-token";
constexpr std::string_view kWritableProperty = "speed";
constexpr std::string_view kCanaryToken = "canary-token-4d8f2c";
constexpr std::string_view kCanaryPayload = "canary-payload-value-77";

// 慢客户端场景的有界队列配置：帧约 13 KiB（64 实体），两项队列都只容得下一至两帧。
constexpr std::uint64_t kSlowEntityCount = 64;
constexpr std::size_t kSlowStateQueueBytes = 32U * 1024U;
constexpr std::size_t kSlowReliableQueueBytes = 512U;
constexpr std::size_t kSlowWriteBufferBytes = 8U * 1024U;

// 基线淘汰场景：未确认记录上限调小，客户端停止 ack 后必须很快回退 keyframe。
constexpr std::size_t kEvictMaxUnackedFrames = 3;

// 主循环每步推进一个仿真 tick（20ms），仿真时间流速远快于墙钟；
// 慢客户端/停 ack 场景必须显式调大 ack 超时，避免抢占背压路径的判定。
constexpr std::uint32_t kEvictAckTimeoutSeconds = 120;
constexpr std::uint32_t kSlowAckTimeoutSeconds = 300;

[[nodiscard]] geoworld::world::PositionEcef at_enu(double east, double north) {
    const geoworld::spatial::Ecef origin =
        geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
    const geoworld::spatial::Ecef point = geoworld::spatial::enu_to_ecef(
        origin, kOriginGeodetic, geoworld::spatial::Enu{east, north, 0.0});
    return geoworld::world::PositionEcef{point.x, point.y, point.z};
}

// 客户端线程唯一写入、主线程在 done 后读取；诊断只在首次失败时记录。
struct ScenarioState {
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::string diagnostic;
};

void fail(ScenarioState& state, const std::string& diagnostic) {
    if (state.diagnostic.empty()) {
        state.diagnostic = diagnostic;
    }
    state.failed.store(true, std::memory_order_release);
    state.done.store(true, std::memory_order_release);
}

// 进程内服务器组合：投影引擎 + 世界运行时 + GatewayCore + gRPC 控制面 + WebSocket 数据面。
// ControlServer/StreamTransport 不可移动，用 optional 就地构造。
struct ServerRig {
    ProjectionEngine engine;
    std::shared_ptr<FixtureAuthentication> authentication;
    std::shared_ptr<FixtureAuthorization> authorization;
    geoworld::runtime::WorldRuntime runtime;
    GatewayCore core;
    std::optional<ControlServer> control;
    std::optional<StreamTransport> transport;
    std::uint64_t entity_count{};
    bool ok{};

    ServerRig(GatewayConfig gateway_config, ProjectionConfig projection_config,
              ControlServerConfig control_config, TransportConfig transport_config,
              std::uint64_t entities, std::string& diagnostic)
        : engine(make_projection_config(projection_config), make_policy()),
          authentication(std::make_shared<FixtureAuthentication>()),
          authorization(std::make_shared<FixtureAuthorization>()),
          core(std::move(gateway_config), engine, authentication, authorization,
               [] { return std::chrono::steady_clock::now(); },
               [counter = std::uint64_t{}]() mutable {
                   return "m4sec-ticket-" + std::to_string(++counter);
               },
               geoworld::protocol::control_api_version,
               geoworld::protocol::data_schema_version),
          entity_count(entities) {
        authentication->add_credential(std::string{kCredentialToken}, {"viewer", false});
        authorization->allow_writable_property(std::string{kWritableProperty});
        core.set_frame_encoder(geoworld::gateway::make_frame_encoder());
        core.set_receipt_encoder(geoworld::gateway::make_receipt_encoder());
        core.set_heartbeat_encoder(geoworld::gateway::make_heartbeat_encoder());
        core.set_command_submitter(
            [this](std::uint64_t target_tick,
                   geoworld::simulation::CommandPayload payload,
                   geoworld::simulation::CommandMeta meta) {
                return runtime.submit(target_tick, std::move(payload), meta);
            });
        runtime.add_projection_observer(
            [this](const geoworld::world::World& world, std::uint64_t tick,
                   std::uint64_t state_hash,
                   const geoworld::spatial::SpatialQuery* spatial) {
                static_cast<void>(spatial);
                engine.on_projection(world, tick, state_hash);
            });

        control_config.tick_source = [this] {
            return static_cast<std::uint64_t>(runtime.clock().tick());
        };
        control.emplace(core, std::move(control_config));
        if (!control->start(diagnostic)) {
            return;
        }
        transport.emplace(core, geoworld::protocol::ProtocolLimits{},
                          std::move(transport_config));
        if (!transport->start(diagnostic)) {
            control->shutdown();
            return;
        }

        for (std::uint64_t index = 0; index < entity_count; ++index) {
            geoworld::world::WorldObject seed;
            seed.id = WorldId{kSeedWidBase + index};
            seed.position = at_enu(10.0 + static_cast<double>(index % 8U) * 10.0,
                                   10.0 + static_cast<double>(index / 8U) * 10.0);
            seed.semantic_type = "test.entity";
            seed.lifecycle = geoworld::world::LifecycleState::active;
            seed.properties.emplace(std::string{kWritableProperty}, 1.0);
            static_cast<void>(runtime.submit(
                0, geoworld::simulation::CreateObjectCommand{seed}));
        }
        diagnostic.clear();
        ok = true;
    }

    static ProjectionConfig make_projection_config(ProjectionConfig config) {
        config.enu_origin = geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
        return config;
    }

    static ProjectionPolicy make_policy() {
        ProjectionPolicy policy;
        policy.allow_property(std::string{kWritableProperty});
        return policy;
    }

    void shutdown() {
        if (transport.has_value()) {
            transport->shutdown();
        }
        if (control.has_value()) {
            control->shutdown();
        }
    }
};

// 主循环驱动上下文：与 geoworldd 同序 step -> 回执 -> 控制面封送 -> pump -> transport poll。
// mutate_world 打开时每 tick 改写全部实体 speed，保证 delta 持续流动。
struct DriveContext {
    ServerRig& rig;
    bool mutate_world{};
    double mutation_value{2.0};
    std::set<std::uint64_t> known_connections;
    std::set<std::uint64_t> reason_recorded;
    std::vector<GatewayError> disconnect_reasons;
};

void drive_once(DriveContext& context) {
    ServerRig& rig = context.rig;
    const geoworld::runtime::StepResult step = rig.runtime.step();
    rig.core.on_commands_applied(step.commands);
    rig.control->poll();
    rig.core.pump(step.tick);
    rig.transport->poll();

    for (const ConnectionId connection : rig.transport->active_connections()) {
        context.known_connections.insert(connection.value);
    }
    for (const std::uint64_t id : context.known_connections) {
        const ConnectionId connection{id};
        if (!context.reason_recorded.contains(id)
            && rig.core.must_disconnect(connection)) {
            context.reason_recorded.insert(id);
            context.disconnect_reasons.push_back(
                rig.core.disconnect_reason(connection));
        }
    }

    if (context.mutate_world) {
        context.mutation_value += 1.0;
        for (std::uint64_t index = 0; index < rig.entity_count; ++index) {
            static_cast<void>(rig.runtime.submit(
                step.tick + 1,
                geoworld::simulation::SetPropertyCommand{
                    WorldId{kSeedWidBase + index}, std::string{kWritableProperty},
                    context.mutation_value}));
        }
    }
    std::this_thread::sleep_for(kLoopSleep);
}

[[nodiscard]] bool drive_until_done(DriveContext& context, const ScenarioState& state) {
    const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (!state.done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        drive_once(context);
    }
    return state.done.load(std::memory_order_acquire);
}

void drive_for(DriveContext& context, std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        drive_once(context);
    }
}

void drive_io_once(DriveContext& context) {
    context.rig.control->poll();
    context.rig.transport->poll();
    std::this_thread::sleep_for(kLoopSleep);
}

[[nodiscard]] bool reasons_contain(const DriveContext& context, GatewayError error) {
    for (const GatewayError reason : context.disconnect_reasons) {
        if (reason == error) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] StreamClientConfig make_client_config(const ServerRig& rig) {
    StreamClientConfig config;
    config.control_address =
        "127.0.0.1:" + std::to_string(rig.control->bound_port());
    config.stream_port = rig.transport->bound_port();
    config.credential_token = std::string{kCredentialToken};
    config.io_timeout = kClientIoTimeout;
    return config;
}

[[nodiscard]] bool open_and_subscribe(StreamClient& client, ScenarioState& state) {
    std::string diagnostic;
    if (!client.open_session(diagnostic)) {
        fail(state, "open_session: " + diagnostic);
        return false;
    }
    geoworld::projection::Subscription subscription;
    subscription.area = geoworld::spatial::Aabb{
        geoworld::spatial::Enu{-kAoiExtentMeters, -kAoiExtentMeters, -kAoiExtentMeters},
        geoworld::spatial::Enu{kAoiExtentMeters, kAoiExtentMeters, kAoiExtentMeters},
    };
    if (!client.update_subscription(subscription, diagnostic)) {
        fail(state, "update_subscription: " + diagnostic);
        return false;
    }
    return true;
}

[[nodiscard]] bool read_until_keyframe(StreamClient& client, ScenarioState& state,
                                       geoworld::protocol::WireKeyframe& out,
                                       int max_reads) {
    for (int attempt = 0; attempt < max_reads; ++attempt) {
        std::string diagnostic;
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail(state, "read_frame(keyframe): " + diagnostic);
            return false;
        }
        if (const auto* keyframe =
                std::get_if<geoworld::protocol::WireKeyframe>(&*frame)) {
            out = *keyframe;
            return true;
        }
    }
    fail(state, "限定帧数内未收到 keyframe");
    return false;
}

// 读取直到 snapshot 大于 after_snapshot 的 keyframe：跳过冻结前已入队的旧 keyframe。
[[nodiscard]] bool read_until_keyframe_after(StreamClient& client,
                                             ScenarioState& state,
                                             geoworld::protocol::WireKeyframe& out,
                                             std::uint64_t after_snapshot,
                                             int max_reads) {
    for (int attempt = 0; attempt < max_reads; ++attempt) {
        std::string diagnostic;
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail(state, "read_frame(keyframe_after): " + diagnostic);
            return false;
        }
        if (const auto* keyframe =
                std::get_if<geoworld::protocol::WireKeyframe>(&*frame);
            keyframe != nullptr && keyframe->snapshot_id > after_snapshot) {
            out = *keyframe;
            return true;
        }
    }
    fail(state, "限定帧数内未收到新 keyframe");
    return false;
}

[[nodiscard]] bool read_until_delta(StreamClient& client, ScenarioState& state,
                                    geoworld::protocol::WireDelta& out,
                                    std::uint64_t after_snapshot, int max_reads) {
    for (int attempt = 0; attempt < max_reads; ++attempt) {
        std::string diagnostic;
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail(state, "read_frame(delta): " + diagnostic);
            return false;
        }
        if (const auto* delta = std::get_if<geoworld::protocol::WireDelta>(&*frame);
            delta != nullptr && delta->snapshot_id > after_snapshot) {
            out = *delta;
            return true;
        }
    }
    fail(state, "限定帧数内未收到新 delta");
    return false;
}

[[nodiscard]] bool send_ack(StreamClient& client, ScenarioState& state,
                            std::uint64_t epoch, std::uint64_t snapshot) {
    std::string diagnostic;
    if (!client.send_ack(epoch, snapshot, diagnostic)) {
        fail(state, "send_ack: " + diagnostic);
        return false;
    }
    return true;
}

// 连接被服务器关闭后 read_frame 必须失败；在墙钟期限内排空任意数量的
// 在途帧，避免把生产者吞吐差异误当成关闭语义。
[[nodiscard]] bool expect_connection_closed(StreamClient& client, ScenarioState& state) {
    const auto deadline = std::chrono::steady_clock::now() + kConnectionCloseDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string diagnostic;
        if (!client.read_frame(diagnostic).has_value()) {
            return true;
        }
    }
    fail(state, "连接未在预期内被服务器关闭");
    return false;
}

[[nodiscard]] std::optional<geoworld::gateway::SessionId> parse_session(
    const std::string& text) {
    geoworld::gateway::SessionId id;
    id.value = std::stoull(text);
    return id.valid() ? std::optional{id} : std::nullopt;
}

// ---- 场景 1：ack 乱序/过旧幂等，未知 snapshot 与错误 epoch 断开，服务器继续服务 ----

void ack_rules_client(StreamClientConfig config, ScenarioState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    std::string diagnostic;
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream: " + diagnostic);
        return;
    }

    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    const std::uint64_t epoch = keyframe.stream_epoch;
    const std::uint64_t s1 = keyframe.snapshot_id;
    if (!send_ack(client, state, epoch, s1)) {
        return;
    }
    geoworld::protocol::WireDelta delta;
    if (!read_until_delta(client, state, delta, s1, kMaxCatchupReads)) {
        return;
    }
    const std::uint64_t s2 = delta.snapshot_id;
    if (!read_until_delta(client, state, delta, s2, kMaxCatchupReads)) {
        return;
    }
    const std::uint64_t s3 = delta.snapshot_id;

    // 乱序与过旧 ack：全部幂等，连接不得被误判断开。
    if (!send_ack(client, state, epoch, s3) || !send_ack(client, state, epoch, s2)
        || !send_ack(client, state, epoch, s1)) {
        return;
    }
    if (!read_until_delta(client, state, delta, s3, kMaxCatchupReads)) {
        return;
    }
    if (!send_ack(client, state, epoch, delta.snapshot_id)
        || !send_ack(client, state, epoch, s1)) {
        return;
    }

    // 未知 snapshot ack：协议错误，服务器必须断开该连接。
    if (!send_ack(client, state, epoch, kUnknownSnapshot)) {
        return;
    }
    if (!expect_connection_closed(client, state)) {
        return;
    }

    // 错误 epoch ack：必须断开。
    if (!open_and_subscribe(client, state)) {
        return;
    }
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream(2): " + diagnostic);
        return;
    }
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    if (!send_ack(client, state, keyframe.stream_epoch + kWrongEpochOffset,
                  keyframe.snapshot_id)) {
        return;
    }
    if (!expect_connection_closed(client, state)) {
        return;
    }

    // 服务器仍存活并服务新连接。
    if (!open_and_subscribe(client, state)) {
        return;
    }
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream(3): " + diagnostic);
        return;
    }
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    client.disconnect_stream();
    state.done.store(true, std::memory_order_release);
}

[[nodiscard]] bool scenario_ack_rules() {
    std::string diagnostic;
    ControlServerConfig control_config;
    control_config.port = 0;
    TransportConfig transport_config;
    transport_config.port = 0;
    ServerRig rig{GatewayConfig{}, ProjectionConfig{}, control_config,
                  transport_config, 2, diagnostic};
    if (!rig.ok) {
        std::cerr << "scenario_ack_rules rig: " << diagnostic << '\n';
        return false;
    }

    ScenarioState state;
    DriveContext context{rig, true};
    std::thread client_thread{ack_rules_client, make_client_config(rig),
                              std::ref(state)};
    if (!drive_until_done(context, state)) {
        fail(state, "场景超时");
    }
    if (client_thread.joinable()) {
        client_thread.join();
    }
    rig.shutdown();

    if (state.failed.load(std::memory_order_acquire)) {
        std::cerr << "scenario_ack_rules: " << state.diagnostic << '\n';
        return false;
    }
    if (!reasons_contain(context, GatewayError::ack_unknown)) {
        std::cerr << "scenario_ack_rules: 未记录未知 ack 断开原因\n";
        return false;
    }
    if (!reasons_contain(context, GatewayError::epoch_mismatch)) {
        std::cerr << "scenario_ack_rules: 未记录 epoch 不匹配断开原因\n";
        return false;
    }
    return true;
}

// ---- 场景 2：停止 ack 导致未确认记录淘汰，服务器自动回退 keyframe ----

void eviction_client(StreamClientConfig config, ScenarioState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    std::string diagnostic;
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream: " + diagnostic);
        return;
    }
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    if (!send_ack(client, state, keyframe.stream_epoch, keyframe.snapshot_id)) {
        return;
    }
    // 不再 ack：未确认记录超过上限后，下一状态帧必须是 keyframe（baseline = 0）。
    for (int attempt = 0; attempt < kMaxCatchupReads; ++attempt) {
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail(state, "read_frame(evict): " + diagnostic);
            return;
        }
        if (const auto* next = std::get_if<geoworld::protocol::WireKeyframe>(&*frame);
            next != nullptr && next->baseline_snapshot_id == 0) {
            state.done.store(true, std::memory_order_release);
            client.disconnect_stream();
            return;
        }
    }
    fail(state, "未确认淘汰后未收到自动 keyframe");
}

[[nodiscard]] bool scenario_baseline_eviction_keyframe() {
    std::string diagnostic;
    ProjectionConfig projection_config;
    projection_config.max_unacked_frames = kEvictMaxUnackedFrames;
    GatewayConfig gateway_config;
    gateway_config.ack_timeout_seconds = kEvictAckTimeoutSeconds;
    ControlServerConfig control_config;
    control_config.port = 0;
    TransportConfig transport_config;
    transport_config.port = 0;
    ServerRig rig{gateway_config, projection_config, control_config,
                  transport_config, 2, diagnostic};
    if (!rig.ok) {
        std::cerr << "scenario_baseline_eviction_keyframe rig: " << diagnostic << '\n';
        return false;
    }

    ScenarioState state;
    DriveContext context{rig, true};
    std::thread client_thread{eviction_client, make_client_config(rig),
                              std::ref(state)};
    if (!drive_until_done(context, state)) {
        fail(state, "场景超时");
    }
    if (client_thread.joinable()) {
        client_thread.join();
    }
    rig.shutdown();
    if (state.failed.load(std::memory_order_acquire)) {
        std::cerr << "scenario_baseline_eviction_keyframe: " << state.diagnostic << '\n';
        return false;
    }
    return true;
}

// ---- 场景 3：慢客户端。A：state queue 合并/淘汰后强制 keyframe，恢复后 hash 一致；
// B：reliable queue 满必须断开慢连接并清扫，健康连接不受影响 ----

struct SlowResumeState : ScenarioState {
    std::string session_id;
    std::atomic<int> stage{0};
    // 静止 keyframe 的 snapshot 必须大于该值；主线程在安排 keyframe 时记录，
    // 用于跳过冻结前已入队的旧 keyframe。
    std::uint64_t keyframe_after_snapshot{};
    std::uint64_t replica_hash{};
};

void slow_resume_client(StreamClientConfig config, SlowResumeState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    state.session_id = client.session().session_id;
    std::string diagnostic;
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream: " + diagnostic);
        return;
    }
    geoworld::protocol::ReplicaAccumulator replica;
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    replica.apply(keyframe);
    if (!send_ack(client, state, keyframe.stream_epoch, keyframe.snapshot_id)) {
        return;
    }

    // 慢读窗口：不读不 ack，内核/写缓冲/核心 state queue 依次填满并触发重同步。
    std::this_thread::sleep_for(kSlowPause);

    bool forced_keyframe = false;
    for (int attempt = 0; attempt < kMaxCatchupReads && !forced_keyframe; ++attempt) {
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail(state, "read_frame(resume): " + diagnostic);
            return;
        }
        if (const auto* next = std::get_if<geoworld::protocol::WireKeyframe>(&*frame);
            next != nullptr && next->baseline_snapshot_id == 0) {
            forced_keyframe = true;
        }
    }
    if (!forced_keyframe) {
        fail(state, "恢复读取后未收到强制 keyframe");
        return;
    }
    state.stage.store(1, std::memory_order_release);

    // 主线程停止世界变化并安排静止 keyframe 后，客户端追到一致状态。
    const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (state.stage.load(std::memory_order_acquire) < 2) {
        if (std::chrono::steady_clock::now() > deadline) {
            fail(state, "等待静止 keyframe 安排超时");
            return;
        }
        std::this_thread::sleep_for(kLoopSleep);
    }
    if (!read_until_keyframe_after(client, state, keyframe,
                                   state.keyframe_after_snapshot,
                                   kMaxCatchupReads)) {
        return;
    }
    replica.apply(keyframe);
    static_cast<void>(client.send_ack(keyframe.stream_epoch, keyframe.snapshot_id,
                                      diagnostic));
    state.replica_hash = replica.hash();
    state.done.store(true, std::memory_order_release);
    // 等主线程完成 hash 比对后再断开，避免连接提前被清扫。
    const auto disconnect_deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (state.stage.load(std::memory_order_acquire) < 3
           && std::chrono::steady_clock::now() < disconnect_deadline) {
        std::this_thread::sleep_for(kLoopSleep);
    }
    client.disconnect_stream();
}

struct ReliableOverflowState : ScenarioState {
    std::atomic<std::uint64_t> healthy_frames{0};
    std::atomic<bool> stop_healthy{false};
    std::atomic<int> overflow_stage{0};
};

void healthy_client(StreamClientConfig config, ReliableOverflowState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    std::string diagnostic;
    if (!client.connect_stream(diagnostic)) {
        fail(state, "healthy connect_stream: " + diagnostic);
        return;
    }
    while (!state.stop_healthy.load(std::memory_order_acquire)) {
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail(state, "healthy read_frame: " + diagnostic);
            return;
        }
        if (const auto* keyframe =
                std::get_if<geoworld::protocol::WireKeyframe>(&*frame)) {
            static_cast<void>(client.send_ack(keyframe->stream_epoch,
                                              keyframe->snapshot_id, diagnostic));
        } else if (const auto* delta =
                       std::get_if<geoworld::protocol::WireDelta>(&*frame)) {
            static_cast<void>(client.send_ack(delta->stream_epoch,
                                              delta->snapshot_id, diagnostic));
        }
        state.healthy_frames.fetch_add(1, std::memory_order_relaxed);
    }
    client.disconnect_stream();
    state.done.store(true, std::memory_order_release);
}

void reliable_overflow_client(StreamClientConfig config, ReliableOverflowState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    std::string diagnostic;
    if (!client.acquire_ownership(WorldId{kSeedWidBase}, {std::string{kWritableProperty}},
                                  client.session().current_tick + kOwnershipLeaseTicks,
                                  diagnostic)) {
        fail(state, "acquire_ownership: " + diagnostic);
        return;
    }
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream: " + diagnostic);
        return;
    }
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    if (!send_ack(client, state, keyframe.stream_epoch, keyframe.snapshot_id)) {
        return;
    }
    state.overflow_stage.store(1, std::memory_order_release);
    const auto freeze_deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (state.overflow_stage.load(std::memory_order_acquire) < 2) {
        if (std::chrono::steady_clock::now() > freeze_deadline) {
            fail(state, "等待 tick 边界冻结超时");
            return;
        }
        std::this_thread::sleep_for(kLoopSleep);
    }
    // 先停读形成堵塞：大帧填满内核与传输写缓冲后，核心队列才开始积压。
    std::this_thread::sleep_for(kSlowPause);
    // 冻结 tick 期间只发命令不读流；命令进入同一目标 tick，终态回执在相位
    // 边界成批产生，确保测试的是 reliable queue 上限而非操作系统缓冲大小。
    std::uint64_t accepted_count = 0;
    for (std::uint64_t sequence = 1; sequence <= kFloodCommandCount; ++sequence) {
        const std::optional<SubmitResult> result = client.submit_set_property(
            WorldId{kSeedWidBase}, std::string{kWritableProperty},
            static_cast<double>(sequence), sequence, 0, diagnostic);
        if (!result.has_value()) {
            fail(state, "submit_set_property: " + diagnostic);
            return;
        }
        if (result->status == geoworld::gateway::ReceiptStatus::accepted) {
            ++accepted_count;
        }
    }
    if (accepted_count != kFloodCommandCount) {
        fail(state, "回执洪水命令未全部 accepted");
        return;
    }
    state.overflow_stage.store(3, std::memory_order_release);
    if (!expect_connection_closed(client, state)) {
        return;
    }
    state.done.store(true, std::memory_order_release);
}

[[nodiscard]] bool scenario_slow_client() {
    std::string diagnostic;
    GatewayConfig gateway_config;
    gateway_config.max_state_queue_bytes = kSlowStateQueueBytes;
    gateway_config.max_reliable_queue_bytes = kSlowReliableQueueBytes;
    gateway_config.ack_timeout_seconds = kSlowAckTimeoutSeconds;
    ControlServerConfig control_config;
    control_config.port = 0;
    TransportConfig transport_config;
    transport_config.port = 0;
    transport_config.max_write_buffer_bytes = kSlowWriteBufferBytes;
    ServerRig rig{gateway_config, ProjectionConfig{}, control_config,
                  transport_config, kSlowEntityCount, diagnostic};
    if (!rig.ok) {
        std::cerr << "scenario_slow_client rig: " << diagnostic << '\n';
        return false;
    }

    // A：state queue 淘汰后强制 keyframe，恢复后 replica hash 与服务端视图一致。
    {
        SlowResumeState state;
        DriveContext context{rig, true};
        std::thread client_thread{slow_resume_client, make_client_config(rig),
                                  std::ref(state)};
        const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
        int handled_stage = 0;
        while (!state.done.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            drive_once(context);
            const int stage = state.stage.load(std::memory_order_acquire);
            if (stage > handled_stage) {
                handled_stage = stage;
                if (stage == 1) {
                    // 停止世界变化，排空在途帧后安排静止 keyframe，供 hash 精确比对。
                    // 记录当前 snapshot：静止 keyframe 的 snapshot 必须更大，
                    // 客户端据此跳过冻结前已入队的旧 keyframe。
                    context.mutate_world = false;
                    drive_for(context, kDrainWait);
                    state.keyframe_after_snapshot = rig.engine.latest_snapshot_id();
                    const std::optional<geoworld::gateway::SessionId> session =
                        parse_session(state.session_id);
                    if (!session.has_value()
                        || rig.core.request_keyframe(*session) != GatewayError::none) {
                        fail(state, "静止 keyframe 安排失败");
                        break;
                    }
                    state.stage.store(2, std::memory_order_release);
                }
            }
        }
        if (!state.done.load(std::memory_order_acquire)) {
            fail(state, "慢客户端恢复场景超时");
        }
        bool resume_ok = !state.failed.load(std::memory_order_acquire);
        if (resume_ok) {
            // 客户端在 stage 3 前保持连接，此处比对时连接仍然存活。
            const std::vector<ConnectionId> active = rig.transport->active_connections();
            if (active.size() != 1) {
                std::cerr << "scenario_slow_client(resume): 连接数异常\n";
                resume_ok = false;
            } else if (rig.engine.connection_view_hash(active[0])
                       != state.replica_hash) {
                std::cerr << "scenario_slow_client(resume): 恢复后 hash 与服务端视图不一致 server="
                          << rig.engine.connection_view_hash(active[0])
                          << " replica=" << state.replica_hash << '\n';
                resume_ok = false;
            }
        }
        state.stage.store(3, std::memory_order_release);
        if (client_thread.joinable()) {
            client_thread.join();
        }
        if (!resume_ok) {
            if (!state.diagnostic.empty()) {
                std::cerr << "scenario_slow_client(resume): " << state.diagnostic
                          << '\n';
            }
            rig.shutdown();
            return false;
        }
        drive_for(context, kDrainWait);
    }

    // B：reliable queue 满断开慢客户端并清扫，健康连接持续收帧。
    {
        ReliableOverflowState slow_state;
        ReliableOverflowState healthy_state;
        DriveContext context{rig, true};
        std::thread healthy_thread{healthy_client, make_client_config(rig),
                                   std::ref(healthy_state)};
        std::thread slow_thread{reliable_overflow_client, make_client_config(rig),
                                std::ref(slow_state)};
        const auto freeze_deadline = std::chrono::steady_clock::now() + kTestDeadline;
        while (slow_state.overflow_stage.load(std::memory_order_acquire) < 1
               && !slow_state.done.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < freeze_deadline) {
            drive_once(context);
        }
        if (!slow_state.done.load(std::memory_order_acquire)) {
            slow_state.overflow_stage.store(2, std::memory_order_release);
            while (slow_state.overflow_stage.load(std::memory_order_acquire) < 3
                   && !slow_state.done.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < freeze_deadline) {
                drive_io_once(context);
            }
        }
        if (!slow_state.done.load(std::memory_order_acquire)
            && slow_state.overflow_stage.load(std::memory_order_acquire) >= 3) {
            static_cast<void>(drive_until_done(context, slow_state));
        }
        if (!slow_state.done.load(std::memory_order_acquire)) {
            fail(slow_state, "reliable 溢出场景超时");
        }
        if (slow_thread.joinable()) {
            slow_thread.join();
        }
        if (!slow_state.failed.load(std::memory_order_acquire)) {
            // 等慢连接清扫完毕：只剩健康连接。
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (rig.core.connection_count() != 1
                   && std::chrono::steady_clock::now() < deadline) {
                drive_once(context);
            }
        }
        const std::size_t connections_after_cleanup = rig.core.connection_count();
        healthy_state.stop_healthy.store(true, std::memory_order_release);
        if (healthy_thread.joinable()) {
            healthy_thread.join();
        }
        rig.shutdown();

        if (slow_state.failed.load(std::memory_order_acquire)) {
            std::cerr << "scenario_slow_client(overflow): " << slow_state.diagnostic
                      << '\n';
            return false;
        }
        if (healthy_state.failed.load(std::memory_order_acquire)) {
            std::cerr << "scenario_slow_client(healthy): " << healthy_state.diagnostic
                      << '\n';
            return false;
        }
        if (connections_after_cleanup != 1) {
            std::cerr << "scenario_slow_client(overflow): 慢连接断开后未被清扫\n";
            return false;
        }
        if (!reasons_contain(context, GatewayError::slow_client)) {
            std::cerr << "scenario_slow_client(overflow): 未记录慢客户端断开原因\n";
            return false;
        }
        if (healthy_state.healthy_frames.load(std::memory_order_relaxed) == 0) {
            std::cerr << "scenario_slow_client(healthy): 健康连接未收到帧\n";
            return false;
        }
    }
    return true;
}

// ---- 场景 4：畸形输入端到端 + gRPC 无效字段稳定错误码 ----

struct MalformedState : ScenarioState {
    std::string control_address;
};

[[nodiscard]] bool malformed_case(StreamClient& client, ScenarioState& state,
                                  std::span<const std::uint8_t> payload,
                                  bool expect_error_frame) {
    std::string diagnostic;
    if (!open_and_subscribe(client, state)) {
        return false;
    }
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream: " + diagnostic);
        return false;
    }
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return false;
    }
    // 超大帧可能在服务器关闭后写失败，写结果不作为判据。
    static_cast<void>(client.send_raw_bytes(payload, diagnostic));
    bool error_frame_seen = false;
    bool connection_closed = false;
    for (int attempt = 0; attempt < kMaxReadAttempts; ++attempt) {
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            connection_closed = true;
            break;
        }
        if (const auto* reliable =
                std::get_if<geoworld::protocol::WireReliable>(&*frame);
            reliable != nullptr
            && reliable->kind == geoworld::protocol::ReliableKind::protocol_error) {
            if (reliable->error.code.rfind("GWG", 0) != 0) {
                fail(state, "协议错误帧缺少稳定错误码");
                return false;
            }
            error_frame_seen = true;
        }
    }
    if (!connection_closed) {
        fail(state, "畸形输入后连接未被关闭");
        return false;
    }
    if (expect_error_frame && !error_frame_seen) {
        fail(state, "畸形输入后未收到协议错误帧");
        return false;
    }
    client.disconnect_stream();
    return true;
}

void malformed_client(StreamClientConfig config, MalformedState& state) {
    StreamClient client{std::move(config)};

    const std::vector<std::uint8_t> garbage(64, 0xAB);
    if (!malformed_case(client, state, garbage, true)) {
        return;
    }

    std::vector<std::uint8_t> wrong_identifier = geoworld::protocol::encode_client_control(
        geoworld::protocol::WireClientControl{geoworld::protocol::WireAck{1, 1}});
    // FlatBuffers file identifier 位于偏移 4..8。
    wrong_identifier[4] = 'X';
    wrong_identifier[5] = 'X';
    wrong_identifier[6] = 'X';
    wrong_identifier[7] = 'X';
    if (!malformed_case(client, state, wrong_identifier, true)) {
        return;
    }

    std::vector<std::uint8_t> truncated = geoworld::protocol::encode_client_control(
        geoworld::protocol::WireClientControl{geoworld::protocol::WireAck{1, 1}});
    truncated.resize(truncated.size() / 2U);
    if (!malformed_case(client, state, truncated, true)) {
        return;
    }

    // 超过 8 MiB 硬上限：Beast read_message_max 直接断开，不要求协议错误帧。
    const geoworld::protocol::ProtocolLimits limits;
    const std::vector<std::uint8_t> oversized(limits.max_frame_bytes + 64U, 0xCD);
    if (!malformed_case(client, state, oversized, false)) {
        return;
    }

    // 服务器存活：新会话仍能完成握手并收到 keyframe。
    geoworld::protocol::WireKeyframe keyframe;
    std::string diagnostic;
    if (!open_and_subscribe(client, state) || !client.connect_stream(diagnostic)) {
        fail(state, "畸形输入后服务器不再服务新连接: " + diagnostic);
        return;
    }
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    client.disconnect_stream();

    // gRPC 侧无效字段：稳定错误码。ClientContext 不可拷贝/移动，逐次就地构造。
    auto stub = pb::GeoWorldControl::NewStub(grpc::CreateChannel(
        state.control_address, grpc::InsecureChannelCredentials()));

    pb::OpenSessionRequest bad_auth;
    bad_auth.set_credential_token("wrong-token");
    bad_auth.mutable_control_versions()->set_minimum(
        geoworld::protocol::control_api_version);
    bad_auth.mutable_control_versions()->set_maximum(
        geoworld::protocol::control_api_version);
    bad_auth.mutable_data_versions()->set_minimum(
        geoworld::protocol::data_schema_version);
    bad_auth.mutable_data_versions()->set_maximum(
        geoworld::protocol::data_schema_version);
    pb::OpenSessionResponse bad_auth_response;
    grpc::ClientContext auth_context;
    auth_context.set_deadline(std::chrono::system_clock::now() + kGrpcDeadline);
    if (!stub->OpenSession(&auth_context, bad_auth, &bad_auth_response).ok()
        || bad_auth_response.error().code() != "GWG002") {
        fail(state, "OpenSession 无效凭证未返回 GWG002");
        return;
    }

    pb::OpenSessionRequest bad_version = bad_auth;
    bad_version.set_credential_token(std::string{kCredentialToken});
    bad_version.mutable_control_versions()->set_minimum(2);
    bad_version.mutable_control_versions()->set_maximum(3);
    pb::OpenSessionResponse bad_version_response;
    grpc::ClientContext version_context;
    version_context.set_deadline(std::chrono::system_clock::now() + kGrpcDeadline);
    if (!stub->OpenSession(&version_context, bad_version, &bad_version_response).ok()
        || bad_version_response.error().code() != "GWG001") {
        fail(state, "OpenSession 版本不兼容未返回 GWG001");
        return;
    }

    pb::OpenSessionResponse opened;
    grpc::ClientContext open_context;
    open_context.set_deadline(std::chrono::system_clock::now() + kGrpcDeadline);
    pb::OpenSessionRequest open_request = bad_auth;
    open_request.set_credential_token(std::string{kCredentialToken});
    if (!stub->OpenSession(&open_context, open_request, &opened).ok()
        || opened.session_id().empty()) {
        fail(state, "OpenSession 合法请求失败");
        return;
    }

    pb::SubmitCommandRequest two_params;
    two_params.set_session_id(opened.session_id());
    two_params.set_client_sequence(1);
    two_params.set_target_wid(kSeedWidBase);
    two_params.set_operation(pb::SET_PROPERTY);
    (*two_params.mutable_parameters())["a"].set_double_value(1.0);
    (*two_params.mutable_parameters())["b"].set_double_value(2.0);
    pb::SubmitCommandResponse two_params_response;
    grpc::ClientContext two_params_context;
    two_params_context.set_deadline(std::chrono::system_clock::now() + kGrpcDeadline);
    if (!stub->SubmitCommand(&two_params_context, two_params, &two_params_response).ok()
        || two_params_response.status() != pb::REJECTED
        || two_params_response.error().code() != "GWG005") {
        fail(state, "SubmitCommand 参数数量非法未返回 GWG005");
        return;
    }

    pb::SubmitCommandRequest bad_operation;
    bad_operation.set_session_id(opened.session_id());
    bad_operation.set_client_sequence(2);
    bad_operation.set_target_wid(kSeedWidBase);
    bad_operation.set_operation(pb::COMMAND_OPERATION_UNSPECIFIED);
    pb::SubmitCommandResponse bad_operation_response;
    grpc::ClientContext bad_operation_context;
    bad_operation_context.set_deadline(std::chrono::system_clock::now() + kGrpcDeadline);
    if (!stub->SubmitCommand(&bad_operation_context, bad_operation,
                             &bad_operation_response)
             .ok()
        || bad_operation_response.status() != pb::REJECTED
        || bad_operation_response.error().code() != "GWG204") {
        fail(state, "SubmitCommand 未知 operation 未返回 GWG204");
        return;
    }

    pb::SubmitCommandRequest bogus_session = bad_operation;
    bogus_session.set_session_id("not-a-session");
    bogus_session.set_operation(pb::DESTROY_OBJECT);
    pb::SubmitCommandResponse bogus_session_response;
    grpc::ClientContext bogus_session_context;
    bogus_session_context.set_deadline(std::chrono::system_clock::now() + kGrpcDeadline);
    if (!stub->SubmitCommand(&bogus_session_context, bogus_session,
                             &bogus_session_response)
             .ok()
        || bogus_session_response.status() != pb::REJECTED
        || bogus_session_response.error().code() != "GWG005") {
        fail(state, "SubmitCommand 非法 session 未返回 GWG005");
        return;
    }
    state.done.store(true, std::memory_order_release);
}

[[nodiscard]] bool scenario_malformed_input() {
    std::string diagnostic;
    ControlServerConfig control_config;
    control_config.port = 0;
    TransportConfig transport_config;
    transport_config.port = 0;
    ServerRig rig{GatewayConfig{}, ProjectionConfig{}, control_config,
                  transport_config, 1, diagnostic};
    if (!rig.ok) {
        std::cerr << "scenario_malformed_input rig: " << diagnostic << '\n';
        return false;
    }

    MalformedState state;
    state.control_address = "127.0.0.1:" + std::to_string(rig.control->bound_port());
    DriveContext context{rig, false};
    std::thread client_thread{malformed_client, make_client_config(rig),
                              std::ref(state)};
    if (!drive_until_done(context, state)) {
        fail(state, "场景超时");
    }
    if (client_thread.joinable()) {
        client_thread.join();
    }
    rig.shutdown();
    if (state.failed.load(std::memory_order_acquire)) {
        std::cerr << "scenario_malformed_input: " << state.diagnostic << '\n';
        return false;
    }
    return true;
}

// ---- 场景 5：未授权命令拒绝（GWG003）、ticket 网络层重放拒绝、过期 ticket 拒绝 ----

void authorization_client(StreamClientConfig config, ScenarioState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    std::string diagnostic;
    // 未持有所有权提交可写属性：GWG003。
    const std::optional<SubmitResult> denied = client.submit_set_property(
        WorldId{kSeedWidBase}, std::string{kWritableProperty}, 2.0, 1, 0, diagnostic);
    if (!denied.has_value()
        || denied->status != geoworld::gateway::ReceiptStatus::rejected
        || denied->error_code != "GWG003") {
        fail(state, "未授权命令未返回 GWG003: " + diagnostic);
        return;
    }

    // ticket 重放：首次握手消费 ticket，第二次同 ticket 必须失败。
    if (!client.connect_stream(diagnostic)) {
        fail(state, "connect_stream: " + diagnostic);
        return;
    }
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    const std::string replay_ticket = client.session().stream_ticket;
    client.disconnect_stream();
    if (client.connect_stream(diagnostic, replay_ticket)) {
        fail(state, "ticket 重放未被拒绝");
        return;
    }
    state.done.store(true, std::memory_order_release);
}

void expired_ticket_client(StreamClientConfig config, ScenarioState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    std::string diagnostic;
    std::this_thread::sleep_for(kTicketExpiryWait);
    if (client.connect_stream(diagnostic)) {
        fail(state, "过期 ticket 未被拒绝");
        return;
    }
    // 服务器存活：新会话新 ticket 立即成功。
    if (!open_and_subscribe(client, state)) {
        return;
    }
    if (!client.connect_stream(diagnostic)) {
        fail(state, "新 ticket 连接失败: " + diagnostic);
        return;
    }
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    client.disconnect_stream();
    state.done.store(true, std::memory_order_release);
}

[[nodiscard]] bool scenario_authorization_and_ticket() {
    std::string diagnostic;
    ControlServerConfig control_config;
    control_config.port = 0;
    TransportConfig transport_config;
    transport_config.port = 0;
    ServerRig rig{GatewayConfig{}, ProjectionConfig{}, control_config,
                  transport_config, 1, diagnostic};
    if (!rig.ok) {
        std::cerr << "scenario_authorization_and_ticket rig: " << diagnostic << '\n';
        return false;
    }
    {
        ScenarioState state;
        DriveContext context{rig, false};
        std::thread client_thread{authorization_client, make_client_config(rig),
                                  std::ref(state)};
        if (!drive_until_done(context, state)) {
            fail(state, "场景超时");
        }
        if (client_thread.joinable()) {
            client_thread.join();
        }
        if (state.failed.load(std::memory_order_acquire)) {
            std::cerr << "scenario_authorization_and_ticket: " << state.diagnostic
                      << '\n';
            rig.shutdown();
            return false;
        }
        drive_for(context, kDrainWait);
    }
    rig.shutdown();

    GatewayConfig short_ttl;
    short_ttl.stream_ticket_ttl_seconds = 1;
    ServerRig ttl_rig{short_ttl, ProjectionConfig{}, control_config,
                      transport_config, 1, diagnostic};
    if (!ttl_rig.ok) {
        std::cerr << "scenario_authorization_and_ticket ttl rig: " << diagnostic << '\n';
        return false;
    }
    {
        ScenarioState state;
        DriveContext context{ttl_rig, false};
        std::thread client_thread{expired_ticket_client, make_client_config(ttl_rig),
                                  std::ref(state)};
        if (!drive_until_done(context, state)) {
            fail(state, "场景超时");
        }
        if (client_thread.joinable()) {
            client_thread.join();
        }
        ttl_rig.shutdown();
        if (state.failed.load(std::memory_order_acquire)) {
            std::cerr << "scenario_authorization_and_ticket(expired): "
                      << state.diagnostic << '\n';
            return false;
        }
    }
    return true;
}

// ---- 场景 6：TLS 策略断言 + 自签证书 TLS loopback 闭环 ----

struct TlsState : ScenarioState {
    std::atomic<int> stage{0};
    std::uint64_t hash_after_keyframe{};
    bool receipt_applied{};
};

void tls_client(StreamClientConfig config, TlsState& state) {
    StreamClient client{std::move(config)};
    if (!open_and_subscribe(client, state)) {
        return;
    }
    std::string diagnostic;
    if (!client.acquire_ownership(WorldId{kSeedWidBase}, {std::string{kWritableProperty}},
                                  client.session().current_tick + kOwnershipLeaseTicks,
                                  diagnostic)) {
        fail(state, "acquire_ownership: " + diagnostic);
        return;
    }
    if (!client.connect_stream(diagnostic)) {
        fail(state, "TLS connect_stream: " + diagnostic);
        return;
    }
    geoworld::protocol::ReplicaAccumulator replica;
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        return;
    }
    replica.apply(keyframe);
    if (!send_ack(client, state, keyframe.stream_epoch, keyframe.snapshot_id)) {
        return;
    }
    state.hash_after_keyframe = replica.hash();
    state.stage.store(1, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (state.stage.load(std::memory_order_acquire) < 2) {
        if (std::chrono::steady_clock::now() > deadline) {
            fail(state, "等待命令阶段超时");
            return;
        }
        std::this_thread::sleep_for(kLoopSleep);
    }
    const std::optional<SubmitResult> result = client.submit_set_property(
        WorldId{kSeedWidBase}, std::string{kWritableProperty}, 9.5, 1, 1, diagnostic);
    if (!result.has_value()
        || result->status != geoworld::gateway::ReceiptStatus::accepted) {
        fail(state, "TLS submit_set_property: " + diagnostic);
        return;
    }
    for (int attempt = 0; attempt < kMaxCatchupReads; ++attempt) {
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail(state, "TLS read_frame: " + diagnostic);
            return;
        }
        if (const auto* delta = std::get_if<geoworld::protocol::WireDelta>(&*frame)) {
            replica.apply(*frame);
            static_cast<void>(client.send_ack(delta->stream_epoch,
                                              delta->snapshot_id, diagnostic));
        } else if (const auto* reliable =
                       std::get_if<geoworld::protocol::WireReliable>(&*frame)) {
            if (reliable->kind == geoworld::protocol::ReliableKind::command_receipt
                && reliable->receipt.status
                       == geoworld::protocol::ReceiptStatus::applied) {
                state.receipt_applied = true;
                state.done.store(true, std::memory_order_release);
                client.disconnect_stream();
                return;
            }
        }
    }
    fail(state, "TLS 闭环未收到 applied 回执");
}

[[nodiscard]] bool scenario_tls() {
    // loopback 判定：字面回环地址与 localhost 通过，通配与外部地址拒绝。
    using geoworld::gateway::is_loopback_address;
    if (!is_loopback_address("127.0.0.1") || !is_loopback_address("::1")
        || !is_loopback_address("localhost") || is_loopback_address("0.0.0.0")
        || is_loopback_address("192.0.2.1") || is_loopback_address("")) {
        std::cerr << "scenario_tls: loopback 判定异常\n";
        return false;
    }

    // 非 loopback 监听缺 TLS 证书：控制面与数据面都必须启动失败（校验在绑定之前）。
    {
        ProjectionEngine engine{
            ServerRig::make_projection_config(ProjectionConfig{}),
            ServerRig::make_policy()};
        auto authentication = std::make_shared<FixtureAuthentication>();
        auto authorization = std::make_shared<FixtureAuthorization>();
        GatewayCore core{
            GatewayConfig{}, engine, authentication, authorization,
            [] { return std::chrono::steady_clock::now(); },
            [] { return std::string{"tls-check-ticket"}; },
            geoworld::protocol::control_api_version,
            geoworld::protocol::data_schema_version};
        std::string diagnostic;
        ControlServerConfig bad_control;
        bad_control.listen_address = "0.0.0.0";
        bad_control.tick_source = [] { return std::uint64_t{}; };
        ControlServer bad_server{core, bad_control};
        if (bad_server.start(diagnostic) || diagnostic.empty()) {
            std::cerr << "scenario_tls: 非 loopback 控制面缺 TLS 未拒绝\n";
            return false;
        }
        TransportConfig bad_transport;
        bad_transport.listen_address = "192.0.2.1";
        StreamTransport bad_stream{core, geoworld::protocol::ProtocolLimits{},
                                   bad_transport};
        if (bad_stream.start(diagnostic) || diagnostic.empty()) {
            std::cerr << "scenario_tls: 非 loopback 数据面缺 TLS 未拒绝\n";
            return false;
        }
    }

    // 自签证书 TLS loopback 闭环；证书只写入临时目录，测试结束删除。
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "geoworld-m4sec-tls";
    std::filesystem::create_directories(temp_dir);
    const std::filesystem::path cert_file = temp_dir / "cert.pem";
    const std::filesystem::path key_file = temp_dir / "key.pem";
    const std::string openssl_command =
        "openssl req -x509 -newkey rsa:2048 -nodes -keyout \"" + key_file.string()
        + "\" -out \"" + cert_file.string()
        + "\" -days 1 -subj \"/CN=localhost\""
        + " -addext \"subjectAltName=IP:127.0.0.1,DNS:localhost\" >/dev/null 2>&1";
    if (std::system(openssl_command.c_str()) != 0
        || !std::filesystem::exists(cert_file) || !std::filesystem::exists(key_file)) {
        std::cerr << "scenario_tls: 自签证书生成失败\n";
        return false;
    }

    std::string diagnostic;
    ControlServerConfig control_config;
    control_config.port = 0;
    control_config.tls = geoworld::gateway::TlsCertificateFiles{cert_file.string(),
                                                                key_file.string()};
    TransportConfig transport_config;
    transport_config.port = 0;
    transport_config.tls = control_config.tls;
    ServerRig rig{GatewayConfig{}, ProjectionConfig{}, control_config,
                  transport_config, 1, diagnostic};
    if (!rig.ok) {
        std::cerr << "scenario_tls rig: " << diagnostic << '\n';
        std::filesystem::remove_all(temp_dir);
        return false;
    }

    TlsState state;
    DriveContext context{rig, false};
    StreamClientConfig client_config = make_client_config(rig);
    client_config.use_tls = true;
    client_config.tls_root_certificate_file = cert_file.string();
    std::thread client_thread{tls_client, client_config, std::ref(state)};
    const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
    int handled_stage = 0;
    while (!state.done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        drive_once(context);
        const int stage = state.stage.load(std::memory_order_acquire);
        if (stage > handled_stage) {
            handled_stage = stage;
            if (stage == 1) {
                const std::vector<ConnectionId> active =
                    rig.transport->active_connections();
                if (active.size() != 1
                    || rig.engine.connection_view_hash(active[0])
                           != state.hash_after_keyframe) {
                    fail(state, "TLS keyframe 后 replica hash 与服务端视图不一致");
                    break;
                }
                state.stage.store(2, std::memory_order_release);
            }
        }
    }
    if (!state.done.load(std::memory_order_acquire)) {
        fail(state, "TLS 场景超时");
    }
    if (client_thread.joinable()) {
        client_thread.join();
    }
    rig.shutdown();
    std::filesystem::remove_all(temp_dir);

    if (state.failed.load(std::memory_order_acquire)) {
        std::cerr << "scenario_tls: " << state.diagnostic << '\n';
        return false;
    }
    if (!state.receipt_applied) {
        std::cerr << "scenario_tls: 未收到 applied 回执\n";
        return false;
    }
    return true;
}

// ---- 场景 7：geoworldd 日志脱敏。以金丝雀 token/payload 驱动真实守护进程，
// 断言其 stdout/stderr 不含 token、stream ticket 与命令 payload ----

#ifdef GW_GEOWORLD_DAEMON_PATH

class DaemonProcess {
public:
    DaemonProcess() = default;
    DaemonProcess(const DaemonProcess&) = delete;
    DaemonProcess& operator=(const DaemonProcess&) = delete;

    ~DaemonProcess() { stop(); }

    [[nodiscard]] bool start(const std::filesystem::path& log_file) {
        const pid_t child = ::fork();
        if (child < 0) {
            return false;
        }
        if (child == 0) {
            const int log_fd = ::open(log_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            if (log_fd < 0 || ::dup2(log_fd, STDOUT_FILENO) < 0
                || ::dup2(log_fd, STDERR_FILENO) < 0) {
                _exit(126);
            }
            ::close(log_fd);
            const std::string run_ticks = std::to_string(kRedactionRunTicks);
            ::execl(GW_GEOWORLD_DAEMON_PATH, GW_GEOWORLD_DAEMON_PATH,
                    "--control-port", "0", "--stream-port", "0",
                    "--observer-token", kCanaryToken.data(), "--run-ticks",
                    run_ticks.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        pid_ = child;
        return true;
    }

    [[nodiscard]] bool wait(std::chrono::seconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            const pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            if (result < 0 && errno != EINTR) {
                pid_ = -1;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        return false;
    }

private:
    void stop() noexcept {
        if (pid_ <= 0) {
            return;
        }
        static_cast<void>(::kill(pid_, SIGTERM));
        if (wait(kDaemonStopDeadline)) {
            return;
        }
        if (pid_ > 0) {
            static_cast<void>(::kill(pid_, SIGKILL));
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
    }

    pid_t pid_{-1};
};

[[nodiscard]] bool wait_log_contains(const std::filesystem::path& log_file,
                                     std::string_view needle,
                                     std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream stream{log_file};
        std::ostringstream content;
        content << stream.rdbuf();
        if (content.str().find(needle) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

[[nodiscard]] bool parse_bound_port(const std::filesystem::path& log_file,
                                    std::string_view key, std::uint16_t& port) {
    std::ifstream stream{log_file};
    std::ostringstream content;
    content << stream.rdbuf();
    const std::string text = content.str();
    const std::size_t at = text.find(key);
    if (at == std::string::npos) {
        return false;
    }
    const std::size_t begin = at + key.size();
    const std::size_t end = text.find_first_not_of("0123456789", begin);
    if (end == begin) {
        return false;
    }
    port = static_cast<std::uint16_t>(
        std::stoul(text.substr(begin, end - begin)));
    return true;
}

[[nodiscard]] bool wait_bound_ports(const std::filesystem::path& log_file,
                                    std::uint16_t& control_port,
                                    std::uint16_t& stream_port,
                                    std::chrono::seconds timeout) {
    // 启动行由多次插入写出，读到前缀时端口数字可能尚未落盘，必须重试解析。
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (parse_bound_port(log_file, "control=127.0.0.1:", control_port)
            && parse_bound_port(log_file, "stream=127.0.0.1:", stream_port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

[[nodiscard]] bool scenario_log_redaction() {
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "geoworld-m4sec-log";
    std::filesystem::create_directories(temp_dir);
    const std::filesystem::path log_file = temp_dir / "geoworldd.log";
    // 临时目录跨运行复用，先清掉上一次可能遗留的日志。
    std::filesystem::remove(log_file);
    DaemonProcess daemon;
    if (!daemon.start(log_file)) {
        std::cerr << "scenario_log_redaction: 守护进程启动失败\n";
        return false;
    }
    if (!wait_log_contains(log_file, "geoworldd: control=", kDaemonStartupDeadline)) {
        std::cerr << "scenario_log_redaction: 守护进程未就绪\n";
        return false;
    }
    std::uint16_t control_port = 0;
    std::uint16_t stream_port = 0;
    if (!wait_bound_ports(log_file, control_port, stream_port,
                          kDaemonStartupDeadline)) {
        std::cerr << "scenario_log_redaction: 端口解析失败\n";
        return false;
    }

    StreamClientConfig client_config;
    client_config.control_address = "127.0.0.1:" + std::to_string(control_port);
    client_config.stream_port = stream_port;
    client_config.credential_token = std::string{kCanaryToken};
    client_config.io_timeout = std::chrono::milliseconds{3'000};
    StreamClient client{client_config};
    ScenarioState state;
    if (!open_and_subscribe(client, state)) {
        std::cerr << "scenario_log_redaction: " << state.diagnostic << '\n';
        return false;
    }
    std::string diagnostic;
    if (!client.acquire_ownership(WorldId{kSeedWidBase}, {std::string{kWritableProperty}},
                                  client.session().current_tick + kOwnershipLeaseTicks,
                                  diagnostic)) {
        std::cerr << "scenario_log_redaction acquire_ownership: " << diagnostic << '\n';
        return false;
    }
    if (!client.connect_stream(diagnostic)) {
        std::cerr << "scenario_log_redaction connect_stream: " << diagnostic << '\n';
        return false;
    }
    const std::string stream_ticket = client.session().stream_ticket;
    geoworld::protocol::WireKeyframe keyframe;
    if (!read_until_keyframe(client, state, keyframe, kMaxReadAttempts)) {
        std::cerr << "scenario_log_redaction: " << state.diagnostic << '\n';
        return false;
    }
    static_cast<void>(client.send_ack(keyframe.stream_epoch, keyframe.snapshot_id,
                                      diagnostic));
    const std::optional<SubmitResult> result = client.submit_set_property(
        WorldId{kSeedWidBase}, std::string{kWritableProperty},
        std::string{kCanaryPayload}, 1, 1, diagnostic);
    if (!result.has_value()) {
        std::cerr << "scenario_log_redaction submit: " << diagnostic << '\n';
        return false;
    }
    // 读若干帧保证命令与回执流经日志路径，随后关闭。
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!client.read_frame(diagnostic).has_value()) {
            break;
        }
    }
    client.disconnect_stream();
    static_cast<void>(client.close_session(diagnostic));

    if (!wait_log_contains(log_file, "geoworldd: shutdown",
                           kDaemonRunDeadline)) {
        std::cerr << "scenario_log_redaction: 守护进程未按 run-ticks 退出\n";
        return false;
    }
    if (!daemon.wait(kDaemonStopDeadline)) {
        std::cerr << "scenario_log_redaction: 守护进程关闭失败\n";
        return false;
    }
    std::ifstream stream{log_file};
    std::ostringstream content;
    content << stream.rdbuf();
    const std::string log = content.str();
    std::filesystem::remove_all(temp_dir);
    if (log.empty()) {
        std::cerr << "scenario_log_redaction: 日志为空\n";
        return false;
    }
    if (log.find(kCanaryToken) != std::string::npos) {
        std::cerr << "scenario_log_redaction: 日志泄漏 credential token\n";
        return false;
    }
    if (!stream_ticket.empty()
        && log.find(stream_ticket) != std::string::npos) {
        std::cerr << "scenario_log_redaction: 日志泄漏 stream ticket\n";
        return false;
    }
    if (log.find(kCanaryPayload) != std::string::npos) {
        std::cerr << "scenario_log_redaction: 日志泄漏命令 payload\n";
        return false;
    }
    return true;
}

#endif

} // namespace

int main() {
    if (!scenario_ack_rules()) {
        return 1;
    }
    if (!scenario_baseline_eviction_keyframe()) {
        return 2;
    }
    if (!scenario_slow_client()) {
        return 3;
    }
    if (!scenario_malformed_input()) {
        return 4;
    }
    if (!scenario_authorization_and_ticket()) {
        return 5;
    }
    if (!scenario_tls()) {
        return 6;
    }
#ifdef GW_GEOWORLD_DAEMON_PATH
    if (!scenario_log_redaction()) {
        return 7;
    }
#endif
    std::cout << "m4_security passed\n";
    return 0;
}
