#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/world/world.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geoworld::projection {

enum class FrequencyClass { on_change, slow, fast };

struct ProjectionMetadata {
    FrequencyClass frequency{FrequencyClass::on_change};
    std::uint32_t priority{};
    std::vector<std::string> visibility_tags;
};

// 客户端可见的规范化实体视图。fid 由连接投影状态在发送 enter 前赋予，
// 规范化与稳定 hash 不依赖 fid（fid 是每连接 epoch 内的局部映射）。
struct ProjectedEntity {
    foundation::WorldId wid;
    foundation::FeatureId fid{};
    std::uint64_t version{};
    world::PositionEcef position;
    std::string semantic_type;
    std::string geometry_ref;
    world::LifecycleState lifecycle{world::LifecycleState::staged};
    world::PropertyBag properties;
    world::PropertyBag state;
    std::vector<world::Relation> relations;
    std::vector<std::string> capabilities;
    ProjectionMetadata metadata;
};

} // namespace geoworld::projection
