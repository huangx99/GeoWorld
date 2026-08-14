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
#include "geoworld/gateway/durable.hpp"
#include "geoworld/gateway/durable_persistence.hpp"
#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/recovery.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/wal.hpp"

#include <filesystem>
#include <future>
#include <memory>
#endif

#include <atomic>
#include <algorithm>
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
    std::uint64_t checkpoint_interval_ticks{1'000};
    std::uint64_t hash_interval_ticks{100};
};

constexpr std::string_view kUsage =
    "用法: geoworldd [--control-address host] [--control-port port]"
    " [--stream-address host] [--stream-port port]"
    " [--observer-token token] [--admin-token token]"
    " [--seed-wid id] [--run-ticks n] [--io-threads n]"
    " [--tls-cert file] [--tls-key file]"
    " [--durable-root dir] [--durable-world-id id] [--durable-branch uuid]"
    " [--checkpoint-interval-ticks n] [--hash-interval-ticks n]";

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
        } else if (flag == "--checkpoint-interval-ticks") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.checkpoint_interval_ticks = std::stoull(value);
        } else if (flag == "--hash-interval-ticks") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.hash_interval_ticks = std::stoull(value);
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

#if GW_BUILD_M5
void register_runtime_checkpoint_providers(
    geoworld::persistence::CheckpointRegistry& registry,
    geoworld::runtime::WorldRuntime& runtime) {
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_world_provider(runtime.world_for_restore())));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_clock_provider(runtime.clock_for_restore())));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_command_buffer_provider(runtime.commands_for_restore())));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_event_bus_provider(runtime.events_for_restore())));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_ai_intents_provider(runtime.ai_intents_for_restore())));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_random_streams_provider(runtime.random_streams_for_restore())));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_artifacts_provider(runtime.artifacts_for_restore())));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_ecs_active_set_provider(
            runtime.ecs_for_restore(), runtime.world_for_restore())));
}

[[nodiscard]] geoworld::gateway::GatewayError recovery_error(
    geoworld::simulation::CommandRejectReason reason) {
    switch (reason) {
    case geoworld::simulation::CommandRejectReason::none:
        return geoworld::gateway::GatewayError::none;
    case geoworld::simulation::CommandRejectReason::missing_object:
        return geoworld::gateway::GatewayError::missing_object;
    case geoworld::simulation::CommandRejectReason::version_conflict:
        return geoworld::gateway::GatewayError::version_conflict;
    case geoworld::simulation::CommandRejectReason::apply_failed:
        return geoworld::gateway::GatewayError::invalid_request;
    }
    return geoworld::gateway::GatewayError::invalid_request;
}

struct ReplayedDurableCommand {
    geoworld::gateway::RecoveredDurableCommand command;
    std::uint64_t external_lsn{};
    bool outcome_already_persisted{};
    std::optional<geoworld::simulation::CommandOutcome> outcome;
};

[[nodiscard]] bool same_durable_request(
    const geoworld::gateway::RecoveredDurableCommand& command,
    const geoworld::gateway::RecoveredDurableOutcome& outcome) {
    return command.principal_id == outcome.principal_id
           && command.request_id == outcome.request_id;
}
#endif

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
#if GW_BUILD_M5
    geoworld::persistence::CheckpointRegistry checkpoint_registry;
    std::unique_ptr<geoworld::persistence::CheckpointCoordinator> checkpoint_coordinator;
    std::future<geoworld::persistence::Result<geoworld::persistence::PublishedCheckpoint>>
        checkpoint_publish;
    std::vector<geoworld::persistence::AppendTicket> hash_tickets;
    bool recovered_existing_state = false;
    register_runtime_checkpoint_providers(checkpoint_registry, runtime);
