#include "geoworld/persistence/recovery.hpp"

#include "geoworld/persistence/branch.hpp"
#include "geoworld/persistence/wal.hpp"

#include "checkpoint_codec.hpp"
#include "le_codec.hpp"

#include <algorithm>
#include <map>

namespace geoworld::persistence {

namespace {

[[nodiscard]] std::optional<std::uint64_t> parse_manifest_tick(std::string_view name) {
    constexpr std::string_view prefix = "ckpt-";
    constexpr std::string_view extension = ".gwckm";
    constexpr std::size_t digits = 20;
    if (name.size() < prefix.size() + digits + extension.size()
        || name.substr(0, prefix.size()) != prefix
        || name.substr(name.size() - extension.size()) != extension) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char ch : name.substr(prefix.size(), digits)) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

} // namespace

CheckpointLoader::CheckpointLoader(DurableLayout layout, geoworld::foundation::WorldId world,
                                   BranchId branch, std::shared_ptr<FileOps> file_ops)
    : layout_(std::move(layout)),
      world_(world),
      branch_(branch),
      ops_(file_ops ? std::move(file_ops) : make_posix_file_ops()) {}

Result<std::vector<CheckpointInfo>> CheckpointLoader::list_valid() const {
    Result<std::vector<CheckpointInfo>> result;
    auto listed = ops_->list_files(layout_.checkpoint_dir());
    if (!listed.ok()) {
        if (listed.error == PersistenceError::not_found) {
            return result;
        }
        return {{}, listed.error};
    }
    for (const std::filesystem::path& file : listed.value) {
        const std::string name = file.filename().string();
        // tmp- 前缀是发布中断残留，永不成恢复候选。
        if (name.starts_with(kTempFilePrefix)) {
            continue;
        }
        if (!parse_manifest_tick(name).has_value()) {
            continue;
        }
        auto verified =
            detail::verify_checkpoint_pair(*ops_, layout_.checkpoint_dir(), file, world_, branch_);
        if (!verified.ok()) {
            continue;
        }
        CheckpointInfo info;
        info.anchor = verified.value.manifest.anchor;
        info.checkpoint_content_hash = verified.value.manifest.checkpoint_content_hash;
        info.manifest_path = file;
        info.data_path = layout_.checkpoint_dir() / verified.value.manifest.data_file;
        info.data_length = verified.value.manifest.data_length;
        result.value.push_back(std::move(info));
    }
    std::sort(result.value.begin(), result.value.end(), [](const CheckpointInfo& left,
                                                           const CheckpointInfo& right) {
        return left.anchor.completed_tick < right.anchor.completed_tick;
    });
    return result;
}

Result<CheckpointInfo> CheckpointLoader::latest_valid() const {
    auto valid = list_valid();
    if (!valid.ok()) {
        return {{}, valid.error};
    }
    if (valid.value.empty()) {
        return {{}, PersistenceError::not_found};
    }
    return {valid.value.back(), PersistenceError::none};
}

Result<LoadedCheckpoint> CheckpointLoader::load(const CheckpointInfo& info) const {
    auto verified = detail::verify_checkpoint_pair(*ops_, layout_.checkpoint_dir(),
                                                   info.manifest_path, world_, branch_);
    if (!verified.ok()) {
        return {{}, verified.error};
    }
    Result<LoadedCheckpoint> result;
    result.value.info = info;
    for (auto& block : verified.value.data.blocks) {
        result.value.blocks.push_back(
            CheckpointBlock{block.schema, std::move(block.payload)});
    }
    return result;
}

PersistenceError CheckpointLoader::restore_into(const CheckpointRegistry& registry,
                                                const LoadedCheckpoint& checkpoint) const {
    auto ordered = registry.providers_in_restore_order();
    if (!ordered.ok()) {
        return ordered.error;
    }
    std::map<std::string, const CheckpointBlock*> blocks_by_id;
    for (const CheckpointBlock& block : checkpoint.blocks) {
        blocks_by_id.emplace(block.schema.provider_id, &block);
    }
    std::map<std::string, CheckpointBlock> migrated;
    // 第一阶段验证全部 provider，任何失败都不会修改目标运行时。
    for (const auto& provider : ordered.value) {
        const auto found = blocks_by_id.find(provider->schema().provider_id);
        if (found == blocks_by_id.end()) {
            return PersistenceError::checkpoint_incomplete;
        }
        const CheckpointBlock* block = found->second;
        if (block->schema.schema_version != provider->schema().schema_version) {
            auto upcasted = provider->upcast(block->payload, block->schema.schema_version);
            if (!upcasted.ok()) return upcasted.error;
            CheckpointBlock current{provider->schema(), std::move(upcasted.value)};
            block = &migrated.emplace(provider->schema().provider_id,
                                      std::move(current)).first->second;
            found->second = block;
        }
        const PersistenceError error =
            provider->validate_restore(block->payload, block->schema.schema_version);
        if (error != PersistenceError::none) {
            return error;
        }
    }
    // 检查点含未注册权威 provider：拒绝而不是跳过。
    if (blocks_by_id.size() != ordered.value.size()) {
        return PersistenceError::provider_unknown;
    }
    for (const auto& provider : ordered.value) {
        const CheckpointBlock& block = *blocks_by_id.at(provider->schema().provider_id);
        const PersistenceError error =
            provider->restore(block.payload, block.schema.schema_version);
        if (error != PersistenceError::none) {
            return error;
        }
    }
    return PersistenceError::none;
}

RecoveryPlanner::RecoveryPlanner(DurableLayout layout, geoworld::foundation::WorldId world,
                                 BranchId branch, std::shared_ptr<FileOps> file_ops)
    : layout_(std::move(layout)), world_(world), branch_(branch),
      ops_(file_ops ? std::move(file_ops) : make_posix_file_ops()) {}

Result<RecoveryPlan> RecoveryPlanner::build(TailPolicy tail_policy) const {
    Result<RecoveryPlan> result;
    CheckpointLoader loader{layout_, world_, branch_, ops_};
    auto latest = loader.latest_valid();
    std::uint64_t included_lsn{};
    if (latest.ok()) {
        auto loaded = loader.load(latest.value);
        if (!loaded.ok()) return {{}, loaded.error};
        included_lsn = loaded.value.info.anchor.included_lsn.value;
        result.value.checkpoint = std::move(loaded.value);
    } else if (latest.error != PersistenceError::not_found) {
        return {{}, latest.error};
    } else {
        auto branch_manifest = load_branch_manifest(branch_manifest_path(layout_), ops_);
        if (branch_manifest.ok()) {
            if (branch_manifest.value.world != world_
                || branch_manifest.value.branch != branch_) {
                return {{}, PersistenceError::manifest_invalid};
            }
            DurableLayout parent_layout{
                branch_manifest.value.base_checkpoint.parent_path().parent_path()};
            CheckpointLoader parent_loader{parent_layout, world_,
                                           branch_manifest.value.parent_branch, ops_};
            auto parent_checkpoints = parent_loader.list_valid();
            if (!parent_checkpoints.ok()) return {{}, parent_checkpoints.error};
            const auto selected = std::find_if(
                parent_checkpoints.value.begin(), parent_checkpoints.value.end(),
                [&](const CheckpointInfo& candidate) {
                    return candidate.manifest_path.lexically_normal()
                               == branch_manifest.value.base_checkpoint.lexically_normal();
                });
            if (selected == parent_checkpoints.value.end()
                || selected->anchor.completed_tick != branch_manifest.value.fork_tick
                || selected->anchor.included_lsn != branch_manifest.value.fork_lsn) {
                return {{}, PersistenceError::checkpoint_invalid};
            }
            auto loaded = parent_loader.load(*selected);
            if (!loaded.ok()) return {{}, loaded.error};
            included_lsn = loaded.value.info.anchor.included_lsn.value;
            result.value.checkpoint = std::move(loaded.value);
        } else if (branch_manifest.error != PersistenceError::not_found) {
            return {{}, branch_manifest.error};
        }
    }

    WalScanResult scan = scan_wal_directory(
        layout_.wal_dir(), *ops_, tail_policy,
        result.value.checkpoint.has_value() ? Lsn{} : kFirstLsn);
    if (!scan.ok()) return {{}, scan.error};
    if (result.value.checkpoint.has_value() && scan.first_lsn.valid()) {
        const auto first_required = included_lsn == 0
            ? std::optional<Lsn>{kFirstLsn}
            : next_lsn(Lsn{included_lsn});
        if (!first_required.has_value() || scan.first_lsn.value > first_required->value) {
            return {{}, PersistenceError::lsn_discontinuity};
        }
    }
    if (scan.last_lsn.valid() && scan.last_lsn.value < included_lsn) {
        return {{}, PersistenceError::lsn_discontinuity};
    }
    for (auto& record : scan.records) {
        if (record.lsn.value > included_lsn) {
            result.value.replay_records.push_back(std::move(record));
        }
    }
    result.value.last_lsn = scan.last_lsn;
    return result;
}

std::vector<std::byte> encode_state_hash_point(StateHashPoint point) {
    std::vector<std::byte> bytes(16);
    detail::write_le64(bytes.data(), point.tick);
    detail::write_le64(bytes.data() + 8, point.hash);
    return bytes;
}

std::optional<StateHashPoint> decode_state_hash_point(
    std::span<const std::byte> payload) noexcept {
    if (payload.size() != 16) return std::nullopt;
    return StateHashPoint{detail::read_le64(payload.data()),
                          detail::read_le64(payload.data() + 8)};
}

} // namespace geoworld::persistence
