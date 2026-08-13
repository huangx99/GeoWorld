// M4-C loopback 端到端闭环：gRPC 开会话/订阅/命令 + WebSocket keyframe/delta
// -> replica hash 与服务端连接投影视图 hash 一致 -> 终态回执 -> 断线重连新 keyframe。
// 组合方式与 geoworldd 相同，但直接在进程内组合对象（loopback 明文配置）。
#include "stream_client.hpp"

#include "geoworld/gateway/control_server.hpp"
#include "geoworld/gateway/frame_codec.hpp"
#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/gateway/stream_transport.hpp"
#include "geoworld/protocol/replica.hpp"
#include "geoworld/protocol/version.hpp"
#include "geoworld/projection/engine.hpp"
#include "geoworld/runtime/world_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

using geoworld::client::StreamClient;
using geoworld::client::StreamClientConfig;
using geoworld::foundation::WorldId;
using geoworld::gateway::ControlServer;
using geoworld::gateway::ControlServerConfig;
using geoworld::gateway::FixtureAuthentication;
using geoworld::gateway::FixtureAuthorization;
using geoworld::gateway::GatewayConfig;
using geoworld::gateway::GatewayCore;
using geoworld::gateway::StreamTransport;
using geoworld::gateway::TransportConfig;
using geoworld::projection::ConnectionId;
using geoworld::projection::ProjectionConfig;
using geoworld::projection::ProjectionEngine;
using geoworld::projection::ProjectionPolicy;

const geoworld::spatial::Geodetic kOriginGeodetic{31.0, 121.0, 0.0};
constexpr std::uint64_t kSeedWid = 7;
constexpr double kAoiExtentMeters = 1000.0;
constexpr std::uint64_t kOwnershipLeaseTicks = 100'000;
constexpr double kUpdatedSpeed = 9.5;
constexpr auto kTestDeadline = std::chrono::seconds{60};
constexpr auto kLoopSleep = std::chrono::milliseconds{1};

[[nodiscard]] geoworld::world::PositionEcef at_enu(double east, double north) {
    const geoworld::spatial::Ecef origin =
        geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
    const geoworld::spatial::Ecef point = geoworld::spatial::enu_to_ecef(
        origin, kOriginGeodetic, geoworld::spatial::Enu{east, north, 0.0});
    return geoworld::world::PositionEcef{point.x, point.y, point.z};
}

// 客户端线程与主线程（世界线程）之间只传递不可变结果与原子阶段标记。
struct ClientOutcome {
    std::uint64_t hash_after_keyframe{};
    std::uint64_t hash_after_command{};
    std::uint64_t hash_after_reconnect{};
    std::uint64_t reconnect_baseline{1};
    bool got_keyframe{};
    bool receipt_applied{};
    bool saw_update_after_command{};
    bool reconnect_first_frame_keyframe{};
};

struct SharedState {
    std::atomic<int> stage{0};
    std::atomic<bool> done{false};
    std::atomic<bool> ticket_ready{false};
    std::string reconnect_ticket;
    std::string session_id;
    std::string diagnostic;
    ClientOutcome outcome;
};

[[nodiscard]] bool wait_stage(const SharedState& shared, int stage) {
    const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (shared.stage.load(std::memory_order_acquire) < stage) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(kLoopSleep);
    }
    return true;
}

