#include "geoworld/persistence/checkpoint.hpp"

#include "geoworld/persistence/branch.hpp"
#include "geoworld/persistence/wal.hpp"

#include "checkpoint_codec.hpp"

#include <crc32c/crc32c.h>
#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

namespace geoworld::persistence {

namespace {

// 世代后缀避免同 tick 重试覆盖旧数据；manifest 最后发布，始终是提交点。
inline constexpr std::string_view kCheckpointDataExtension = ".gwck";
inline constexpr std::string_view kCheckpointManifestExtension = ".gwckm";

[[nodiscard]] std::string checkpoint_file_stem(std::uint64_t completed_tick,
                                                std::string_view generation = {}) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "ckpt-%020llu",
                  static_cast<unsigned long long>(completed_tick));
    std::string result{buffer};
    if (!generation.empty()) {
        result.push_back('-');
        result.append(generation);
    }
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> parse_checkpoint_tick(std::string_view name,
                                                                 std::string_view extension) {
    constexpr std::string_view prefix = "ckpt-";
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

// 回收被已发布检查点覆盖的已关闭 WAL segment：segment 末条记录 LSN 由
// 下一段（关闭段或活跃段）的首 LSN 推得；无法确定末端的最后一段保守保留。
[[nodiscard]] std::size_t prune_covered_segments(const std::filesystem::path& wal_dir,
                                                 FileOps& ops, Lsn included_lsn) {
    if (!included_lsn.valid()) {
        return 0;
    }
    auto listed = ops.list_files(wal_dir);
    if (!listed.ok()) {
        return 0;
    }
    std::vector<std::pair<Lsn, std::filesystem::path>> closed;
    std::optional<Lsn> active_first;
    for (const std::filesystem::path& file : listed.value) {
        const std::string name = file.filename().string();
        if (name.size() >= kActiveSegmentSuffix.size()
            && name.compare(name.size() - kActiveSegmentSuffix.size(),
                            kActiveSegmentSuffix.size(), kActiveSegmentSuffix)
                   == 0) {
            active_first = parse_segment_file_name(std::string_view{name}.substr(
                0, name.size() - kActiveSegmentSuffix.size()));
            continue;
        }
        if (name.size() >= kSegmentExtension.size()
            && name.compare(name.size() - kSegmentExtension.size(), kSegmentExtension.size(),
                            kSegmentExtension)
                   == 0) {
            const std::optional<Lsn> first = parse_segment_file_name(std::string_view{name}.substr(
                0, name.size() - kSegmentExtension.size()));
            if (first.has_value()) {
                closed.emplace_back(*first, file);
            }
        }
    }
    std::sort(closed.begin(), closed.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    std::size_t removed = 0;
    for (std::size_t index = 0; index < closed.size(); ++index) {
        std::optional<Lsn> next_first;
        if (index + 1 < closed.size()) {
            next_first = closed[index + 1].first;
        } else if (active_first.has_value()) {
            next_first = active_first;
        }
        if (!next_first.has_value()) {
            continue;
        }
        // 该段末条记录 LSN = 下一段首 LSN - 1；被 included_lsn 覆盖才可删除。
        if (next_first->value > 0 && next_first->value - 1 <= included_lsn.value) {
            if (ops.remove_file(closed[index].second) == PersistenceError::none) {
                ++removed;
            }
        }
    }
    return removed;
}

[[nodiscard]] Result<std::set<std::filesystem::path>> pinned_checkpoints(
    const DurableLayout& layout, const std::shared_ptr<FileOps>& ops) {
    Result<std::set<std::filesystem::path>> result;
    auto branches = ops->list_directories(layout.root.parent_path());
    if (!branches.ok()) {
        result.error = branches.error;
        return result;
    }
    for (const auto& branch_root : branches.value) {
        const std::string name = branch_root.filename().string();
        if (!name.starts_with("branch-")) {
            continue;
        }
        auto manifest = load_branch_manifest(
            branch_manifest_path(DurableLayout{branch_root}), ops);
        if (manifest.ok()) {
            result.value.insert(manifest.value.base_checkpoint.lexically_normal());
        } else if (manifest.error != PersistenceError::not_found) {
            result.error = manifest.error;
            return result;
        }
    }
    return result;
}

} // namespace

PersistenceError CheckpointRegistry::register_provider(
    std::shared_ptr<CheckpointProvider> provider) {
    if (provider == nullptr || provider->schema().provider_id.empty()
        || provider->schema().schema_version == 0) {
        return PersistenceError::record_invalid;
    }
    if (find(provider->schema().provider_id) != nullptr) {
        return PersistenceError::config_invalid;
    }
    providers_.push_back(std::move(provider));
    return PersistenceError::none;
}

std::vector<std::shared_ptr<CheckpointProvider>> CheckpointRegistry::providers_by_id() const {
    std::vector<std::shared_ptr<CheckpointProvider>> sorted = providers_;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left->schema().provider_id < right->schema().provider_id;
    });
    return sorted;
}