#endif
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
        geoworld::persistence::RecoveryPlanner planner{
            layout, wal_config.world, wal_config.branch, file_ops};
        auto recovery = planner.build();
        if (!recovery.ok()) {
            std::cerr << "durable 恢复计划失败: "
                      << geoworld::persistence::error_code(recovery.error) << '\n';
            return 5;
        }
        if (recovery.value.checkpoint.has_value()) {
            wal_config.recovery_floor_lsn =
                recovery.value.checkpoint->info.anchor.included_lsn;
        }
        if (recovery.value.checkpoint.has_value()) {
            geoworld::persistence::CheckpointLoader loader{
                layout, wal_config.world, wal_config.branch, file_ops};
            const auto restored = loader.restore_into(
                checkpoint_registry, *recovery.value.checkpoint);
            if (restored != geoworld::persistence::PersistenceError::none) {
                std::cerr << "durable 检查点恢复失败: "
                          << geoworld::persistence::error_code(restored) << '\n';
                return 5;
            }
            recovered_existing_state = true;
        }

        std::unordered_map<std::uint64_t, std::uint64_t> recovery_hashes;
        std::vector<ReplayedDurableCommand> replayed_commands;
        std::vector<geoworld::gateway::RecoveredDurableOutcome> persisted_outcomes;
        std::uint64_t replay_until_tick{};
        bool replay_required = false;
        for (const auto& record : recovery.value.replay_records) {
            if (record.kind == geoworld::persistence::WalRecordKind::external_command) {
                auto command = geoworld::gateway::decode_external_command_record(record.payload);
                if (!command.has_value()) {
                    std::cerr << "durable 外部命令解码失败, LSN=" << record.lsn.value << '\n';
                    return 5;
                }
                geoworld::simulation::CommandMeta meta;
                meta.durable_lsn = record.lsn.value;
                meta.ingress_sequence = record.lsn.value;
                meta.expected_object_version = command->expected_object_version;
                ReplayedDurableCommand replayed;
                replayed.command = std::move(*command);
                replayed.external_lsn = record.lsn.value;
                if (runtime.submit(replayed.command.target_tick,
                                   std::move(replayed.command.payload), meta) == 0) {
                    std::cerr << "durable 恢复命令入队失败, LSN=" << record.lsn.value << '\n';
                    return 5;
                }
                replay_until_tick = std::max(replay_until_tick, replayed.command.target_tick);
                replayed_commands.push_back(std::move(replayed));
                replay_required = true;
            } else if (record.kind == geoworld::persistence::WalRecordKind::state_hash) {
                const auto point = geoworld::persistence::decode_state_hash_point(record.payload);
                if (!point.has_value()) {
                    std::cerr << "durable hash 记录损坏, LSN=" << record.lsn.value << '\n';
                    return 5;
                }
                recovery_hashes.insert_or_assign(point->tick, point->hash);
                replay_until_tick = std::max(replay_until_tick, point->tick);
                replay_required = true;
            } else if (record.kind == geoworld::persistence::WalRecordKind::command_outcome) {
                auto outcome =
                    geoworld::gateway::decode_command_outcome_record(record.payload);
                if (!outcome.has_value()) {
                    std::cerr << "durable 命令终态损坏, LSN=" << record.lsn.value << '\n';
                    return 5;
                }
                persisted_outcomes.push_back(std::move(*outcome));
            } else if (record.kind
                              != geoworld::persistence::WalRecordKind::checkpoint_marker) {
                std::cerr << "durable 恢复缺少该输入类型的处理器, LSN="
                          << record.lsn.value << " kind="
                          << static_cast<std::uint16_t>(record.kind) << '\n';
                return 5;
            }
        }
        recovered_existing_state = recovered_existing_state
                                   || !recovery.value.replay_records.empty();
        while (replay_required
               && static_cast<std::uint64_t>(runtime.clock().tick()) <= replay_until_tick) {
            const auto step = runtime.step();
            for (const auto& outcome : step.commands.outcomes) {
                const auto found = std::find_if(
                    replayed_commands.begin(), replayed_commands.end(),
                    [&outcome](const ReplayedDurableCommand& candidate) {
                        return candidate.external_lsn == outcome.durable_lsn;
                    });
                if (found != replayed_commands.end()) {
                    found->outcome = outcome;
                }
            }
            const auto expected = recovery_hashes.find(step.tick);
            if (expected != recovery_hashes.end() && expected->second != step.state_hash) {
                std::cerr << "durable 回放 hash 分叉, tick=" << step.tick
                          << " expected=" << expected->second
                          << " actual=" << step.state_hash << '\n';
                return 5;
            }
        }
        for (auto& command : replayed_commands) {
            command.outcome_already_persisted = std::any_of(
                persisted_outcomes.begin(), persisted_outcomes.end(),
                [&command](const auto& outcome) {
                    return same_durable_request(command.command, outcome);
                });
            if (!command.outcome.has_value()) {
                std::cerr << "durable 恢复命令缺少执行终态, LSN="
                          << command.external_lsn << '\n';
                return 5;
            }
        }

        geoworld::persistence::CheckpointConfig checkpoint_config;
        checkpoint_config.layout = layout;
        checkpoint_config.world = wal_config.world;
        checkpoint_config.branch = wal_config.branch;
        checkpoint_config.authoritative_modules =
            geoworld::runtime::WorldRuntime::authoritative_state_modules();
        checkpoint_coordinator =
            std::make_unique<geoworld::persistence::CheckpointCoordinator>(
                checkpoint_config, file_ops);

        // 扫描全部 WAL 重建跨重启幂等索引；恢复计划已完成尾部修剪。
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
        for (const auto& replayed : replayed_commands) {
            if (replayed.outcome_already_persisted) {
                continue;
            }
            const auto& applied = *replayed.outcome;
            geoworld::persistence::WalRecord outcome_record;
            outcome_record.kind = geoworld::persistence::WalRecordKind::command_outcome;
            outcome_record.target_tick = 0;
            outcome_record.payload = geoworld::gateway::encode_command_outcome_record(
                replayed.command.principal_id, replayed.command.request_id,
                replayed.command.client_sequence, applied.ingress_sequence,
                applied.applied, recovery_error(applied.reason));
            auto ticket = durable_writer->append(std::move(outcome_record));
            if (!ticket.ok()) {
                std::cerr << "durable 恢复终态写入失败: "
                          << geoworld::persistence::error_code(ticket.error) << '\n';
                return 5;
            }
            const auto committed = ticket.value.wait();
            if (!committed.ok()
                || !core.restore_durable_record(
                    geoworld::gateway::DurableRecordKind::command_outcome,
                    committed.lsn.value,
                    geoworld::gateway::encode_command_outcome_record(
                        replayed.command.principal_id, replayed.command.request_id,
                        replayed.command.client_sequence, applied.ingress_sequence,
                        applied.applied, recovery_error(applied.reason)))) {
                std::cerr << "durable 恢复终态提交失败\n";
                return 5;
            }
        }
        core.set_durable_log(
            geoworld::gateway::make_persistence_admission_log(durable_writer));
        runtime.add_checkpoint_anchor_callback(
            [&checkpoint_registry, &checkpoint_coordinator, &checkpoint_publish,
             &durable_writer, &config](std::uint64_t completed_tick,
                                      std::uint64_t state_hash) {
                if (config.checkpoint_interval_ticks == 0
                    || (completed_tick + 1) % config.checkpoint_interval_ticks != 0) {
                    return;
                }
                if (checkpoint_publish.valid()
                    && checkpoint_publish.wait_for(std::chrono::seconds{0})
                           != std::future_status::ready) {
                    return;
                }
                if (checkpoint_publish.valid() && !checkpoint_publish.get().ok()) {
                    std::cerr << "后台检查点发布失败\n";
                }
                geoworld::persistence::CheckpointAnchor anchor{
                    completed_tick, completed_tick + 1,
                    durable_writer->last_durable_lsn(), state_hash};
                auto captured = checkpoint_coordinator->capture(checkpoint_registry, anchor);
                if (!captured.ok()) {
                    std::cerr << "检查点冻结失败: "
                              << geoworld::persistence::error_code(captured.error) << '\n';
                    return;
                }
                checkpoint_publish = std::async(
                    std::launch::async,
                    [&checkpoint_registry, coordinator = checkpoint_coordinator.get(),
                     state = std::move(captured.value)]() mutable {
                        return coordinator->publish(checkpoint_registry, std::move(state));
                    });
            });
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
#if GW_BUILD_M5
    if (!recovered_existing_state) {
#endif
        static_cast<void>(runtime.submit(
            0, geoworld::simulation::CreateObjectCommand{seed}));
#if GW_BUILD_M5
    }
