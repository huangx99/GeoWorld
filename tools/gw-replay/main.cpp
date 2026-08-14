#include "geoworld/debug/state_hash.hpp"
#include "geoworld/gateway/durable.hpp"
#include "geoworld/persistence/branch.hpp"
#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/recovery.hpp"
#include "geoworld/runtime/world_runtime.hpp"
#include "geoworld/world/snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string_view>

namespace {

using geoworld::foundation::WorldId;
using namespace geoworld::persistence;

struct Input {
    std::filesystem::path root;
    WorldId world;
    BranchId branch;
};

struct ReplayResult {
    geoworld::runtime::WorldRuntime runtime;
    std::map<std::uint64_t, std::uint64_t> recorded_hashes;
    Lsn last_lsn{};
};

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

std::optional<Input> parse_input(char** argv, int offset) {
    Input input;
    input.root = argv[offset];
    const std::string_view world_text{argv[offset + 1]};
    const auto parsed = std::from_chars(world_text.data(), world_text.data() + world_text.size(),
                                        input.world.value);
    const auto branch = parse_branch_id(argv[offset + 2]);
    if (parsed.ec != std::errc{} || parsed.ptr != world_text.data() + world_text.size()
        || !input.world.valid() || !branch.has_value()) return std::nullopt;
    input.branch = *branch;
    return input;
}

Result<ReplayResult> replay(const Input& input) {
    Result<ReplayResult> result;
    const auto layout = make_durable_layout(input.root, input.world, input.branch);
    RecoveryPlanner planner{layout, input.world, input.branch};
    auto plan = planner.build(TailPolicy::strict);
    if (!plan.ok()) return {{}, plan.error};
    CheckpointRegistry registry;
    register_providers(registry, result.value.runtime);
    if (plan.value.checkpoint.has_value()) {
        CheckpointLoader loader{layout, input.world, input.branch};
        const auto error = loader.restore_into(registry, *plan.value.checkpoint);
        if (error != PersistenceError::none) return {{}, error};
    }
    std::uint64_t until{};
    bool advance = false;
    for (const auto& record : plan.value.replay_records) {
        if (record.kind == WalRecordKind::external_command) {
            auto command = geoworld::gateway::decode_external_command_record(record.payload);
            if (!command.has_value()) return {{}, PersistenceError::record_invalid};
            geoworld::simulation::CommandMeta meta;
            meta.durable_lsn = record.lsn.value;
            meta.expected_object_version = command->expected_object_version;
            if (result.value.runtime.submit(command->target_tick,
                                            std::move(command->payload), meta) == 0)
                return {{}, PersistenceError::record_invalid};
            until = std::max(until, command->target_tick);
            advance = true;
        } else if (record.kind == WalRecordKind::state_hash) {
            const auto point = decode_state_hash_point(record.payload);
            if (!point.has_value()) return {{}, PersistenceError::record_invalid};
            result.value.recorded_hashes.insert_or_assign(point->tick, point->hash);
            until = std::max(until, point->tick);
            advance = true;
        } else if (record.kind != WalRecordKind::command_outcome
                   && record.kind != WalRecordKind::checkpoint_marker) {
            return {{}, PersistenceError::provider_unknown};
        }
    }
    while (advance && static_cast<std::uint64_t>(result.value.runtime.clock().tick()) <= until) {
        const auto step = result.value.runtime.step();
        const auto expected = result.value.recorded_hashes.find(step.tick);
        if (expected != result.value.recorded_hashes.end() && expected->second != step.state_hash)
            return {{}, PersistenceError::checkpoint_invalid};
    }
    result.value.last_lsn = plan.value.last_lsn;
    return result;
}

int inspect(const Input& input) {
    const auto layout = make_durable_layout(input.root, input.world, input.branch);
    RecoveryPlanner planner{layout, input.world, input.branch};
    auto plan = planner.build(TailPolicy::strict);
    if (!plan.ok()) {
        std::cerr << "校验失败: " << error_code(plan.error) << '\n';
        return 3;
    }
    std::cout << "世界: " << input.world.value << "\n分支: " << format_branch_id(input.branch)
              << "\n最后 LSN: " << plan.value.last_lsn.value << '\n';
    if (plan.value.checkpoint.has_value()) {
        const auto& checkpoint = *plan.value.checkpoint;
        std::cout << "检查点 tick: " << checkpoint.info.anchor.completed_tick
                  << "\n检查点 LSN: " << checkpoint.info.anchor.included_lsn.value
                  << "\n世界状态 hash: " << checkpoint.info.anchor.world_state_hash
                  << "\n检查点内容 hash: " << checkpoint.info.checkpoint_content_hash
                  << "\nProvider 数: " << checkpoint.blocks.size() << '\n';
        for (const auto& block : checkpoint.blocks)
            std::cout << "  " << block.schema.provider_id << " v"
                      << block.schema.schema_version << " " << block.payload.size() << " 字节\n";
    } else {
        std::cout << "检查点: 无\n";
    }
    std::cout << "待回放记录: " << plan.value.replay_records.size() << '\n';
    return 0;
}

int compare(const Input& left, const Input& right, bool bisect_only) {
    auto a = replay(left);
    auto b = replay(right);
    if (!a.ok() || !b.ok()) {
        std::cerr << "回放失败: " << error_code(a.ok() ? b.error : a.error) << '\n';
        return 3;
    }
    std::optional<std::uint64_t> first;
    auto ai = a.value.recorded_hashes.begin();
    auto bi = b.value.recorded_hashes.begin();
    while (ai != a.value.recorded_hashes.end() && bi != b.value.recorded_hashes.end()) {
        if (ai->first != bi->first || ai->second != bi->second) {
            first = std::min(ai->first, bi->first);
            break;
        }
        ++ai; ++bi;
    }
    if (!first.has_value() && (ai != a.value.recorded_hashes.end()
                               || bi != b.value.recorded_hashes.end()))
        first = ai != a.value.recorded_hashes.end() ? ai->first : bi->first;
    if (first.has_value()) std::cout << "首个 hash 分叉 tick: " << *first << '\n';
    else std::cout << "记录 hash 序列一致\n";
    if (!bisect_only) {
        const auto differences = compare_world_snapshots(
            geoworld::world::capture_snapshot(a.value.runtime.world()),
            geoworld::world::capture_snapshot(b.value.runtime.world()));
        std::cout << "对象差异数: " << differences.size() << '\n';
        for (const auto& difference : differences)
            std::cout << "  WID=" << difference.id.value << " 类型="
                      << static_cast<int>(difference.kind) << '\n';
    }
    return first.has_value() ? 4 : 0;
}

int fork_branch(const Input& parent, BranchId child,
                std::optional<std::uint64_t> requested_tick) {
    const auto parent_layout = make_durable_layout(parent.root, parent.world, parent.branch);
    const auto child_layout = make_durable_layout(parent.root, parent.world, child);
    auto created = fork_branch_at_checkpoint(parent_layout, child_layout, parent.world,
                                             parent.branch, child, requested_tick);
    if (!created.ok()) {
        std::cerr << "分支创建失败: " << error_code(created.error) << '\n';
        return 3;
    }
    std::cout << "分支已创建: " << format_branch_id(child)
              << "\n基线未复制: " << created.value.base_checkpoint << '\n';
    return 0;
}

int compact(const Input& input, const std::filesystem::path& output_root) {
    auto executed = replay(input);
    if (!executed.ok()) {
        std::cerr << "回放失败: " << error_code(executed.error) << '\n';
        return 3;
    }
    CheckpointRegistry registry;
    register_providers(registry, executed.value.runtime);
    CheckpointConfig config;
    config.layout = make_durable_layout(output_root, input.world, input.branch);
    config.world = input.world;
    config.branch = input.branch;
    config.authoritative_modules =
        geoworld::runtime::WorldRuntime::authoritative_state_modules();
    CheckpointCoordinator coordinator{config};
    const auto resume = static_cast<std::uint64_t>(executed.value.runtime.clock().tick());
    if (resume == 0) {
        std::cerr << "空世界没有已完成 tick，无法 compact\n";
        return 3;
    }
    CheckpointAnchor anchor{resume - 1, resume, executed.value.last_lsn,
                            geoworld::debug::world_state_hash(executed.value.runtime.world())};
    auto captured = coordinator.capture(registry, anchor);
    if (!captured.ok()) return 3;
    auto published = coordinator.publish(registry, std::move(captured.value));
    if (!published.ok()) return 3;
    std::cout << "新检查点: " << published.value.manifest_path << '\n';
    return 0;
}

constexpr std::string_view kUsage =
    "用法:\n"
    "  gw-replay inspect|verify|replay <root> <world_id> <branch_uuid>\n"
    "  gw-replay compare|bisect <rootA> <worldA> <branchA> <rootB> <worldB> <branchB>\n"
    "  gw-replay fork <root> <world_id> <parent_branch> <new_branch> [checkpoint_tick]\n"
    "  gw-replay compact <root> <world_id> <branch_uuid> <output_root>";

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << kUsage << '\n';
        return 2;
    }
    const std::string_view command{argv[1]};
    const auto input = parse_input(argv, 2);
    if (!input.has_value()) {
        std::cerr << "输入参数非法\n";
        return 2;
    }
    if (command == "inspect") return inspect(*input);
    if (command == "verify") {
        const auto result = replay(*input);
        if (!result.ok()) {
            std::cerr << "校验失败: " << error_code(result.error) << '\n';
            return 3;
        }
        std::cout << "校验通过\n";
        return 0;
    }
    if (command == "replay") {
        const auto result = replay(*input);
        if (!result.ok()) {
            std::cerr << "回放失败: " << error_code(result.error) << '\n';
            return 3;
        }
        std::cout << "回放完成: tick=" << result.value.runtime.clock().tick()
                  << " objects=" << result.value.runtime.world().size()
                  << " hash=" << geoworld::debug::world_state_hash(result.value.runtime.world())
                  << '\n';
        return 0;
    }
    if ((command == "compare" || command == "bisect") && argc == 8) {
        const auto right = parse_input(argv, 5);
        return right.has_value() ? compare(*input, *right, command == "bisect") : 2;
    }
    if (command == "fork" && (argc == 6 || argc == 7)) {
        const auto child = parse_branch_id(argv[5]);
        std::optional<std::uint64_t> tick;
        if (argc == 7) {
            std::uint64_t parsed_tick{};
            const std::string_view text{argv[6]};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                                parsed_tick);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return 2;
            tick = parsed_tick;
        }
        return child.has_value() ? fork_branch(*input, *child, tick) : 2;
    }
    if (command == "compact" && argc == 6) return compact(*input, argv[5]);
    std::cerr << kUsage << '\n';
    return 2;
}
