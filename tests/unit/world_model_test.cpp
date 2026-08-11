#include "geoworld/debug/state_hash.hpp"
#include "geoworld/debug/world_inspector.hpp"
#include "geoworld/world/world.hpp"

int main() {
    using geoworld::foundation::WorldId;

    geoworld::world::World world;
    geoworld::world::WorldObject building;
    building.id = WorldId{100};
    building.geometry_ref = "assets/building.glb";
    building.position = {1.0, 2.0, 3.0};
    building.semantic_type = "city.building";
    building.properties["name"] = std::string{"A"};
    building.state["occupied"] = true;
    building.capabilities.push_back("evacuate");
    building.lifecycle = geoworld::world::LifecycleState::active;

    if (!world.insert(building) || world.insert(building)) {
        return 1;
    }
    const auto initial_hash = geoworld::debug::world_state_hash(world);
    if (!world.set_property(WorldId{100}, "height", 32.0)) {
        return 1;
    }

    geoworld::debug::WorldInspector inspector{world};
    const auto inspected = inspector.inspect(WorldId{100});
    if (!inspected.has_value() || inspected->version != 2 || inspector.list().size() != 1) {
        return 1;
    }
    return initial_hash != geoworld::debug::world_state_hash(world) ? 0 : 1;
}