Result<std::vector<std::shared_ptr<CheckpointProvider>>>
CheckpointRegistry::providers_in_restore_order() const {
    Result<std::vector<std::shared_ptr<CheckpointProvider>>> result;
    std::set<std::string> done;
    std::set<std::string> in_progress;
    const auto sorted = providers_by_id();
    // 确定性拓扑排序：按 ID 顺序访问，依赖先序展开；环或未注册依赖拒绝。
    const auto visit = [&](auto&& self, const std::shared_ptr<CheckpointProvider>& provider)
        -> PersistenceError {
        const std::string id = provider->schema().provider_id;
        if (done.contains(id)) {
            return PersistenceError::none;
        }
        if (!in_progress.insert(id).second) {
            return PersistenceError::checkpoint_invalid;
        }
        for (const std::string& dependency : provider->restore_dependencies()) {
            const auto target = find(dependency);
            if (target == nullptr) {
                return PersistenceError::checkpoint_invalid;
            }
            const PersistenceError error = self(self, target);
            if (error != PersistenceError::none) {
                return error;
            }
        }
        in_progress.erase(id);
        done.insert(id);
        result.value.push_back(provider);
        return PersistenceError::none;
    };
    for (const auto& provider : sorted) {
        const PersistenceError error = visit(visit, provider);
        if (error != PersistenceError::none) {
            return {{}, error};
        }
    }
    return result;
}

std::shared_ptr<CheckpointProvider> CheckpointRegistry::find(
    std::string_view provider_id) const {
    for (const auto& provider : providers_) {
        if (provider->schema().provider_id == provider_id) {
            return provider;
        }
    }
    return nullptr;
}

PersistenceError CheckpointRegistry::validate_completeness(
    const std::vector<std::string_view>& authoritative_modules) const {
    for (const std::string_view module : authoritative_modules) {
        if (find(module) == nullptr) {
            return PersistenceError::provider_missing;
        }
    }
    return PersistenceError::none;
}

CheckpointCoordinator::CheckpointCoordinator(CheckpointConfig config,
                                             std::shared_ptr<FileOps> file_ops)
    : config_(std::move(config)),
      ops_(file_ops ? std::move(file_ops) : make_posix_file_ops()) {}

Result<CapturedCheckpoint> CheckpointCoordinator::capture(
    const CheckpointRegistry& registry, const CheckpointAnchor& anchor) const {
    if (!anchor.world_state_hash || anchor.resume_tick != anchor.completed_tick + 1
        || config_.keep_last_checkpoints == 0 || config_.compression_minimum_bytes == 0
        || (config_.compression_enabled
            && (config_.compression_level < ZSTD_minCLevel()
                || config_.compression_level > ZSTD_maxCLevel()))) {
        return {{}, PersistenceError::record_invalid};
    }
    if (!config_.authoritative_modules.empty()) {
        const PersistenceError complete =
            registry.validate_completeness(config_.authoritative_modules);
        if (complete != PersistenceError::none) {
            return {{}, complete};
        }
    }
    Result<CapturedCheckpoint> result;
    result.value.anchor = anchor;
    for (const auto& provider : registry.providers_by_id()) {
        result.value.schemas.push_back(provider->schema());
        FrozenProviderState frozen = provider->freeze();
        if (frozen.included_lsn.has_value()) {
            result.value.anchor.included_lsn = *frozen.included_lsn;
        }
        result.value.frozen.push_back(std::move(frozen));
    }
    return result;
}

