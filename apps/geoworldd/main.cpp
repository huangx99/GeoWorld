#include "geoworld/simulation/tick.hpp"
#include "geoworld/debug/state_hash.hpp"
#include "geoworld/observability/logger.hpp"

#if GW_HAS_ECS_RUNTIME
#include "geoworld/ecs/runtime.hpp"
#endif

#if GW_BUILD_M4

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/gateway/control_server.hpp"
#include "geoworld/gateway/frame_codec.hpp"
#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/gateway/stream_transport.hpp"
#include "geoworld/protocol/version.hpp"
#include "geoworld/projection/engine.hpp"
#include "geoworld/runtime/world_runtime.hpp"

#if GW_BUILD_M5
#include "geoworld/gateway/durable_persistence.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/wal.hpp"

#include <filesystem>
#include <memory>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// geoworldd M4 组合根配置：默认值集中在此，命令行 --flag value 覆盖。
struct DaemonConfig {
    std::string control_address{"127.0.0.1"};
    std::uint16_t control_port{50051};
    std::string stream_address{"127.0.0.1"};
    std::uint16_t stream_port{50052};
    std::string observer_token{"dev-observer-token"};
    std::string admin_token{"dev-admin-token"};
    std::string writable_property{"speed"};
    geoworld::spatial::Geodetic enu_origin{31.0, 121.0, 0.0};
    std::uint64_t seed_wid{7};
    double seed_east{10.0};
    double seed_north{10.0};
    double seed_speed{1.0};
    // 0 表示运行到 SIGINT；否则跑固定 tick 数后退出（便于脚本化验证）。
    std::uint64_t run_ticks{0};
    // 数据面分片 io 线程数；1 为单线程原行为。
    std::uint32_t io_threads{1};
    std::string tls_certificate_file;
    std::string tls_private_key_file;
    // M5 durable admission：空表示关闭（M4 原行为）；非空为 durable_root 目录。
    std::string durable_root;
    std::uint64_t durable_world_id{1};
    // 分支标识：规范 36 字符形式（8-4-4-4-12 十六进制），默认固定分支便于演示。
    std::string durable_branch{"00000000-0000-0000-0000-000000000001"};
};

constexpr std::string_view kUsage =
    "用法: geoworldd [--control-address host] [--control-port port]"
    " [--stream-address host] [--stream-port port]"
    " [--observer-token token] [--admin-token token]"
    " [--seed-wid id] [--run-ticks n] [--io-threads n]"
    " [--tls-cert file] [--tls-key file]"
    " [--durable-root dir] [--durable-world-id id] [--durable-branch uuid]";

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) {
    g_stop = 1;
}

[[nodiscard]] bool parse_cli(int argc, char** argv, DaemonConfig& config) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag{argv[index]};
        const auto take_value = [&]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (flag == "--control-address") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.control_address = value;
        } else if (flag == "--control-port") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.control_port = static_cast<std::uint16_t>(std::stoul(value));
        } else if (flag == "--stream-address") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.stream_address = value;
        } else if (flag == "--stream-port") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.stream_port = static_cast<std::uint16_t>(std::stoul(value));
        } else if (flag == "--observer-token") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.observer_token = value;
        } else if (flag == "--admin-token") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.admin_token = value;
        } else if (flag == "--seed-wid") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.seed_wid = std::stoull(value);
        } else if (flag == "--run-ticks") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.run_ticks = std::stoull(value);
        } else if (flag == "--io-threads") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.io_threads = static_cast<std::uint32_t>(std::stoul(value));
        } else if (flag == "--tls-cert") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.tls_certificate_file = value;
        } else if (flag == "--tls-key") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.tls_private_key_file = value;
        } else if (flag == "--durable-root") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.durable_root = value;
        } else if (flag == "--durable-world-id") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.durable_world_id = std::stoull(value);
        } else if (flag == "--durable-branch") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.durable_branch = value;
        } else {
            return false;
        }
    }
    return true;
}

// stream ticket 生成：随机十六进制，URL 安全（握手经查询串携带，不做百分号解码）。
[[nodiscard]] std::string make_token() {
    static std::mt19937_64 engine{std::random_device{}()};
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string token(32, '0');
    for (std::size_t index = 0; index < token.size(); ++index) {
        token[index] = kHexDigits[engine() % 16U];
    }
    return token;
}

