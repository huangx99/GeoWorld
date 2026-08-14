#include "geoworld/world/world.hpp"
#include "geoworld/world/schema.hpp"
#include "geoworld/world/snapshot.hpp"

#ifndef GW_WORLD_PERSISTENT_STORAGE
#define GW_WORLD_PERSISTENT_STORAGE 0
#endif

#if GW_WORLD_PERSISTENT_STORAGE
#include <immer/map.hpp>
#endif

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace geoworld::world {

struct ObjectEntry {
    std::shared_ptr<WorldObject> object;
    std::uint64_t cow_generation{};
};

#if GW_WORLD_PERSISTENT_STORAGE
using ObjectStorage = immer::map<WorldId, ObjectEntry, foundation::WorldIdHash>;
#else
using ObjectStorage = std::unordered_map<WorldId, ObjectEntry, foundation::WorldIdHash>;
#endif

const ObjectEntry* find_entry(const ObjectStorage& objects, WorldId id) noexcept {
#if GW_WORLD_PERSISTENT_STORAGE
    return objects.find(id);
#else
    const auto found = objects.find(id);
    return found == objects.end() ? nullptr : &found->second;
#endif
}

void set_entry(ObjectStorage& objects, WorldId id, ObjectEntry entry) {
#if GW_WORLD_PERSISTENT_STORAGE
    objects = objects.set(id, std::move(entry));
#else
    objects.insert_or_assign(id, std::move(entry));
#endif
}

void erase_entry(ObjectStorage& objects, WorldId id) {
#if GW_WORLD_PERSISTENT_STORAGE
    objects = objects.erase(id);
#else
    objects.erase(id);
#endif
}

struct World::Storage {
    ObjectStorage objects;
    std::uint64_t cow_generation{1};
    std::uint64_t storage_revision{};
};

namespace {
struct FrozenStorage {
    ObjectStorage objects;
    std::uint64_t next_revision{};
    std::uint64_t erase_revision{};
};
} // namespace

World::World() : storage_(std::make_unique<Storage>()) {}
World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

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
    object.revision = ++next_revision_;
    if (find_entry(storage_->objects, object.id) != nullptr) {
        return false;
    }
    set_entry(storage_->objects, object.id,
              ObjectEntry{std::make_shared<WorldObject>(std::move(object)),
                          storage_->cow_generation});
    return true;
}

bool World::erase(WorldId id) {
    const bool erased = find_entry(storage_->objects, id) != nullptr;
    if (erased) {
        erase_entry(storage_->objects, id);
        ++erase_revision_;
    }
    return erased;
}

WorldObject* World::writable_find(WorldId id) noexcept {
    const auto* found = find_entry(storage_->objects, id);
    if (found == nullptr) return nullptr;
    if (found->cow_generation == storage_->cow_generation) {
        return found->object.get();
    }
    ObjectEntry writable{std::make_shared<WorldObject>(*found->object),
                         storage_->cow_generation};
    WorldObject* result = writable.object.get();
    set_entry(storage_->objects, id, std::move(writable));
    ++storage_->storage_revision;
    return result;
}

const WorldObject* World::find(WorldId id) const noexcept {
    const auto* found = find_entry(storage_->objects, id);
    return found == nullptr ? nullptr : found->object.get();
}

bool World::update(WorldId id, const std::function<void(WorldObject&)>& mutation) {
    const auto* found = find_entry(storage_->objects, id);
    if (found == nullptr || !mutation) {
        return false;
    }
    WorldObject updated = *found->object;
    mutation(updated);
    set_entry(storage_->objects, id,
              ObjectEntry{std::make_shared<WorldObject>(std::move(updated)),
                          storage_->cow_generation});
    ++storage_->storage_revision;
    return true;
}

bool World::set_property(WorldId id, std::string key, PropertyValue value) {
    auto* object = writable_find(id);
    if (object == nullptr || object->lifecycle == LifecycleState::retired) {
        return false;
    }
    object->properties.insert_or_assign(std::move(key), std::move(value));
    ++object->version;
    return true;
}

