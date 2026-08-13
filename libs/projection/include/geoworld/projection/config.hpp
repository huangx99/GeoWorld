#pragma once

#include "geoworld/spatial/cell_grid.hpp"
#include "geoworld/spatial/coordinates.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace geoworld::projection {

// M4-A 具名配置基线，默认值冻结于 docs/M4.md 配置基线表。
// 非法配置必须使启动失败，不允许静默回退到固定值。
struct ProjectionConfig {
    std::uint32_t data_frequency_hz{20};
    std::uint32_t slow_frequency_hz{2};
    std::uint32_t keyframe_interval_seconds{5};
    std::size_t snapshot_history_frames{256};
    std::size_t max_unacked_frames{256};
    std::size_t max_unacked_bytes{16U * 1024U * 1024U};
    std::uint32_t max_feature_ids_per_epoch{1'000'000};
    std::int64_t tick_dt_microseconds{20'000};
    // 单帧单连接 update 数上限；带宽饱和时按 (优先级, 等待时间) 调度，防止低优先级饥饿。
    std::size_t max_updates_per_frame{std::numeric_limits<std::size_t>::max()};
    spatial::CellGrid cell_grid{};
    // ENU 局部系原点，AOI 计算前将 ECEF 转换到该原点；{0,0,0} 视为未配置。
    spatial::Ecef enu_origin{};
};

[[nodiscard]] bool validate(const ProjectionConfig& config, std::string& diagnostic);

} // namespace geoworld::projection