#endif

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "geoworldd: control=" << config.control_address << ":"
              << control.bound_port()
              << " stream=" << config.stream_address << ":" << transport.bound_port()
              << '\n' << std::flush;

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
#if GW_BUILD_M5
        if (durable_writer && config.hash_interval_ticks != 0
            && (step.tick + 1) % config.hash_interval_ticks == 0) {
            geoworld::persistence::WalRecord hash_record;
            hash_record.kind = geoworld::persistence::WalRecordKind::state_hash;
            hash_record.target_tick = step.tick;
            hash_record.payload = geoworld::persistence::encode_state_hash_point(
                {step.tick, step.state_hash});
            auto ticket = durable_writer->append(std::move(hash_record));
            if (ticket.ok()) {
                hash_tickets.push_back(std::move(ticket.value));
            }
        }
        std::erase_if(hash_tickets, [](const auto& ticket) {
            const auto outcome = ticket.wait_for(std::chrono::milliseconds{0});
            if (!outcome.has_value()) return false;
            if (!outcome->ok()) {
                std::cerr << "状态 hash WAL 写入失败: "
                          << geoworld::persistence::error_code(outcome->error) << '\n';
                g_stop = 1;
            }
            return true;
        });
#endif
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
              << " objects=" << runtime.world().size() << '\n' << std::flush;
    transport.shutdown();
    control.shutdown();
#if GW_BUILD_M5
    if (checkpoint_publish.valid()) {
        const auto published = checkpoint_publish.get();
        if (!published.ok()) {
            std::cerr << "后台检查点发布失败: "
                      << geoworld::persistence::error_code(published.error) << '\n';
        }
    }
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
