// M5-B 集成：WorldRuntime 稳定 tick 边界捕获 -> 原子发布 -> eager 恢复 -> hash 一致。

#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/recovery.hpp"
#include "geoworld/persistence/wal.hpp"

#include "geoworld/debug/state_hash.hpp"
#include "geoworld/runtime/world_runtime.hpp"
#include "geoworld/simulation/command_buffer.hpp"
#include "geoworld/world/world.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

namespace simulation = geoworld::simulation;
namespace world = geoworld::world;

using geoworld::foundation::WorldId;
using geoworld::persistence::BranchId;
using geoworld::persistence::CheckpointAnchor;
using geoworld::persistence::CheckpointConfig;
using geoworld::persistence::CheckpointCoordinator;
using geoworld::persistence::CheckpointLoader;
using geoworld::persistence::CheckpointRegistry;
using geoworld::persistence::PersistenceError;

void register_runtime_providers(CheckpointRegistry& registry,
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

std::atomic<int> g_dir_counter{0};

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(std::string_view name) {
        path = std::filesystem::temp_directory_path()
               / ("gw-m5b-it-" + std::string{name} + "-" + std::to_string(::getpid()) + "-"
                  + std::to_string(++g_dir_counter));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

[[nodiscard]] world::WorldObject make_object(std::uint64_t id, double speed) {
    world::WorldObject object;
    object.id = WorldId{id};
    object.geometry_ref = "geom-" + std::to_string(id);
    object.semantic_type = "vehicle";
    object.position = world::PositionEcef{static_cast<double>(id), 2.0, 3.0};
    object.properties = {{"speed", speed}};
    object.lifecycle = world::LifecycleState::active;
    return object;
}

// 端到端：锚点捕获 -> 发布 -> 新世界恢复 -> hash/时钟一致 -> 无新输入推进 hash 稳定。
[[nodiscard]] bool capture_publish_restore_consistent() {
    TempDir dir("runtime");
    const auto branch =
        geoworld::persistence::parse_branch_id("01234567-89ab-cdef-0123-456789abcdef")
            .value_or(BranchId{});
    const auto layout =
        geoworld::persistence::make_durable_layout(dir.path, WorldId{7}, branch);

    // WAL 并行运行：锚点 included_lsn 取自 durable 边界，恢复后可从 included_lsn+1 重放。
    geoworld::persistence::WalConfig wal_config;
    wal_config.durable_root = dir.path;
    wal_config.world = WorldId{7};
    wal_config.branch = branch;
    geoworld::persistence::WalWriter wal_writer{wal_config};
    if (!wal_writer.start().ok()) {
        return false;
    }
    geoworld::persistence::Lsn last_durable;
    for (std::uint64_t index = 0; index < 2; ++index) {
        geoworld::persistence::WalRecord record;
        record.kind = geoworld::persistence::WalRecordKind::external_command;
        record.target_tick = index;
        record.payload = {std::byte{'c'}, std::byte{'m'}, std::byte{'d'}};
        auto ticket = wal_writer.append(std::move(record));
        if (!ticket.ok() || !ticket.value.wait().ok()) {
            return false;
        }
        last_durable = wal_writer.last_durable_lsn();
    }

    geoworld::runtime::WorldRuntime runtime;
    CheckpointRegistry registry;
    register_runtime_providers(registry, runtime);
    for (std::uint64_t lsn = 1; lsn <= last_durable.value; ++lsn) {
        simulation::CommandMeta meta;
        meta.durable_lsn = lsn;
        static_cast<void>(runtime.submit(
            100, simulation::SetPropertyCommand{WorldId{1}, "durable", 1.0}, meta));
    }

    CheckpointConfig checkpoint_config;
    checkpoint_config.world = WorldId{7};
    checkpoint_config.branch = branch;
    checkpoint_config.layout = layout;
    checkpoint_config.authoritative_modules =
        geoworld::runtime::WorldRuntime::authoritative_state_modules();
    CheckpointCoordinator coordinator{checkpoint_config};

    constexpr std::uint64_t capture_tick = 2;
    std::uint64_t captured_hash = 0;
    std::uint64_t checkpoint_content_hash = 0;
    bool captured = false;
    // 稳定边界锚点回调：Track A/B/C 完成、状态 hash 已计算、clock.advance 之前。
    runtime.add_checkpoint_anchor_callback(
        [&](std::uint64_t completed_tick, std::uint64_t state_hash) {
            if (completed_tick != capture_tick || captured) {
                return;
            }
            captured = true;
            captured_hash = state_hash;
            const CheckpointAnchor anchor{completed_tick, completed_tick + 1, last_durable,
                                          state_hash};
            auto captured_checkpoint = coordinator.capture(registry, anchor);
            if (!captured_checkpoint.ok()) {
                captured_hash = 0;
                return;
            }
            const auto published =
                coordinator.publish(registry, std::move(captured_checkpoint.value));
            if (!published.ok()) {
                captured_hash = 0;
                return;
            }
            if (published.value.anchor.world_state_hash != captured_hash) {
                captured_hash = 0;
                return;
            }
            checkpoint_content_hash = published.value.checkpoint_content_hash;
        });

    static_cast<void>(runtime.submit(0, simulation::CreateObjectCommand{make_object(1, 10.0)}));
    static_cast<void>(runtime.submit(0, simulation::CreateObjectCommand{make_object(2, 20.0)}));
    static_cast<void>(
        runtime.submit(1, simulation::SetPropertyCommand{WorldId{1}, "speed", 11.5}));
    for (int step = 0; step < 4; ++step) {
        static_cast<void>(runtime.step());
    }
    if (!captured || captured_hash == 0) {
        return false;
    }
    // 捕获后继续演化：恢复不得观察这些变化。
    static_cast<void>(runtime.submit(4, simulation::CreateObjectCommand{make_object(3, 30.0)}));
    static_cast<void>(runtime.step());
    const std::uint64_t diverged_hash =
        geoworld::debug::world_state_hash(runtime.world());
    if (diverged_hash == captured_hash) {
        return false;
    }

    // eager 恢复到全新运行时。
    geoworld::runtime::WorldRuntime restored;
    CheckpointRegistry restore_registry;
    register_runtime_providers(restore_registry, restored);
    CheckpointLoader loader{layout, WorldId{7}, branch};
    auto latest = loader.latest_valid();
    if (!latest.ok() || latest.value.anchor.completed_tick != capture_tick
        || latest.value.anchor.resume_tick != capture_tick + 1
        || latest.value.anchor.included_lsn != geoworld::persistence::Lsn{2}
        || latest.value.anchor.world_state_hash != captured_hash
        || latest.value.checkpoint_content_hash != checkpoint_content_hash
        || checkpoint_content_hash == 0) {
        return false;
    }
    auto loaded = loader.load(latest.value);
    if (!loaded.ok()
        || loader.restore_into(restore_registry, loaded.value) != PersistenceError::none) {
        return false;
    }
    // 验收门 5：完整检查点恢复后，无新输入时权威 hash 与捕获时一致。
    if (geoworld::debug::world_state_hash(restored.world()) != captured_hash) {
        return false;
    }
    if (restored.clock().tick() != static_cast<std::int64_t>(capture_tick + 1)) {
        return false;
    }
    const auto step_result = restored.step();
    return step_result.tick == capture_tick + 1 && step_result.state_hash == captured_hash;
}

// durable 启动闸门：缺 provider 时拒绝，完整注册时通过。
[[nodiscard]] bool durable_start_requires_all_stateful_modules() {
    world::World world;
    simulation::TickClock clock{};
    CheckpointRegistry registry;
    static_cast<void>(registry.register_provider(geoworld::persistence::make_world_provider(world)));
    static_cast<void>(registry.register_provider(geoworld::persistence::make_clock_provider(clock)));
    const auto& modules = geoworld::runtime::WorldRuntime::authoritative_state_modules();
    if (modules.empty()) {
        return false;
    }
    if (registry.validate_completeness(modules) != PersistenceError::provider_missing) {
        return false;
    }
    geoworld::runtime::WorldRuntime runtime;
    CheckpointRegistry complete;
    register_runtime_providers(complete, runtime);
    if (complete.validate_completeness(modules) != PersistenceError::none) {
        return false;
    }
    TempDir dir("watermark");
    CheckpointConfig config;
    config.world = WorldId{8};
    config.branch = geoworld::persistence::parse_branch_id(
        "00000000-0000-0000-0000-000000000008").value_or(BranchId{});
    config.layout = geoworld::persistence::make_durable_layout(
        dir.path, config.world, config.branch);
    config.authoritative_modules = modules;
    CheckpointCoordinator coordinator{config};
    auto before_admission = coordinator.capture(complete, {0, 1, {9}, 1});
    if (!before_admission.ok() || before_admission.value.anchor.included_lsn.valid()) {
        return false;
    }
    simulation::CommandMeta meta;
    meta.durable_lsn = 5;
    if (runtime.submit(100, simulation::DestroyObjectCommand{WorldId{1}}, meta) == 0) {
        return false;
    }
    auto after_admission = coordinator.capture(complete, {0, 1, {9}, 1});
    return after_admission.ok()
           && after_admission.value.anchor.included_lsn
                  == geoworld::persistence::Lsn{5};
}

} // namespace

int main() {
    if (!capture_publish_restore_consistent()) {
        return 1;
    }
    if (!durable_start_requires_all_stateful_modules()) {
        return 2;
    }
    return 0;
}
