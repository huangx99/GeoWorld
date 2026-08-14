#include "geoworld/debug/state_hash.hpp"
#include "geoworld/gateway/durable.hpp"
#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/recovery.hpp"
#include "geoworld/persistence/wal.hpp"
#include "geoworld/runtime/world_runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include <unistd.h>
#include <sys/wait.h>

namespace {

using namespace geoworld::persistence;
using geoworld::foundation::WorldId;

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

Result<std::uint64_t> recover_once(const DurableLayout& layout, WorldId world,
                                   BranchId branch) {
    Result<std::uint64_t> result;
    RecoveryPlanner planner{layout, world, branch};
    auto plan = planner.build(TailPolicy::strict);
    if (!plan.ok()) return {{}, plan.error};
    geoworld::runtime::WorldRuntime runtime;
    CheckpointRegistry registry;
    register_providers(registry, runtime);
    if (!plan.value.checkpoint.has_value()) return {{}, PersistenceError::not_found};
    CheckpointLoader loader{layout, world, branch};
    const auto restored = loader.restore_into(registry, *plan.value.checkpoint);
    if (restored != PersistenceError::none) return {{}, restored};

    std::uint64_t replay_until{};
    std::map<std::uint64_t, std::uint64_t> hashes;
    for (const auto& record : plan.value.replay_records) {
        if (record.kind == WalRecordKind::external_command) {
            auto command = geoworld::gateway::decode_external_command_record(record.payload);
            if (!command.has_value()) return {{}, PersistenceError::record_invalid};
            geoworld::simulation::CommandMeta meta;
            meta.durable_lsn = record.lsn.value;
            meta.expected_object_version = command->expected_object_version;
            if (runtime.submit(command->target_tick, std::move(command->payload), meta) == 0)
                return {{}, PersistenceError::record_invalid};
            replay_until = std::max(replay_until, command->target_tick);
        } else if (record.kind == WalRecordKind::state_hash) {
            const auto point = decode_state_hash_point(record.payload);
            if (!point.has_value()) return {{}, PersistenceError::record_invalid};
            hashes.emplace(point->tick, point->hash);
            replay_until = std::max(replay_until, point->tick);
        }
    }
    while (static_cast<std::uint64_t>(runtime.clock().tick()) <= replay_until) {
        const auto step = runtime.step();
        const auto expected = hashes.find(step.tick);
        if (expected != hashes.end() && expected->second != step.state_hash)
            return {{}, PersistenceError::checkpoint_invalid};
    }
    result.value = geoworld::debug::world_state_hash(runtime.world());
    return result;
}

[[nodiscard]] bool transfer_byte(int descriptor, char& value, bool write_value) {
    for (;;) {
        const ssize_t transferred = write_value ? ::write(descriptor, &value, 1)
                                                : ::read(descriptor, &value, 1);
        if (transferred == 1) return true;
        if (transferred < 0 && errno == EINTR) continue;
        return false;
    }
}

[[nodiscard]] bool sigkill_after_durable_acceptance() {
    const auto root = std::filesystem::temp_directory_path()
                      / ("gw-m5-sigkill-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const WorldId world_id{16};
    const auto branch = parse_branch_id("00000000-0000-0000-0000-000000000016").value();
    WalConfig config;
    config.durable_root = root;
    config.world = world_id;
    config.branch = branch;

    geoworld::gateway::ExternalCommand command;
    command.client_sequence = 77;
    command.target_wid = WorldId{1};
    command.params = geoworld::gateway::SetPropertyParams{"strength", 25.0};
    geoworld::gateway::DurableRequestId request_id{};
    request_id.back() = 16;
    const auto payload = geoworld::gateway::encode_external_command_record(
        "sigkill-test", request_id, 1, command);

    int ready[2]{};
    if (::pipe(ready) != 0) return false;
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(ready[0]);
        WalWriter writer{config};
        char marker = 'E';
        if (writer.start().ok()) {
            WalRecord record;
            record.kind = WalRecordKind::external_command;
            record.target_tick = 1;
            record.payload = payload;
            auto ticket = writer.append(std::move(record));
            if (ticket.ok() && ticket.value.wait().ok()) marker = 'D';
        }
        static_cast<void>(transfer_byte(ready[1], marker, true));
        if (marker == 'D') {
            for (;;) ::pause();
        }
        ::_exit(2);
    }
    ::close(ready[1]);
    if (child < 0) {
        ::close(ready[0]);
        return false;
    }
    char marker{};
    const bool durable = transfer_byte(ready[0], marker, false) && marker == 'D';
    ::close(ready[0]);
    if (!durable || ::kill(child, SIGKILL) != 0) {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, nullptr, 0));
        return false;
    }
    int status{};
    if (::waitpid(child, &status, 0) != child || !WIFSIGNALED(status)
        || WTERMSIG(status) != SIGKILL) return false;

    const auto layout = make_durable_layout(root, world_id, branch);
    const auto ops = make_posix_file_ops();
    const auto after_kill = scan_wal_directory(layout.wal_dir(), *ops, TailPolicy::strict);
    if (!after_kill.ok() || after_kill.records.size() != 1
        || after_kill.records.front().kind != WalRecordKind::external_command
        || after_kill.records.front().lsn != Lsn{1}
        || after_kill.records.front().payload != payload) return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        geoworld::runtime::WorldRuntime runtime;
        geoworld::world::WorldObject object;
        object.id = WorldId{1};
        object.semantic_type = "sigkill.entity";
        object.lifecycle = geoworld::world::LifecycleState::active;
        object.properties.emplace("strength", 10.0);
        if (runtime.submit(0, geoworld::simulation::CreateObjectCommand{object}) == 0)
            return false;
        static_cast<void>(runtime.step());
        auto recovered = geoworld::gateway::decode_external_command_record(payload);
        if (!recovered.has_value()) return false;
        geoworld::simulation::CommandMeta meta;
        meta.ingress_sequence = 1;
        meta.durable_lsn = 1;
        if (runtime.submit(recovered->target_tick, std::move(recovered->payload), meta) == 0)
            return false;
        const auto step = runtime.step();
        const auto* restored = runtime.world().find(WorldId{1});
        if (step.commands.applied != 1 || step.commands.outcomes.size() != 1
            || restored == nullptr || restored->version != 2
            || std::get<double>(restored->properties.at("strength")) != 25.0) return false;
    }

    WalWriter restarted{config};
    if (!restarted.start().ok()) return false;
    WalRecord outcome;
    outcome.kind = WalRecordKind::command_outcome;
    outcome.payload = geoworld::gateway::encode_command_outcome_record(
        "sigkill-test", request_id, command.client_sequence, 1, true,
        geoworld::gateway::GatewayError::none);
    auto outcome_ticket = restarted.append(std::move(outcome));
    if (!outcome_ticket.ok()) return false;
    const auto committed = outcome_ticket.value.wait();
    restarted.shutdown();
    if (!committed.ok() || committed.lsn != Lsn{2}) return false;

    geoworld::gateway::DurableIdempotencyIndex index;
    const auto complete = scan_wal_directory(layout.wal_dir(), *ops, TailPolicy::strict);
    for (const auto& record : complete.records) {
        if (!index.restore(static_cast<geoworld::gateway::DurableRecordKind>(record.kind),
                           record.lsn.value, record.payload)) return false;
    }
    const auto lookup = index.lookup({"sigkill-test", request_id},
                                     geoworld::gateway::make_command_fingerprint(command));
    std::filesystem::remove_all(root, ec);
    return complete.ok() && complete.records.size() == 2 && lookup.entry != nullptr
           && lookup.content_match
           && lookup.entry->state == geoworld::gateway::DurableEntryState::applied;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path()
                      / ("gw-m5-recovery-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const WorldId world_id{15};
    const auto branch = parse_branch_id("00000000-0000-0000-0000-000000000015").value();
    const auto layout = make_durable_layout(root, world_id, branch);

    geoworld::runtime::WorldRuntime baseline;
    CheckpointRegistry registry;
    register_providers(registry, baseline);
    CheckpointConfig checkpoint_config;
    checkpoint_config.layout = layout;
    checkpoint_config.world = world_id;
    checkpoint_config.branch = branch;
    checkpoint_config.authoritative_modules =
        geoworld::runtime::WorldRuntime::authoritative_state_modules();
    CheckpointCoordinator coordinator{checkpoint_config};
    bool checkpoint_published = false;
    baseline.add_checkpoint_anchor_callback(
        [&](std::uint64_t tick, std::uint64_t state_hash) {
            if (tick != 0) return;
            auto captured = coordinator.capture(registry, {tick, tick + 1, Lsn{}, state_hash});
            checkpoint_published = captured.ok()
                && coordinator.publish(registry, std::move(captured.value)).ok();
        });
    geoworld::world::WorldObject object;
    object.id = WorldId{1};
    object.semantic_type = "recovery.entity";
    object.lifecycle = geoworld::world::LifecycleState::active;
    object.properties.emplace("strength", 10.0);
    static_cast<void>(baseline.submit(0, geoworld::simulation::CreateObjectCommand{object}));
    const auto initial = baseline.step();
    if (!checkpoint_published || initial.tick != 0) return 1;

    WalConfig wal_config;
    wal_config.durable_root = root;
    wal_config.world = world_id;
    wal_config.branch = branch;
    WalWriter writer{wal_config};
    if (!writer.start().ok()) return 2;

    geoworld::gateway::ExternalCommand command;
    command.client_sequence = 1;
    command.target_wid = WorldId{1};
    command.params = geoworld::gateway::SetPropertyParams{"strength", 25.0};
    geoworld::gateway::DurableRequestId request_id{};
    request_id.back() = 1;
    WalRecord external;
    external.kind = WalRecordKind::external_command;
    external.target_tick = 1;
    external.payload = geoworld::gateway::encode_external_command_record(
        "recovery-test", request_id, 1, command);
    auto external_ticket = writer.append(std::move(external));
    if (!external_ticket.ok()) return 2;
    const auto external_outcome = external_ticket.value.wait();
    if (!external_outcome.ok()) return 2;

    geoworld::simulation::CommandMeta meta;
    meta.durable_lsn = external_outcome.lsn.value;
    static_cast<void>(baseline.submit(
        1, geoworld::simulation::SetPropertyCommand{WorldId{1}, "strength", 25.0}, meta));
    const auto changed = baseline.step();
    WalRecord hash;
    hash.kind = WalRecordKind::state_hash;
    hash.target_tick = changed.tick;
    hash.payload = encode_state_hash_point({changed.tick, changed.state_hash});
    auto hash_ticket = writer.append(std::move(hash));
    if (!hash_ticket.ok() || !hash_ticket.value.wait().ok()) return 2;
    writer.shutdown();

    const std::uint64_t expected = geoworld::debug::world_state_hash(baseline.world());
    const auto first = recover_once(layout, world_id, branch);
    const auto second = recover_once(layout, world_id, branch);
    std::filesystem::remove_all(root, ec);
    if (!first.ok() || !second.ok() || first.value != expected || second.value != expected
        || first.value != second.value) {
        std::cerr << "expected=" << expected << " first=" << first.value
                  << " first_error=" << error_code(first.error)
                  << " second=" << second.value
                  << " second_error=" << error_code(second.error) << '\n';
        return 3;
    }
    return sigkill_after_durable_acceptance() ? 0 : 4;
}
