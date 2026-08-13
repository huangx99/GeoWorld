#include "geoworld/projection/policy.hpp"

#include <algorithm>

namespace geoworld::projection {

void ProjectionPolicy::allow_property(std::string key) {
    allowed_properties_.insert(std::move(key));
    ++version_;
}

void ProjectionPolicy::allow_state_key(std::string key) {
    allowed_state_keys_.insert(std::move(key));
    ++version_;
}

void ProjectionPolicy::allow_relation_type(std::string type) {
    allowed_relation_types_.insert(std::move(type));
    ++version_;
}

void ProjectionPolicy::allow_capability(std::string capability) {
    allowed_capabilities_.insert(std::move(capability));
    ++version_;
}

void ProjectionPolicy::set_frequency_resolver(FrequencyResolver resolver) {
    frequency_resolver_ = std::move(resolver);
    ++version_;
}

void ProjectionPolicy::set_priority_resolver(PriorityResolver resolver) {
    priority_resolver_ = std::move(resolver);
    ++version_;
}

[[nodiscard]] std::uint64_t ProjectionPolicy::version() const noexcept {
    return version_;
}

[[nodiscard]] ProjectedEntity ProjectionPolicy::project(
    const world::WorldObject& object) const {
    ProjectedEntity projected;
    projected.wid = object.id;
    projected.version = object.version;
    projected.position = object.position;
    projected.semantic_type = object.semantic_type;
    projected.geometry_ref = object.geometry_ref;
    projected.lifecycle = object.lifecycle;

    for (const auto& [key, value] : object.properties) {
        if (allowed_properties_.contains(key)) {
            projected.properties.emplace(key, value);
        }
    }
    for (const auto& [key, value] : object.state) {
        if (allowed_state_keys_.contains(key)) {
            projected.state.emplace(key, value);
        }
    }

    // 关系按 (type, target WID) 排序，能力去重后排序，保证规范化输出稳定。
    for (const auto& relation : object.relations) {
        if (allowed_relation_types_.contains(relation.type)) {
            projected.relations.push_back(relation);
        }
    }
    std::sort(projected.relations.begin(), projected.relations.end(),
        [](const world::Relation& lhs, const world::Relation& rhs) {
            if (lhs.type != rhs.type) {
                return lhs.type < rhs.type;
            }
            return lhs.target < rhs.target;
        });

    for (const auto& capability : object.capabilities) {
        if (allowed_capabilities_.contains(capability)) {
            projected.capabilities.push_back(capability);
        }
    }
    std::sort(projected.capabilities.begin(), projected.capabilities.end());
    projected.capabilities.erase(
        std::unique(projected.capabilities.begin(), projected.capabilities.end()),
        projected.capabilities.end());

    projected.metadata.frequency = frequency_resolver_
        ? frequency_resolver_(object) : FrequencyClass::on_change;
    projected.metadata.priority = priority_resolver_ ? priority_resolver_(object) : 0U;
    return projected;
}

} // namespace geoworld::projection