Result<PublishedCheckpoint> CheckpointCoordinator::publish(
    const CheckpointRegistry& registry, CapturedCheckpoint captured) {
    PersistenceError error = ops_->create_directories(config_.layout.checkpoint_dir());
    if (error != PersistenceError::none) {
        return {{}, error};
    }
    // 第 3 步：后台确定性编码，块按稳定 provider ID 排序（capture 已保证顺序）。
    std::vector<CheckpointBlock> blocks;
    blocks.reserve(captured.frozen.size());
    for (std::size_t index = 0; index < captured.frozen.size(); ++index) {
        const auto provider = registry.find(captured.schemas[index].provider_id);
        if (provider == nullptr) {
            return {{}, PersistenceError::provider_missing};
        }
        CheckpointBlock block = provider->encode(captured.frozen[index]);
        if (block.schema != captured.schemas[index]) {
            return {{}, PersistenceError::checkpoint_invalid};
        }
        blocks.push_back(std::move(block));
    }
    const std::uint64_t checkpoint_content_hash =
        detail::authoritative_checkpoint_hash(blocks);
    const std::vector<std::byte> uncompressed_data = detail::encode_world_checkpoint(
        config_.world, config_.branch, captured.anchor, checkpoint_content_hash, blocks);
    std::vector<std::byte> data = uncompressed_data;
    bool compressed = false;
    if (config_.compression_enabled
        && uncompressed_data.size() >= config_.compression_minimum_bytes
        && config_.compression_level >= ZSTD_minCLevel()
        && config_.compression_level <= ZSTD_maxCLevel()) {
        std::vector<std::byte> candidate(ZSTD_compressBound(uncompressed_data.size()));
        const std::size_t size = ZSTD_compress(
            candidate.data(), candidate.size(), uncompressed_data.data(),
            uncompressed_data.size(), config_.compression_level);
        if (!ZSTD_isError(size) && size < uncompressed_data.size()) {
            candidate.resize(size);
            data = std::move(candidate);
            compressed = true;
        }
    }
    const std::uint32_t data_crc =
        crc32c::Crc32c(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    std::string generation = format_branch_id(generate_branch_id());
    generation.erase(std::remove(generation.begin(), generation.end(), '-'), generation.end());
    const std::string stem = checkpoint_file_stem(captured.anchor.completed_tick, generation);
    const std::string data_name = stem + std::string{kCheckpointDataExtension}
                                  + (compressed ? ".zst" : "");
    const std::filesystem::path data_path = config_.layout.checkpoint_dir() / data_name;
    const std::filesystem::path manifest_path =
        config_.layout.checkpoint_dir()
        / (stem + std::string{kCheckpointManifestExtension});

    // 第 4-6 步：数据先原子发布，manifest 最后原子发布（提交点）。
    error = atomic_publish(*ops_, data_path, data);
    if (error != PersistenceError::none) {
        return {{}, error};
    }
    std::vector<CheckpointSchema> schemas = captured.schemas;
    const std::vector<std::byte> manifest_bytes = detail::encode_checkpoint_manifest(
        config_.world, config_.branch, captured.anchor, checkpoint_content_hash, schemas,
        data_name, data.size(), data_crc, uncompressed_data.size(), compressed);
    error = atomic_publish(*ops_, manifest_path, manifest_bytes);
    if (error != PersistenceError::none) {
        // manifest 未提交：数据文件无提交点引用，保留上一份完整检查点。
        ops_->remove_file(data_path);
        return {{}, error};
    }

    // 第 7 步：重新打开校验长度、checksum、schema 与状态 hash 后才标记可恢复。
    const auto verify = detail::verify_checkpoint_pair(*ops_, config_.layout.checkpoint_dir(),
                                                       manifest_path, config_.world,
                                                       config_.branch);
    if (!verify.ok()) {
        return {{}, verify.error};
    }

    // 第 8 步：发布成功后才按保留策略回收旧检查点与被覆盖的旧 WAL segment。
    auto listed = ops_->list_files(config_.layout.checkpoint_dir());
    if (listed.ok()) {
        std::vector<std::pair<std::uint64_t, std::filesystem::path>> manifests;
        for (const std::filesystem::path& file : listed.value) {
            const std::string name = file.filename().string();
            if (const auto tick = parse_checkpoint_tick(name, kCheckpointManifestExtension)) {
                manifests.emplace_back(*tick, file);
            }
        }
        std::sort(manifests.begin(), manifests.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });
        const auto pinned = pinned_checkpoints(config_.layout, ops_);
        while (manifests.size() > config_.keep_last_checkpoints) {
            if (!pinned.ok()) {
                break;
            }
            const auto removable = std::find_if(
                manifests.begin(), manifests.end(), [&](const auto& candidate) {
                    return candidate.second.lexically_normal() != manifest_path.lexically_normal()
                           && !pinned.value.contains(candidate.second.lexically_normal());
                });
            if (removable == manifests.end()) {
                break;
            }
            const auto& [tick, oldest] = *removable;
            static_cast<void>(tick);
            auto verified = detail::verify_checkpoint_pair(
                *ops_, config_.layout.checkpoint_dir(), oldest, config_.world, config_.branch);
            if (verified.ok()) {
                ops_->remove_file(config_.layout.checkpoint_dir()
                                  / verified.value.manifest.data_file);
            }
            ops_->remove_file(oldest);
            manifests.erase(removable);
        }
    }
    if (config_.prune_wal_after_publish) {
        static_cast<void>(prune_covered_segments(config_.layout.wal_dir(), *ops_,
                                                 captured.anchor.included_lsn));
    }
    return {PublishedCheckpoint{captured.anchor, checkpoint_content_hash, manifest_path,
                                data.size()},
            PersistenceError::none};
}

} // namespace geoworld::persistence