void run_client(StreamClientConfig config, SharedState& shared) {
    StreamClient client{std::move(config)};
    std::string diagnostic;
    const auto fail = [&shared, &diagnostic](const char* step) {
        shared.diagnostic = std::string{step} + ": " + diagnostic;
        shared.done.store(true, std::memory_order_release);
    };

    if (!client.open_session(diagnostic)) {
        fail("open_session");
        return;
    }
    shared.session_id = client.session().session_id;

    geoworld::projection::Subscription subscription;
    subscription.area = geoworld::spatial::Aabb{
        geoworld::spatial::Enu{-kAoiExtentMeters, -kAoiExtentMeters, -kAoiExtentMeters},
        geoworld::spatial::Enu{kAoiExtentMeters, kAoiExtentMeters, kAoiExtentMeters},
    };
    if (!client.update_subscription(subscription, diagnostic)) {
        fail("update_subscription");
        return;
    }
    if (!client.acquire_ownership(WorldId{kSeedWid}, {"speed"},
                                  client.session().current_tick + kOwnershipLeaseTicks,
                                  diagnostic)) {
        fail("acquire_ownership");
        return;
    }
    if (!client.connect_stream(diagnostic)) {
        fail("connect_stream");
        return;
    }

    geoworld::protocol::ReplicaAccumulator replica;
    bool submitted = false;
    bool reconnecting = false;
    std::string reconnect_ticket;

    const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            fail("read_frame");
            return;
        }
        std::uint64_t epoch = 0;
        std::uint64_t snapshot = 0;
        bool state_frame = true;
        if (const auto* keyframe =
                std::get_if<geoworld::protocol::WireKeyframe>(&*frame)) {
            replica.apply(*frame);
            epoch = keyframe->stream_epoch;
            snapshot = keyframe->snapshot_id;
            if (!reconnecting) {
                shared.outcome.got_keyframe = true;
                shared.outcome.hash_after_keyframe = replica.hash();
            } else {
                shared.outcome.reconnect_first_frame_keyframe = true;
                shared.outcome.reconnect_baseline = keyframe->baseline_snapshot_id;
                shared.outcome.hash_after_reconnect = replica.hash();
            }
        } else if (const auto* delta =
                       std::get_if<geoworld::protocol::WireDelta>(&*frame)) {
            replica.apply(*frame);
            epoch = delta->stream_epoch;
            snapshot = delta->snapshot_id;
            for (const auto& update : delta->updates) {
                if (update.wid == WorldId{kSeedWid}) {
                    shared.outcome.saw_update_after_command = true;
                }
            }
        } else if (const auto* reliable =
                       std::get_if<geoworld::protocol::WireReliable>(&*frame)) {
            state_frame = false;
            if (reliable->kind == geoworld::protocol::ReliableKind::command_receipt
                && reliable->receipt.status
                       == geoworld::protocol::ReceiptStatus::applied) {
                shared.outcome.receipt_applied = true;
            }
        } else {
            state_frame = false; // Heartbeat 不参与 replica 状态。
        }
        if (state_frame) {
            if (!client.send_ack(epoch, snapshot, diagnostic)) {
                fail("send_ack");
                return;
            }
        }

        if (shared.outcome.got_keyframe && !submitted) {
            shared.stage.store(1, std::memory_order_release);
            if (!wait_stage(shared, 2)) {
                fail("wait_stage(2)");
                return;
            }
            const auto result = client.submit_set_property(
                WorldId{kSeedWid}, "speed", kUpdatedSpeed, 1, 1, diagnostic);
            if (!result.has_value()
                || result->status != geoworld::gateway::ReceiptStatus::accepted) {
                diagnostic = result.has_value() ? result->error_code : diagnostic;
                fail("submit_set_property");
                return;
            }
            submitted = true;
        }
        if (submitted && shared.outcome.receipt_applied
            && shared.outcome.saw_update_after_command
            && shared.stage.load(std::memory_order_acquire) == 2) {
            shared.outcome.hash_after_command = replica.hash();
            shared.stage.store(3, std::memory_order_release);
        }
        if (shared.stage.load(std::memory_order_acquire) == 4 && !reconnecting) {
            if (!shared.ticket_ready.load(std::memory_order_acquire)) {
                continue;
            }
            reconnect_ticket = shared.reconnect_ticket;
            reconnecting = true;
            replica.clear();
            client.disconnect_stream();
            if (!client.connect_stream(diagnostic, reconnect_ticket)) {
                fail("reconnect_stream");
                return;
            }
        }
        if (reconnecting && shared.outcome.reconnect_first_frame_keyframe) {
            shared.stage.store(5, std::memory_order_release);
            shared.done.store(true, std::memory_order_release);
            client.disconnect_stream();
            return;
        }
    }
    diagnostic = "客户端阶段超时";
    fail("deadline");
}

} // namespace

