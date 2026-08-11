#include "geoworld/debug/world_inspector.hpp"

namespace geoworld::debug {

std::vector<ObjectSummary> WorldInspector::list() const {
    const auto objects = world_.snapshot();
    std::vector<ObjectSummary> summaries;
    summaries.reserve(objects.size());
    for (const auto& object : objects) {
        summaries.push_back(ObjectSummary{
            object.id, object.semantic_type, object.lifecycle, object.version
        });
    }
    return summaries;
}

std::optional<world::WorldObject> WorldInspector::inspect(foundation::WorldId id) const {
    const auto* object = world_.find(id);
    if (object == nullptr) {
        return std::nullopt;
    }
    return *object;
}

} // namespace geoworld::debug
