#pragma once

#include "geoworld/projection/entity.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>

namespace geoworld::projection {

// 字段白名单策略：默认不暴露任何属性，只有显式允许的键进入投影。
// 每次修改都会递增 policy_version，连接侧版本不一致时下一帧必须是 keyframe。
class ProjectionPolicy {
public:
    using FrequencyResolver = std::function<FrequencyClass(const world::WorldObject&)>;
    using PriorityResolver = std::function<std::uint32_t(const world::WorldObject&)>;

    void allow_property(std::string key);
    void allow_state_key(std::string key);
    void allow_relation_type(std::string type);
    void allow_capability(std::string capability);
    void set_frequency_resolver(FrequencyResolver resolver);
    void set_priority_resolver(PriorityResolver resolver);

    [[nodiscard]] std::uint64_t version() const noexcept;
    [[nodiscard]] ProjectedEntity project(const world::WorldObject& object) const;

private:
    std::set<std::string> allowed_properties_;
    std::set<std::string> allowed_state_keys_;
    std::set<std::string> allowed_relation_types_;
    std::set<std::string> allowed_capabilities_;
    FrequencyResolver frequency_resolver_;
    PriorityResolver priority_resolver_;
    std::uint64_t version_{1};
};

} // namespace geoworld::projection
