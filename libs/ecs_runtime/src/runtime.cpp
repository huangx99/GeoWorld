#include "geoworld/ecs/runtime.hpp"

#include <flecs.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace geoworld::ecs {
namespace {

struct WorldIdentityComponent {
    std::uint64_t value;
};

struct PositionComponent {
    double x;
    double y;
    double z;
};

struct SemanticTypeComponent {
    std::string value;
};

struct Slot {
    ecs_entity_t entity{};
    foundation::WorldId world_id;
    std::uint32_t generation{1};
    bool occupied{false};
};

} // namespace

class Runtime::Impl {
public:
    Impl() {
        world.component<WorldIdentityComponent>("ecs.world_identity")
            .member<std::uint64_t>("value");
        world.component<PositionComponent>("ecs.position_ecef")
            .member<double>("x")
            .member<double>("y")
            .member<double>("z");
        world.component<SemanticTypeComponent>("ecs.semantic_type");
    }

    flecs::world world;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_slots;
    std::unordered_map<foundation::WorldId, foundation::RuntimeId,
                       foundation::WorldIdHash> by_world_id;
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

std::optional<foundation::RuntimeId> Runtime::activate(const world::WorldObject& object) {
    if (!object.id.valid() || impl_->by_world_id.contains(object.id)) {
        return std::nullopt;
    }

    std::uint32_t slot_index;
    if (impl_->free_slots.empty()) {
        slot_index = static_cast<std::uint32_t>(impl_->slots.size());
        impl_->slots.emplace_back();
    } else {
        slot_index = impl_->free_slots.back();
        impl_->free_slots.pop_back();
    }

    auto& slot = impl_->slots[slot_index];
    const auto entity = impl_->world.entity()
        .set<WorldIdentityComponent>({object.id.value})
        .set<PositionComponent>({object.position.x, object.position.y, object.position.z})
        .set<SemanticTypeComponent>({object.semantic_type});

    slot.entity = entity.id();
    slot.world_id = object.id;
    slot.occupied = true;
    const foundation::RuntimeId runtime_id{slot_index, slot.generation};
    impl_->by_world_id.emplace(object.id, runtime_id);
    return runtime_id;
}

bool Runtime::destroy(foundation::RuntimeId id) {
    if (!valid(id)) {
        return false;
    }

    auto& slot = impl_->slots[id.slot];
    impl_->world.entity(slot.entity).destruct();
    impl_->by_world_id.erase(slot.world_id);
    slot.entity = 0;
    slot.world_id = {};
    slot.occupied = false;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    impl_->free_slots.push_back(id.slot);
    return true;
}

bool Runtime::valid(foundation::RuntimeId id) const noexcept {
    if (!id.valid() || id.slot >= impl_->slots.size()) {
        return false;
    }
    const auto& slot = impl_->slots[id.slot];
    return slot.occupied && slot.generation == id.generation;
}

std::optional<foundation::RuntimeId> Runtime::find(foundation::WorldId id) const noexcept {
    const auto iterator = impl_->by_world_id.find(id);
    return iterator == impl_->by_world_id.end() ? std::nullopt
                                                 : std::optional{iterator->second};
}

std::optional<foundation::WorldId> Runtime::world_id(foundation::RuntimeId id) const noexcept {
    return valid(id) ? std::optional{impl_->slots[id.slot].world_id} : std::nullopt;
}

RuntimeSnapshot Runtime::snapshot() const {
    RuntimeSnapshot snapshot;
    snapshot.entities.reserve(impl_->by_world_id.size());

    for (const auto& slot : impl_->slots) {
        if (!slot.occupied) {
            continue;
        }
        const auto entity = impl_->world.entity(slot.entity);
        const auto& position = entity.get<PositionComponent>();
        const auto& semantic_type = entity.get<SemanticTypeComponent>();
        snapshot.entities.push_back(RuntimeEntitySnapshot{
            slot.world_id,
            {position.x, position.y, position.z},
            semantic_type.value
        });
    }

    std::sort(snapshot.entities.begin(), snapshot.entities.end(), [](const auto& left, const auto& right) {
        return left.world_id < right.world_id;
    });
    return snapshot;
}

bool Runtime::restore(const RuntimeSnapshot& snapshot) {
    if (!snapshot.valid()) {
        return false;
    }
    std::unordered_set<foundation::WorldId, foundation::WorldIdHash> ids;
    for (const auto& entity : snapshot.entities) {
        if (!entity.world_id.valid() || !ids.insert(entity.world_id).second) {
            return false;
        }
    }

    clear();
    for (const auto& entity : snapshot.entities) {
        world::WorldObject object;
        object.id = entity.world_id;
        object.position = entity.position;
        object.semantic_type = entity.semantic_type;
        if (!activate(object).has_value()) {
            clear();
            return false;
        }
    }
    return true;
}

std::size_t Runtime::size() const noexcept { return impl_->by_world_id.size(); }

void Runtime::clear() {
    for (std::uint32_t index = 0; index < impl_->slots.size(); ++index) {
        const auto& slot = impl_->slots[index];
        if (slot.occupied) {
            static_cast<void>(destroy({index, slot.generation}));
        }
    }
}

} // namespace geoworld::ecs
