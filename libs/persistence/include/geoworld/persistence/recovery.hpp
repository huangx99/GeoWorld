#pragma once

#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/types.hpp"
#include "geoworld/persistence/wal.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace geoworld::persistence {

// 检查点目录中的已发布文件对：数据块为提交内容，manifest 为原子提交点。
struct CheckpointInfo {
    CheckpointAnchor anchor{};
    std::uint64_t checkpoint_content_hash{};
    std::filesystem::path manifest_path;
    std::filesystem::path data_path;
    std::uint64_t data_length{};
};

// 完整校验通过、已载入内存的检查点。
struct LoadedCheckpoint {
    CheckpointInfo info;
    std::vector<CheckpointBlock> blocks;   // 按稳定 provider ID 排序，payload CRC 已校验
};

// eager 恢复加载器：选择最新且完整验证通过的检查点，按依赖顺序恢复 provider。
class CheckpointLoader {
public:
    CheckpointLoader(DurableLayout layout, geoworld::foundation::WorldId world,
                     BranchId branch, std::shared_ptr<FileOps> file_ops = {});

    // 目录 manifest 之外列出全部已验证检查点；忽略 tmp- 残留与校验失败项。
    [[nodiscard]] Result<std::vector<CheckpointInfo>> list_valid() const;
    [[nodiscard]] Result<CheckpointInfo> latest_valid() const;
    // 重新打开并校验：manifest/数据长度/CRC32C、格式版本、块 CRC、schema 与锚点一致性。
    [[nodiscard]] Result<LoadedCheckpoint> load(const CheckpointInfo& info) const;
    // eager restore：未知 provider、版本不匹配、已注册 provider 缺块均拒绝。
    [[nodiscard]] PersistenceError restore_into(const CheckpointRegistry& registry,
                                                const LoadedCheckpoint& checkpoint) const;

private:
    DurableLayout layout_;
    geoworld::foundation::WorldId world_{};
    BranchId branch_{};
    std::shared_ptr<FileOps> ops_;
};

struct RecoveryPlan {
    std::optional<LoadedCheckpoint> checkpoint;
    std::vector<ScannedRecord> replay_records;
    Lsn last_lsn{};
};

class RecoveryPlanner {
public:
    RecoveryPlanner(DurableLayout layout, geoworld::foundation::WorldId world,
                    BranchId branch, std::shared_ptr<FileOps> file_ops = {});

    // 自动回退到最新的完整检查点；WAL 全程连续校验，只返回检查点水位之后的记录。
    [[nodiscard]] Result<RecoveryPlan> build(TailPolicy tail_policy = TailPolicy::trim_active_tail) const;

private:
    DurableLayout layout_;
    geoworld::foundation::WorldId world_{};
    BranchId branch_{};
    std::shared_ptr<FileOps> ops_;
};

struct StateHashPoint {
    std::uint64_t tick{};
    std::uint64_t hash{};
};

[[nodiscard]] std::vector<std::byte> encode_state_hash_point(StateHashPoint point);
[[nodiscard]] std::optional<StateHashPoint> decode_state_hash_point(
    std::span<const std::byte> payload) noexcept;

} // namespace geoworld::persistence