namespace {

// 同一闭环分别跑在单线程（io_thread_count=1）与分片（>1）传输上。
[[nodiscard]] int run_closed_loop(std::uint32_t io_thread_count) {
    ProjectionConfig projection_config;
    projection_config.enu_origin = geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
    ProjectionPolicy policy;
    policy.allow_property("speed");
    ProjectionEngine engine{projection_config, policy};

    auto authentication = std::make_shared<FixtureAuthentication>();
    authentication->add_credential("observer-token", {"viewer", false});
    auto authorization = std::make_shared<FixtureAuthorization>();
    authorization->allow_writable_property("speed");

    geoworld::runtime::WorldRuntime runtime;
    std::uint64_t ticket_counter = 0;
    GatewayCore core{
        GatewayConfig{}, engine, authentication, authorization,
        [] { return std::chrono::steady_clock::now(); },
        [&ticket_counter] { return "test-ticket-" + std::to_string(++ticket_counter); },
        geoworld::protocol::control_api_version,
        geoworld::protocol::data_schema_version};
    core.set_frame_encoder(geoworld::gateway::make_frame_encoder());
    core.set_receipt_encoder(geoworld::gateway::make_receipt_encoder());
    core.set_heartbeat_encoder(geoworld::gateway::make_heartbeat_encoder());
    core.set_command_submitter(
        [&runtime](std::uint64_t target_tick,
                   geoworld::simulation::CommandPayload payload,
                   geoworld::simulation::CommandMeta meta) {
            return runtime.submit(target_tick, std::move(payload), meta);
        });
    runtime.add_projection_observer(
        [&engine](const geoworld::world::World& world, std::uint64_t tick,
                  std::uint64_t state_hash,
                  const geoworld::spatial::SpatialQuery* spatial) {
            static_cast<void>(spatial);
            engine.on_projection(world, tick, state_hash);
        });

    ControlServerConfig control_config;
    control_config.port = 0; // 临时端口，避免并行测试冲突。
    control_config.tick_source = [&runtime] {
        return static_cast<std::uint64_t>(runtime.clock().tick());
    };
    TransportConfig transport_config;
    transport_config.port = 0;
    transport_config.io_thread_count = io_thread_count;

    ControlServer control{core, control_config};
    std::string diagnostic;
    if (!control.start(diagnostic)) {
        std::cerr << diagnostic << '\n';
        return 2;
    }
    StreamTransport transport{core, geoworld::protocol::ProtocolLimits{},
                              transport_config};
    if (!transport.start(diagnostic)) {
        std::cerr << diagnostic << '\n';
        return 3;
    }

    geoworld::world::WorldObject seed;
    seed.id = WorldId{kSeedWid};
    seed.position = at_enu(10.0, 10.0);
    seed.semantic_type = "test.entity";
    seed.lifecycle = geoworld::world::LifecycleState::active;
    seed.properties.emplace("speed", 1.0);
    static_cast<void>(runtime.submit(
        0, geoworld::simulation::CreateObjectCommand{seed}));

    SharedState shared;
    StreamClientConfig client_config;
    client_config.control_address =
        "127.0.0.1:" + std::to_string(control.bound_port());
    client_config.stream_port = transport.bound_port();
    client_config.credential_token = "observer-token";
    client_config.io_timeout = std::chrono::milliseconds{2'000};
    std::thread client_thread{run_client, client_config, std::ref(shared)};

    // 主循环与 geoworldd 同序：step -> 回执 -> 控制面封送 -> pump -> transport poll。
    int handled_stage = 0;
    std::optional<std::uint64_t> reconnect_epoch_seen;
    const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
    while (!shared.done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        const geoworld::runtime::StepResult step = runtime.step();
        core.on_commands_applied(step.commands);
        control.poll();
        core.pump(step.tick);
        transport.poll();

        const int stage = shared.stage.load(std::memory_order_acquire);
        if (stage > handled_stage) {
            handled_stage = stage;
            const std::vector<ConnectionId> active = transport.active_connections();
            if (stage == 1) {
                if (active.size() != 1
                    || engine.connection_view_hash(active[0])
                           != shared.outcome.hash_after_keyframe) {
                    std::cerr << "keyframe 后 replica hash 与服务端投影视图不一致\n";
                    shared.done.store(true, std::memory_order_release);
                    break;
                }
                shared.stage.store(2, std::memory_order_release);
                handled_stage = 2;
            } else if (stage == 3) {
                if (active.size() != 1
                    || engine.connection_view_hash(active[0])
                           != shared.outcome.hash_after_command) {
                    std::cerr << "命令 delta 后 replica hash 与服务端投影视图不一致\n";
                    shared.done.store(true, std::memory_order_release);
                    break;
                }
                const std::optional<geoworld::gateway::SessionId> session = [&]() {
                    geoworld::gateway::SessionId id;
                    id.value = std::stoull(shared.session_id);
                    return id.valid() ? std::optional{id} : std::nullopt;
                }();
                if (!session.has_value()) {
                    std::cerr << "session id 解析失败\n";
                    shared.done.store(true, std::memory_order_release);
                    break;
                }
                const std::optional<std::string> ticket =
                    core.issue_stream_ticket(*session);
                if (!ticket.has_value()) {
                    std::cerr << "重连 ticket 签发失败\n";
                    shared.done.store(true, std::memory_order_release);
                    break;
                }
                shared.reconnect_ticket = *ticket;
                shared.ticket_ready.store(true, std::memory_order_release);
                shared.stage.store(4, std::memory_order_release);
                handled_stage = 4;
            } else if (stage == 5) {
                if (active.size() != 1
                    || engine.connection_view_hash(active[0])
                           != shared.outcome.hash_after_reconnect) {
                    std::cerr << "重连 keyframe 后 replica hash 与服务端投影视图不一致\n";
                    shared.done.store(true, std::memory_order_release);
                    break;
                }
            }
        }
        std::this_thread::sleep_for(kLoopSleep);
    }

    if (!shared.done.load(std::memory_order_acquire)) {
        shared.done.store(true, std::memory_order_release);
    }
    if (client_thread.joinable()) {
        client_thread.join();
    }
    transport.shutdown();
    control.shutdown();

    if (!shared.diagnostic.empty()) {
        std::cerr << "客户端阶段失败: " << shared.diagnostic << '\n';
        return 4;
    }
    if (!shared.outcome.got_keyframe) {
        return 5;
    }
    if (!shared.outcome.receipt_applied) {
        return 6;
    }
    if (!shared.outcome.saw_update_after_command) {
        return 7;
    }
    if (!shared.outcome.reconnect_first_frame_keyframe
        || shared.outcome.reconnect_baseline != 0) {
        return 8;
    }
    if (shared.stage.load(std::memory_order_acquire) < 5) {
        return 9;
    }
    return 0;
}

} // namespace

int main() {
    const int single = run_closed_loop(1);
    if (single != 0) {
        std::cerr << "单线程传输闭环失败 code=" << single << '\n';
        return single;
    }
    const int sharded = run_closed_loop(3);
    if (sharded != 0) {
        std::cerr << "分片传输闭环失败 code=" << sharded << '\n';
        return 100 + sharded;
    }
    std::cout << "m4_stream closed loop passed (io_thread_count=1,3)\n";
    return 0;
}