int run_daemon(const DaemonConfig& config) {
    std::string diagnostic;

#if GW_BUILD_M5
    // durable writer 必须存活到主循环结束；空表示 durable admission 关闭。
    std::shared_ptr<geoworld::persistence::WalWriter> durable_writer;
#else
    if (!config.durable_root.empty()) {
        std::cerr << "durable admission 未编译（GW_BUILD_M5=OFF），--durable-root 不可用\n";
        return 2;
    }
#endif

    geoworld::projection::ProjectionConfig projection_config;
    projection_config.enu_origin =
        geoworld::spatial::geodetic_to_ecef(config.enu_origin);
    if (!geoworld::projection::validate(projection_config, diagnostic)) {
        std::cerr << "projection 配置非法: " << diagnostic << '\n';
        return 2;
    }
    geoworld::gateway::GatewayConfig gateway_config;
    if (!geoworld::gateway::validate(gateway_config, diagnostic)) {
        std::cerr << "gateway 配置非法: " << diagnostic << '\n';
        return 2;
    }

    geoworld::projection::ProjectionPolicy policy;
    policy.allow_property(config.writable_property);
    geoworld::projection::ProjectionEngine engine{projection_config, policy};

    auto authentication = std::make_shared<geoworld::gateway::FixtureAuthentication>();
    authentication->add_credential(config.observer_token, {"viewer", false});
    authentication->add_credential(config.admin_token, {"admin", true});
    auto authorization = std::make_shared<geoworld::gateway::FixtureAuthorization>();
    authorization->allow_writable_property(config.writable_property);

    geoworld::runtime::WorldRuntime runtime;
    geoworld::gateway::GatewayCore core{
        gateway_config, engine, authentication, authorization,
        [] { return std::chrono::steady_clock::now(); }, make_token,
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

#if GW_BUILD_M5
    if (!config.durable_root.empty()) {
        const std::optional<geoworld::persistence::BranchId> branch =
            geoworld::persistence::parse_branch_id(config.durable_branch);
        if (!branch.has_value()) {
            std::cerr << "durable 分支标识非法: " << config.durable_branch << '\n';
            return 2;
        }
        geoworld::persistence::WalConfig wal_config;
        wal_config.durable_root = config.durable_root;
        wal_config.world = geoworld::foundation::WorldId{config.durable_world_id};
        wal_config.branch = *branch;
        const std::shared_ptr<geoworld::persistence::FileOps> file_ops =
            geoworld::persistence::make_posix_file_ops();
        const geoworld::persistence::DurableLayout layout =
            geoworld::persistence::make_durable_layout(
                wal_config.durable_root, wal_config.world, wal_config.branch);
        // 先扫描重建幂等索引再启动 writer；扫描已修剪断电尾部，writer start
        // 内部的二次扫描看到的是一致状态。
        const std::filesystem::path wal_dir = layout.wal_dir();
        if (std::filesystem::exists(wal_dir)) {
            const geoworld::persistence::WalScanResult scan =
                geoworld::persistence::scan_wal_directory(wal_dir, *file_ops);
            if (!scan.ok()) {
                std::cerr << "durable WAL 扫描失败: "
                          << geoworld::persistence::error_code(scan.error) << '\n';
                return 5;
            }
            if (!geoworld::gateway::restore_durable_index(core, scan)) {
                std::cerr << "durable 幂等索引重建失败（记录损坏）\n";
                return 5;
            }
        }
        durable_writer =
            std::make_shared<geoworld::persistence::WalWriter>(wal_config, file_ops);
        const geoworld::persistence::Status started = durable_writer->start();
        if (!started.ok()) {
            std::cerr << "durable WAL 启动失败: "
                      << geoworld::persistence::error_code(started.error) << '\n';
            return 5;
        }
        core.set_durable_log(
            geoworld::gateway::make_persistence_admission_log(durable_writer));
    }
#endif

    geoworld::gateway::ControlServerConfig control_config;
    control_config.listen_address = config.control_address;
    control_config.port = config.control_port;
    control_config.advertised_data_endpoint =
        "ws://" + config.stream_address + ":" + std::to_string(config.stream_port)
        + std::string(geoworld::gateway::default_stream_path);
    control_config.tick_source = [&runtime] {
        return static_cast<std::uint64_t>(runtime.clock().tick());
    };
    geoworld::gateway::TransportConfig transport_config;
    transport_config.listen_address = config.stream_address;
    transport_config.port = config.stream_port;
    transport_config.io_thread_count = config.io_threads;
    if (!config.tls_certificate_file.empty()) {
        geoworld::gateway::TlsCertificateFiles tls{config.tls_certificate_file,
                                                   config.tls_private_key_file};
        control_config.tls = tls;
        transport_config.tls = tls;
    }

    geoworld::gateway::ControlServer control{core, control_config};
    if (!control.start(diagnostic)) {
        std::cerr << "控制面启动失败: " << diagnostic << '\n';
        return 3;
    }
    geoworld::gateway::StreamTransport transport{core, geoworld::protocol::ProtocolLimits{},
                                                 transport_config};
    if (!transport.start(diagnostic)) {
        std::cerr << "数据面启动失败: " << diagnostic << '\n';
        return 4;
    }

    // 演示种子对象：位于默认 AOI 内的活动实体，携带可写 speed 属性。
    const geoworld::spatial::Ecef seed_ecef = geoworld::spatial::enu_to_ecef(
        projection_config.enu_origin, config.enu_origin,
        geoworld::spatial::Enu{config.seed_east, config.seed_north, 0.0});
    geoworld::world::WorldObject seed;
    seed.id = geoworld::foundation::WorldId{config.seed_wid};
    seed.position = geoworld::world::PositionEcef{seed_ecef.x, seed_ecef.y, seed_ecef.z};
    seed.semantic_type = "geoworld.demo";
    seed.lifecycle = geoworld::world::LifecycleState::active;
    seed.properties.emplace(config.writable_property, config.seed_speed);
    static_cast<void>(runtime.submit(
        0, geoworld::simulation::CreateObjectCommand{seed}));

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "geoworldd: control=" << config.control_address << ":"
              << control.bound_port()
              << " stream=" << config.stream_address << ":" << transport.bound_port()
              << '\n';

    // docs/M4.md 4.3.2 主循环：固定 tick 推进 -> 投影观察（step 内部触发）
    // -> 命令终态回执 -> 控制面封送执行 -> gateway pump -> 传输 poll。
    const auto tick_interval =
        std::chrono::microseconds{projection_config.tick_dt_microseconds};
    std::unordered_map<std::uint64_t, std::uint64_t> reported_hashes;
    auto next_tick_boundary = std::chrono::steady_clock::now();
    while (g_stop == 0
           && (config.run_ticks == 0
               || static_cast<std::uint64_t>(runtime.clock().tick())
                      < config.run_ticks)) {
        const geoworld::runtime::StepResult step = runtime.step();
        core.on_commands_applied(step.commands);
        control.poll();
        // durable WAL 票据完成 -> durable accepted 回执 -> 投递命令缓冲。
        core.poll_durable_tickets();
        core.pump(step.tick);
        transport.poll();

        for (const geoworld::projection::ConnectionId connection :
             transport.active_connections()) {
            const std::uint64_t view_hash = engine.connection_view_hash(connection);
            auto [entry, inserted] =
                reported_hashes.emplace(connection.value, view_hash);
            if (!inserted && entry->second == view_hash) {
                continue;
            }
            entry->second = view_hash;
            std::cout << "connection=" << connection.value
                      << " view_hash=" << view_hash << " tick=" << step.tick << '\n';
        }

        next_tick_boundary += tick_interval;
        std::this_thread::sleep_until(next_tick_boundary);
    }

    std::cout << "geoworldd: shutdown tick=" << runtime.clock().tick()
              << " objects=" << runtime.world().size() << '\n';
    transport.shutdown();
    control.shutdown();
#if GW_BUILD_M5
    // 显式关闭：flush 当前承诺边界，所有已接受 ticket 到达终态。
    if (durable_writer) {
        durable_writer->shutdown();
    }
#endif
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    DaemonConfig config;
    if (!parse_cli(argc, argv, config)) {
        std::cerr << kUsage << '\n';
        return 2;
    }
    return run_daemon(config);
}

#else

#include <iostream>

int main() {
    geoworld::world::World world;
    geoworld::simulation::CommandBuffer commands;
    geoworld::simulation::TickClock clock;

    geoworld::world::WorldObject object;
    object.id = geoworld::foundation::WorldId{1};
    object.semantic_type = "core.example";
    object.lifecycle = geoworld::world::LifecycleState::active;

    static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{object}));
    const auto report = commands.apply(world, 0);
    clock.advance();

    std::size_t runtime_entities = 0;
#if GW_HAS_ECS_RUNTIME
    geoworld::ecs::Runtime ecs;
    const auto* created = world.find(object.id);
    if (created != nullptr) {
        static_cast<void>(ecs.activate(*created));
    }
    runtime_entities = ecs.size();
#endif

    geoworld::observability::Logger logger;
    logger.write(geoworld::observability::LogLevel::info, "runtime", "world started",
                 geoworld::observability::LogContext{static_cast<std::uint64_t>(clock.tick()), object.id});

    std::cout << "geoworldd: tick=" << clock.tick()
              << " objects=" << world.size()
              << " runtime_entities=" << runtime_entities
              << " applied=" << report.applied
              << " hash=" << geoworld::debug::world_state_hash(world) << '\n';
    return 0;
}

#endif
