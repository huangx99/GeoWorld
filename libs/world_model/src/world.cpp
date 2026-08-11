#include "geoworld/world/world.hpp"
#include "geoworld/world/schema.hpp"

#include <algorithm>

namespace geoworld::world {

void register_world_schemas(schema::SchemaRegistry& registry) {
    static_cast<void>(registry.register_schema({
        "world.object", 1, schema::SchemaKind::object_type,
        {{"geometry_ref", schema::FieldType::string},
         {"semantic_type", schema::FieldType::string}}
    }));
    static_cast<void>(registry.register_schema({
        "world.relation", 1, schema::SchemaKind::relation,
        {{"type", schema::FieldType::string}}
    }));
}

bool World::insert(WorldObject object) {
    if (!object.id.valid() || object.version == 0) {
        return false;
    }
    return objects_.emplace(object.id, std::move(object)).second;
}

bool World::erase(WorldId id) { return objects_.erase(id) != 0; }

WorldObject* World::find(WorldId id) noexcept {
    const auto iterator = objects_.find(id);
    return iterator == objects_.end() ? nullptr : &iterator->second;
}

const WorldObject* World::find(WorldId id) const noexcept {
    const auto iterator = objects_.find(id);
    return iterator == objects_.end() ? nullptr : &iterator->second;
}

bool World::set_property(WorldId id, std::string key, PropertyValue value) {
    auto* object = find(id);
    if (object == nullptr || object->lifecycle == LifecycleState::retired) {
        return false;
    }
    object->properties.insert_or_assign(std::move(key), std::move(value));
    ++object->version;
    return true;
}

bool World::add_relation(WorldId source, Relation relation) {
    auto* object = find(source);
    if (object == nullptr || find(relation.target) == nullptr || relation.type.empty()) {
        return false;
    }
    object->relations.push_back(std::move(relation));
    ++object->version;
    return true;
}

std::vector<WorldObject> World::snapshot() const {
    std::vector<WorldObject> result;
    result.reserve(objects_.size());
    for (const auto& [id, object] : objects_) {
        static_cast<void>(id);
        result.push_back(object);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return result;
}

std::size_t World::size() const noexcept { return objects_.size(); }

} // namespace geoworld::world