bool World::add_relation(WorldId source, Relation relation) {
    if (find(relation.target) == nullptr || relation.type.empty()) {
        return false;
    }
    auto* object = writable_find(source);
    if (object == nullptr) return false;
    object->relations.push_back(std::move(relation));
    ++object->version;
    return true;
}

std::vector<WorldObject> World::snapshot() const {
    std::vector<WorldObject> result;
    result.reserve(storage_->objects.size());
    for (const auto& [id, object] : storage_->objects) {
        static_cast<void>(id);
        result.push_back(*object.object);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return result;
}

std::size_t World::size() const noexcept { return storage_->objects.size(); }

std::uint64_t World::storage_revision() const noexcept {
    return storage_->storage_revision;
}

void World::for_each_object(
    const std::function<void(const WorldObject&)>& callback) const {
    for (const auto& [id, object] : storage_->objects) {
        static_cast<void>(id);
        callback(*object.object);
    }
}

void World::restore(std::vector<WorldObject> objects, std::uint64_t next_revision,
                    std::uint64_t erase_revision) {
    ObjectStorage restored;
    for (auto& object : objects) {
        set_entry(restored, object.id,
                  ObjectEntry{std::make_shared<WorldObject>(std::move(object)),
                              storage_->cow_generation});
    }
    storage_->objects = std::move(restored);
    ++storage_->storage_revision;
    next_revision_ = next_revision;
    erase_revision_ = erase_revision;
}

FrozenWorldSnapshot freeze_snapshot(const World& world) {
#if GW_WORLD_PERSISTENT_STORAGE
    auto snapshot = FrozenWorldSnapshot{std::make_shared<const FrozenStorage>(FrozenStorage{
        world.storage_->objects, world.next_revision_, world.erase_revision_})};
    if (world.storage_->cow_generation == std::numeric_limits<std::uint64_t>::max()) {
        ObjectStorage reset;
        for (const auto& [id, entry] : world.storage_->objects) {
            set_entry(reset, id, ObjectEntry{entry.object, 1});
        }
        world.storage_->objects = std::move(reset);
        world.storage_->cow_generation = 2;
        ++world.storage_->storage_revision;
    } else {
        ++world.storage_->cow_generation;
    }
    return snapshot;
#else
    FrozenStorage frozen;
    frozen.next_revision = world.next_revision_;
    frozen.erase_revision = world.erase_revision_;
    frozen.objects.reserve(world.storage_->objects.size());
    for (const auto& [id, entry] : world.storage_->objects) {
        set_entry(frozen.objects, id,
                  ObjectEntry{std::make_shared<WorldObject>(*entry.object), 1});
    }
    return FrozenWorldSnapshot{std::make_shared<const FrozenStorage>(std::move(frozen))};
#endif
}

std::size_t FrozenWorldSnapshot::size() const noexcept {
    return state_ ? static_cast<const FrozenStorage*>(state_.get())->objects.size() : 0;
}

std::uint64_t FrozenWorldSnapshot::next_revision() const noexcept {
    return state_ ? static_cast<const FrozenStorage*>(state_.get())->next_revision : 0;
}

std::uint64_t FrozenWorldSnapshot::erase_revision() const noexcept {
    return state_ ? static_cast<const FrozenStorage*>(state_.get())->erase_revision : 0;
}

const WorldObject* FrozenWorldSnapshot::find(WorldId id) const noexcept {
    if (!state_) return nullptr;
    const auto* entry = find_entry(static_cast<const FrozenStorage*>(state_.get())->objects, id);
    return entry == nullptr ? nullptr : entry->object.get();
}

void FrozenWorldSnapshot::for_each_object(
    const std::function<void(const WorldObject&)>& callback) const {
    if (!state_) return;
    for (const auto& [id, object] : static_cast<const FrozenStorage*>(state_.get())->objects) {
        static_cast<void>(id);
        callback(*object.object);
    }
}

} // namespace geoworld::world
