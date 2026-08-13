#pragma once

#include "geoworld/projection/entity.hpp"

#include <cstdint>

namespace geoworld::projection {

// 规范化投影的稳定 hash：相同世界状态、策略和订阅必须得到逐字节相同的
// 规范化投影，hash 随之相同。fid 不参与 hash（它是每连接局部映射）。
[[nodiscard]] std::uint64_t projected_entity_hash(const ProjectedEntity& entity) noexcept;

} // namespace geoworld::projection
