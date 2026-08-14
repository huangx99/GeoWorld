#include "geoworld/persistence/branch.hpp"
#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/recovery.hpp"
#include "geoworld/persistence/wal.hpp"

#include <filesystem>
#include <memory>

#include <unistd.h>

namespace {

using namespace geoworld::persistence;
using geoworld::foundation::WorldId;

geoworld::world::WorldObject object(std::uint64_t id, double x) {
    geoworld::world::WorldObject result;
    result.id = WorldId{id};
    result.semantic_type = "branch.entity";
    result.position.x = x;
    result.lifecycle = geoworld::world::LifecycleState::active;
    result.version = 1;
    result.revision = id;
    return result;
}

} // namespace

int main() {
    geoworld::world::World baseline_world;
    static_cast<void>(baseline_world.insert(object(1, 1.0)));
    static_cast<void>(baseline_world.insert(object(2, 2.0)));
    const auto baseline = geoworld::world::capture_snapshot(baseline_world);
    WorldOverlay overlay{geoworld::world::freeze_snapshot(baseline_world)};
    if (!baseline_world.update(WorldId{1}, [](auto& value) { value.position.x = 100.0; })
        || !baseline_world.erase(WorldId{2})
        || !baseline_world.insert(object(4, 4.0))
        || overlay.find(WorldId{1}) == nullptr
        || overlay.find(WorldId{1})->position.x != 1.0
        || overlay.find(WorldId{2}) == nullptr || overlay.find(WorldId{4}) != nullptr) return 1;
    if (overlay.find(WorldId{1}) == nullptr || !overlay.erase(WorldId{1})
        || overlay.find(WorldId{1}) != nullptr || !overlay.upsert(object(2, 20.0))
        || !overlay.upsert(object(3, 3.0))) return 2;
    const auto materialized = overlay.materialize();
    if (materialized.objects.size() != 2 || materialized.objects[0].id != WorldId{2}
        || materialized.objects[1].id != WorldId{3} || overlay.overlay_size() != 2
        || overlay.tombstone_count() != 1) return 3;
    const auto differences = compare_world_snapshots(baseline, materialized);
    if (differences.size() != 3) return 4;
    DisabledSideEffectSink sink;
    if (sink.emit("network", {})) return 5;

    const auto root = std::filesystem::temp_directory_path()
                      / ("gw-branch-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto parent = parse_branch_id("00000000-0000-0000-0000-000000000001").value();
    const auto child = parse_branch_id("00000000-0000-0000-0000-000000000002").value();
    const auto layout = make_durable_layout(root, WorldId{9}, child);
    BranchManifestData manifest;
    manifest.world = WorldId{9};
    manifest.branch = child;
    manifest.parent_branch = parent;
    manifest.fork_tick = 10;
    manifest.fork_lsn = Lsn{20};
    manifest.base_checkpoint = "/immutable/base/ckpt.gwckm";
    manifest.overlay_wal = layout.wal_dir();
    auto published = publish_branch_manifest(layout, manifest);
    if (!published.ok()) return 6;
    auto loaded = load_branch_manifest(published.value);
    if (!loaded.ok() || loaded.value.branch != child || loaded.value.parent_branch != parent
        || loaded.value.fork_tick != 10 || loaded.value.fork_lsn != Lsn{20}
        || loaded.value.side_effects_enabled) return 7;
    if (std::filesystem::exists(layout.checkpoint_dir())) return 8;

    const auto parent_layout = make_durable_layout(root, WorldId{9}, parent);
    geoworld::simulation::TickClock clock;
    CheckpointRegistry registry;
    static_cast<void>(registry.register_provider(make_world_provider(baseline_world)));
    static_cast<void>(registry.register_provider(make_clock_provider(clock)));
    CheckpointConfig checkpoint_config;
    checkpoint_config.layout = parent_layout;
    checkpoint_config.world = WorldId{9};
    checkpoint_config.branch = parent;
    CheckpointCoordinator coordinator{checkpoint_config};
    for (const std::uint64_t tick : {10ULL, 20ULL}) {
        auto captured = coordinator.capture(registry, {tick, tick + 1, Lsn{tick}, tick});
        if (!captured.ok() || !coordinator.publish(registry, std::move(captured.value)).ok())
            return 9;
    }
    const auto exact_child =
        parse_branch_id("00000000-0000-0000-0000-000000000003").value();
    const auto exact_layout = make_durable_layout(root, WorldId{9}, exact_child);
    const auto forked = fork_branch_at_checkpoint(parent_layout, exact_layout, WorldId{9},
                                                  parent, exact_child, 10);
    if (!forked.ok() || forked.value.fork_tick != 10 || forked.value.fork_lsn != Lsn{10}
        || std::filesystem::exists(exact_layout.checkpoint_dir())) return 10;
    RecoveryPlanner child_planner{exact_layout, WorldId{9}, exact_child};
    const auto child_recovery = child_planner.build(TailPolicy::strict);
    if (!child_recovery.ok() || !child_recovery.value.checkpoint.has_value()
        || child_recovery.value.checkpoint->info.anchor.completed_tick != 10
        || !child_recovery.value.replay_records.empty()) return 11;
    geoworld::world::World restored_world;
    geoworld::simulation::TickClock restored_clock;
    CheckpointRegistry restore_registry;
    static_cast<void>(restore_registry.register_provider(make_world_provider(restored_world)));
    static_cast<void>(restore_registry.register_provider(make_clock_provider(restored_clock)));
    CheckpointLoader child_loader{exact_layout, WorldId{9}, exact_child};
    if (child_loader.restore_into(restore_registry, *child_recovery.value.checkpoint)
            != PersistenceError::none
        || restored_world.size() != baseline_world.size()
        || restored_world.find(WorldId{1}) == nullptr
        || restored_world.find(WorldId{1})->position.x != 100.0) return 12;
    WalConfig child_wal_config;
    child_wal_config.durable_root = root;
    child_wal_config.world = WorldId{9};
    child_wal_config.branch = exact_child;
    child_wal_config.recovery_floor_lsn = Lsn{10};
    WalWriter child_writer{child_wal_config};
    if (!child_writer.start().ok()) return 13;
    WalRecord child_record;
    child_record.kind = WalRecordKind::checkpoint_marker;
    child_record.target_tick = 11;
    child_record.payload.assign(4, std::byte{'b'});
    auto child_ticket = child_writer.append(std::move(child_record));
    if (!child_ticket.ok()) return 13;
    const auto child_committed = child_ticket.value.wait();
    child_writer.shutdown();
    const auto child_with_overlay = child_planner.build(TailPolicy::strict);
    if (!child_committed.ok() || child_committed.lsn != Lsn{11}
        || !child_with_overlay.ok() || child_with_overlay.value.replay_records.size() != 1
        || child_with_overlay.value.replay_records.front().lsn != Lsn{11}) return 13;

    auto captured = coordinator.capture(registry, {30, 31, Lsn{30}, 30});
    if (!captured.ok() || !coordinator.publish(registry, std::move(captured.value)).ok())
        return 14;
    CheckpointLoader parent_loader{parent_layout, WorldId{9}, parent};
    const auto retained = parent_loader.list_valid();
    if (!retained.ok() || retained.value.size() != 2
        || retained.value[0].anchor.completed_tick != 10
        || retained.value[1].anchor.completed_tick != 30) return 15;
    const auto latest_child =
        parse_branch_id("00000000-0000-0000-0000-000000000004").value();
    const auto latest_layout = make_durable_layout(root, WorldId{9}, latest_child);
    const auto latest = fork_branch_at_checkpoint(parent_layout, latest_layout, WorldId{9},
                                                  parent, latest_child);
    if (!latest.ok() || latest.value.fork_tick != 30 || latest.value.fork_lsn != Lsn{30})
        return 16;
    const auto missing_child =
        parse_branch_id("00000000-0000-0000-0000-000000000005").value();
    const auto missing = fork_branch_at_checkpoint(
        parent_layout, make_durable_layout(root, WorldId{9}, missing_child), WorldId{9},
        parent, missing_child, 15);
    if (missing.error != PersistenceError::not_found) return 17;
    std::filesystem::remove_all(root, ec);
    return 0;
}
