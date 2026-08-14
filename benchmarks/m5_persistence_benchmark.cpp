#include "geoworld/debug/state_hash.hpp"
#include "geoworld/persistence/branch.hpp"
#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/recovery.hpp"
#include "geoworld/persistence/wal.hpp"
#include "geoworld/runtime/world_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <string_view>
#include <thread>

#include <sys/utsname.h>
#include <sys/resource.h>
#include <unistd.h>

#ifndef GW_BENCH_BUILD_TYPE
#define GW_BENCH_BUILD_TYPE "unknown"
#endif
#ifndef GW_BENCH_COMPILER
#define GW_BENCH_COMPILER "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace geoworld::persistence;

inline constexpr std::uint64_t kBenchmarkSegmentMaxBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kBenchmarkPayloadBytes = 64;
inline constexpr auto kDiskSampleInterval = std::chrono::seconds{1};

struct Config {
    std::filesystem::path root{"m5-benchmark-data"};
    std::size_t entities{100'000};
    std::uint64_t duration_seconds{600};
    std::uint64_t checkpoint_interval_ticks{1'000};
    std::size_t wal_records_per_tick{100};
    std::uint64_t wal_segment_max_bytes{kBenchmarkSegmentMaxBytes};
    std::size_t branch_count{8};
};

bool parse(int argc, char** argv, Config& config) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag{argv[index]};
        if (index + 1 >= argc) return false;
        const char* value = argv[++index];
        if (flag == "--root") config.root = value;
        else if (flag == "--entities") config.entities = std::stoull(value);
        else if (flag == "--duration-seconds") config.duration_seconds = std::stoull(value);
        else if (flag == "--checkpoint-interval-ticks")
            config.checkpoint_interval_ticks = std::stoull(value);
        else if (flag == "--wal-records-per-tick")
            config.wal_records_per_tick = std::stoull(value);
        else if (flag == "--wal-segment-max-bytes")
            config.wal_segment_max_bytes = std::stoull(value);
        else if (flag == "--branch-count") config.branch_count = std::stoull(value);
        else return false;
    }
    return config.entities > 0 && config.duration_seconds > 0
           && config.wal_records_per_tick > 0 && config.wal_segment_max_bytes > 0;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::vector<double> milliseconds(const std::vector<std::uint64_t>& nanoseconds) {
    std::vector<double> result;
    result.reserve(nanoseconds.size());
    for (const std::uint64_t value : nanoseconds)
        result.push_back(static_cast<double>(value) / 1'000'000.0);
    return result;
}

std::uintmax_t directory_bytes(const std::filesystem::path& root) {
    std::uintmax_t bytes{};
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator iterator(root, ec), end;
         iterator != end && !ec; iterator.increment(ec)) {
        if (iterator->is_regular_file(ec)) bytes += iterator->file_size(ec);
    }
    return bytes;
}

std::size_t temporary_file_count(const std::filesystem::path& root) {
    std::size_t count{};
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator iterator(root, ec), end;
         iterator != end && !ec; iterator.increment(ec)) {
        if (iterator->is_regular_file(ec)
            && iterator->path().filename().string().starts_with(kTempFilePrefix)) ++count;
    }
    return count;
}

std::uint64_t resident_bytes() {
    std::ifstream statm{"/proc/self/statm"};
    std::uint64_t total_pages{};
    std::uint64_t resident_pages{};
    if (!(statm >> total_pages >> resident_pages)) return 0;
    static_cast<void>(total_pages);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    return page_size > 0 ? resident_pages * static_cast<std::uint64_t>(page_size) : 0;
}

