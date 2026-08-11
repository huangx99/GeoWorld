#pragma once

#include "geoworld/world/world.hpp"

#include <optional>
#include <string>
#include <vector>

namespace geoworld::debug {

struct ObjectSummary {
    foundation::WorldId id;
    std::string semantic_type;
    world::LifecycleState lifecycle;
    std::uint64_t version;
};

class WorldInspector {
public:
    explicit WorldInspector(const world::World& world) noexcept : world_(world) {}

    [[nodiscard]] std::vector<ObjectSummary> list() const;
    [[nodiscard]] std::optional<world::WorldObject> inspect(foundation::WorldId id) const;

private:
    const world::World& world_;
};

} // namespace geoworld::debug
