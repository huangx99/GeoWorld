#pragma once

#include "geoworld/projection/entity.hpp"

#include <cstdint>
#include <variant>
#include <vector>

namespace geoworld::projection {

// 离开原因只用于诊断，不改变客户端删除语义。
enum class LeaveReason { out_of_area, not_relevant, destroyed, policy_changed };

// update 携带该实体完整的最新投影，不做字段级 patch。
struct EntityEnter {
    foundation::WorldId wid;
    foundation::FeatureId fid;
    ProjectedEntity entity;
};

struct EntityUpdate {
    foundation::WorldId wid;
    foundation::FeatureId fid;
    ProjectedEntity entity;
};

struct EntityLeave {
    foundation::WorldId wid;
    foundation::FeatureId fid;
    LeaveReason reason{LeaveReason::out_of_area};
};

// Keyframe：当前连接完整可见集及完整 FID 映射，baseline_snapshot_id = 0。
struct Keyframe {
    std::uint64_t stream_epoch{};
    std::uint64_t stream_sequence{};
    std::uint64_t snapshot_id{};
    std::uint64_t baseline_snapshot_id{};
    std::uint64_t tick{};
    std::uint64_t simulation_time_us{};
    std::vector<EntityEnter> entities;
};

// Delta：leave 按 FID 升序、enter 按 WID 升序、update 按 FID 升序输出。
struct Delta {
    std::uint64_t stream_epoch{};
    std::uint64_t stream_sequence{};
    std::uint64_t snapshot_id{};
    std::uint64_t baseline_snapshot_id{};
    std::uint64_t tick{};
    std::uint64_t simulation_time_us{};
    std::vector<EntityEnter> enters;
    std::vector<EntityUpdate> updates;
    std::vector<EntityLeave> leaves;
};

using StateFrame = std::variant<Keyframe, Delta>;

enum class AckResult {
    accepted,
    duplicate_ignored,
    error_unknown_snapshot,
    error_epoch_mismatch,
};

} // namespace geoworld::projection
