#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/schema/value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace geoworld::world {

using WorldId = foundation::WorldId;

using PropertyValue = schema::Value;
using PropertyBag = schema::PropertyBag;

struct PositionEcef {
    double x{};
    double y{};
    double z{};
};

enum class LifecycleState { staged, active, suspended, retired };

struct Relation {
    WorldId target;
    std::string type;
    PropertyBag attributes;
};

struct WorldObject {
    WorldId id;
    std::string geometry_ref;
    PositionEcef position;
    std::string semantic_type;
    PropertyBag properties;
    PropertyBag state;
    std::vector<Relation> relations;
    std::vector<std::string> capabilities;
    LifecycleState lifecycle{LifecycleState::staged};
    std::uint64_t version{1};
};

class World {
public:
    [[nodiscard]] bool insert(WorldObject object);
    [[nodiscard]] bool erase(WorldId id);
    [[nodiscard]] WorldObject* find(WorldId id) noexcept;
    [[nodiscard]] const WorldObject* find(WorldId id) const noexcept;
    [[nodiscard]] bool set_property(WorldId id, std::string key, PropertyValue value);
    [[nodiscard]] bool add_relation(WorldId source, Relation relation);
    [[nodiscard]] std::vector<WorldObject> snapshot() const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<WorldId, WorldObject, foundation::WorldIdHash> objects_;
};

} // namespace geoworld::world