void register_providers(CheckpointRegistry& registry,
                        geoworld::runtime::WorldRuntime& runtime) {
    static_cast<void>(registry.register_provider(make_world_provider(runtime.world_for_restore())));
    static_cast<void>(registry.register_provider(make_clock_provider(runtime.clock_for_restore())));
    static_cast<void>(registry.register_provider(
        make_command_buffer_provider(runtime.commands_for_restore())));
    static_cast<void>(registry.register_provider(make_event_bus_provider(runtime.events_for_restore())));
    static_cast<void>(registry.register_provider(
        make_ai_intents_provider(runtime.ai_intents_for_restore())));
    static_cast<void>(registry.register_provider(
        make_random_streams_provider(runtime.random_streams_for_restore())));
    static_cast<void>(registry.register_provider(
        make_artifacts_provider(runtime.artifacts_for_restore())));
    static_cast<void>(registry.register_provider(
        make_ecs_active_set_provider(runtime.ecs_for_restore(), runtime.world_for_restore())));
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    if (!parse(argc, argv, config)) {
        std::cerr << "用法: geoworld-m5-persistence-benchmark --root dir --entities n "
                     "--duration-seconds n --checkpoint-interval-ticks n（0=禁用） "
                     "--wal-records-per-tick n --wal-segment-max-bytes n "
                     "--branch-count n\n";
        return 2;
    }
    const geoworld::foundation::WorldId world_id{1};
    const auto branch = parse_branch_id("00000000-0000-0000-0000-000000000001").value();
    std::error_code ec;
    std::filesystem::remove_all(config.root, ec);

    geoworld::runtime::WorldRuntime runtime;
    for (std::size_t index = 0; index < config.entities; ++index) {
        geoworld::world::WorldObject object;
        object.id = geoworld::foundation::WorldId{index + 1};
        object.semantic_type = "benchmark.entity";
        object.lifecycle = geoworld::world::LifecycleState::active;
        object.position = {static_cast<double>(index), static_cast<double>(index % 1'000), 0.0};
        object.properties.emplace("strength", static_cast<std::int64_t>(index % 100 + 1));
        if (!runtime.world_for_restore().insert(std::move(object))) return 3;
    }

    WalConfig wal_config;
    wal_config.durable_root = config.root;
    wal_config.world = world_id;
    wal_config.branch = branch;
    wal_config.segment_max_bytes = config.wal_segment_max_bytes;
    WalWriter writer{wal_config};
    if (!writer.start().ok()) return 3;

    CheckpointRegistry registry;
    register_providers(registry, runtime);
    CheckpointConfig checkpoint_config;
    checkpoint_config.layout = make_durable_layout(config.root, world_id, branch);
    checkpoint_config.world = world_id;
    checkpoint_config.branch = branch;
    checkpoint_config.authoritative_modules =
        geoworld::runtime::WorldRuntime::authoritative_state_modules();
    checkpoint_config.prune_wal_after_publish = true;
    CheckpointCoordinator coordinator{checkpoint_config};

    std::vector<double> append_ms;
    std::vector<double> tick_ms;
    std::vector<double> capture_ms;
    std::vector<double> publish_ms;
    std::future<Result<PublishedCheckpoint>> pending_publish;
    Clock::time_point publish_started;
    PersistenceError capture_error{PersistenceError::none};
    std::vector<std::uintmax_t> disk_samples;
    std::vector<std::uint64_t> rss_samples;
    std::uint64_t checkpoints{};
    std::uint64_t records{};
    runtime.add_checkpoint_anchor_callback(
        [&](std::uint64_t completed_tick, std::uint64_t state_hash) {
            if (config.checkpoint_interval_ticks == 0
                || (completed_tick + 1) % config.checkpoint_interval_ticks != 0
                || pending_publish.valid()) {
                return;
            }
            const auto capture_started = Clock::now();
            auto captured = coordinator.capture(
                registry, {completed_tick, completed_tick + 1,
                           writer.last_durable_lsn(), state_hash});
            capture_ms.push_back(milliseconds(Clock::now() - capture_started));
            if (!captured.ok()) {
                capture_error = captured.error;
                return;
            }
            publish_started = Clock::now();
            pending_publish = std::async(
                std::launch::async,
                [&registry, &coordinator, state = std::move(captured.value)]() mutable {
                    return coordinator.publish(registry, std::move(state));
                });
        });
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds{config.duration_seconds};
    auto next_disk_sample = started + kDiskSampleInterval;
    while (Clock::now() < deadline) {
        if (pending_publish.valid()
            && pending_publish.wait_for(std::chrono::milliseconds{0})
                   == std::future_status::ready) {
            if (!pending_publish.get().ok()) return 5;
            publish_ms.push_back(milliseconds(Clock::now() - publish_started));
            ++checkpoints;
        }
        struct PendingAppend {
            AppendTicket ticket;
            Clock::time_point started;
        };
        std::vector<PendingAppend> pending_appends;
        pending_appends.reserve(config.wal_records_per_tick);
        for (std::size_t index = 0; index < config.wal_records_per_tick; ++index) {
            WalRecord record;
            record.kind = WalRecordKind::checkpoint_marker;
            record.target_tick = static_cast<std::uint64_t>(runtime.clock().tick());
            record.payload.assign(kBenchmarkPayloadBytes,
                                  static_cast<std::byte>(records & 0xffU));
            const auto append_started = Clock::now();
            auto ticket = writer.append(std::move(record));
            if (!ticket.ok()) return 4;
            pending_appends.push_back(PendingAppend{std::move(ticket.value), append_started});
            ++records;
        }
        for (const PendingAppend& append : pending_appends) {
            if (!append.ticket.wait().ok()) return 4;
            append_ms.push_back(milliseconds(Clock::now() - append.started));
        }
        const auto tick_started = Clock::now();
        static_cast<void>(runtime.step());
        if (capture_error != PersistenceError::none) return 5;
        tick_ms.push_back(milliseconds(Clock::now() - tick_started));
        if (Clock::now() >= next_disk_sample) {
            disk_samples.push_back(directory_bytes(config.root));
            rss_samples.push_back(resident_bytes());
            next_disk_sample += kDiskSampleInterval;
        }
    }
    if (pending_publish.valid()) {
        if (!pending_publish.get().ok()) return 5;
        publish_ms.push_back(milliseconds(Clock::now() - publish_started));
        ++checkpoints;
    }
    writer.shutdown();
    const WalWriterMetrics wal_metrics = writer.metrics();
    disk_samples.push_back(directory_bytes(config.root));
    rss_samples.push_back(resident_bytes());

    double restore_ms{};
    std::uint64_t final_hash = geoworld::debug::world_state_hash(runtime.world());
    std::optional<CheckpointInfo> latest_checkpoint;
    geoworld::world::FrozenWorldSnapshot branch_baseline;
    if (checkpoints != 0) {
        geoworld::runtime::WorldRuntime restored;
        CheckpointRegistry restore_registry;
        register_providers(restore_registry, restored);
        CheckpointLoader loader{checkpoint_config.layout, world_id, branch};
        const auto restore_started = Clock::now();
        auto latest = loader.latest_valid();
        if (!latest.ok()) return 6;
        latest_checkpoint = latest.value;
        auto loaded = loader.load(latest.value);
        if (!loaded.ok() || loader.restore_into(restore_registry, loaded.value)
                                != PersistenceError::none) return 6;
        restore_ms = milliseconds(Clock::now() - restore_started);
        final_hash = geoworld::debug::world_state_hash(restored.world());
        branch_baseline = geoworld::world::freeze_snapshot(restored.world());
    }

    std::uintmax_t branch_disk_increment{};
    std::uint64_t branch_rss_increment{};
    std::size_t branch_overlay_objects{};
    if (latest_checkpoint.has_value() && config.branch_count != 0) {
        const auto disk_before = directory_bytes(config.root);
        const auto rss_before = resident_bytes();
        std::vector<WorldOverlay> overlays;
        overlays.reserve(config.branch_count);
        for (std::size_t index = 0; index < config.branch_count; ++index) {
            const BranchId child = generate_branch_id();
            const DurableLayout child_layout = make_durable_layout(config.root, world_id, child);
            auto forked = fork_branch_at_checkpoint(
                checkpoint_config.layout, child_layout, world_id, branch, child,
                latest_checkpoint->anchor.completed_tick);
            if (!forked.ok() || std::filesystem::exists(child_layout.checkpoint_dir())) return 7;
            overlays.emplace_back(branch_baseline);
            const auto id = geoworld::foundation::WorldId{index % config.entities + 1};
            const auto* baseline_object = branch_baseline.find(id);
            if (baseline_object == nullptr) return 7;
            auto changed = *baseline_object;
            changed.position.x += static_cast<double>(index + 1);
            if (!overlays.back().upsert(std::move(changed))) return 7;
            branch_overlay_objects += overlays.back().overlay_size();
        }
        const auto disk_after = directory_bytes(config.root);
        const auto rss_after = resident_bytes();
        branch_disk_increment = disk_after >= disk_before ? disk_after - disk_before : 0;
        branch_rss_increment = rss_after >= rss_before ? rss_after - rss_before : 0;
    }

    struct utsname system{};
    static_cast<void>(uname(&system));
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    const std::uintmax_t disk_bytes = directory_bytes(config.root);
    const auto steady_begin = disk_samples.begin()
        + static_cast<std::ptrdiff_t>(disk_samples.size() * 3 / 4);
    const auto [steady_min, steady_max] =
        std::minmax_element(steady_begin, disk_samples.end());
    const auto rss_steady_begin = rss_samples.begin()
        + static_cast<std::ptrdiff_t>(rss_samples.size() * 3 / 4);
    const auto [rss_steady_min, rss_steady_max] =
        std::minmax_element(rss_steady_begin, rss_samples.end());
    struct rusage usage{};
    static_cast<void>(getrusage(RUSAGE_SELF, &usage));
    const std::uint64_t max_rss_bytes =
        static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
    const auto sync_ms = milliseconds(wal_metrics.sync_nanoseconds);
    const auto group_commit_ms = milliseconds(wal_metrics.group_commit_nanoseconds);
    const auto rotation_ms = milliseconds(wal_metrics.rotation_nanoseconds);
    std::cout << "{\n"
              << "  \"build_type\": \"" << GW_BENCH_BUILD_TYPE << "\",\n"
              << "  \"compiler\": \"" << GW_BENCH_COMPILER << "\",\n"
              << "  \"system\": \"" << system.sysname << ' ' << system.release << "\",\n"
              << "  \"hardware_threads\": " << std::thread::hardware_concurrency() << ",\n"
              << "  \"entities\": " << config.entities << ",\n"
              << "  \"duration_seconds\": " << elapsed << ",\n"
              << "  \"wal_records\": " << records << ",\n"
              << "  \"wal_records_per_second\": " << records / elapsed << ",\n"
              << "  \"append_p50_ms\": " << percentile(append_ms, 0.50) << ",\n"
              << "  \"append_p95_ms\": " << percentile(append_ms, 0.95) << ",\n"
              << "  \"append_p99_ms\": " << percentile(append_ms, 0.99) << ",\n"
              << "  \"group_commit_batches\": " << wal_metrics.group_commit_batches << ",\n"
              << "  \"group_commit_p99_ms\": " << percentile(group_commit_ms, 0.99) << ",\n"
              << "  \"sync_p50_ms\": " << percentile(sync_ms, 0.50) << ",\n"
              << "  \"sync_p95_ms\": " << percentile(sync_ms, 0.95) << ",\n"
              << "  \"sync_p99_ms\": " << percentile(sync_ms, 0.99) << ",\n"
              << "  \"rotation_count\": " << rotation_ms.size() << ",\n"
              << "  \"rotation_p99_ms\": " << percentile(rotation_ms, 0.99) << ",\n"
              << "  \"tick_p99_ms\": " << percentile(tick_ms, 0.99) << ",\n"
              << "  \"capture_pause_p99_ms\": " << percentile(capture_ms, 0.99) << ",\n"
              << "  \"checkpoint_publish_p99_ms\": " << percentile(publish_ms, 0.99) << ",\n"
              << "  \"checkpoint_count\": " << checkpoints << ",\n"
              << "  \"restore_ms\": " << restore_ms << ",\n"
              << "  \"disk_bytes\": " << disk_bytes << ",\n"
              << "  \"steady_disk_min_bytes\": " << *steady_min << ",\n"
              << "  \"steady_disk_max_bytes\": " << *steady_max << ",\n"
              << "  \"temporary_files\": " << temporary_file_count(config.root) << ",\n"
              << "  \"branch_count\": " << config.branch_count << ",\n"
              << "  \"branch_baseline_bytes_copied\": 0,\n"
              << "  \"branch_disk_increment_bytes\": " << branch_disk_increment << ",\n"
              << "  \"branch_rss_increment_bytes\": " << branch_rss_increment << ",\n"
              << "  \"branch_overlay_objects\": " << branch_overlay_objects << ",\n"
              << "  \"max_rss_bytes\": " << max_rss_bytes << ",\n"
              << "  \"steady_rss_min_bytes\": " << *rss_steady_min << ",\n"
              << "  \"steady_rss_max_bytes\": " << *rss_steady_max << ",\n"
              << "  \"final_hash\": " << final_hash
              << "\n}\n";
    return 0;
}
