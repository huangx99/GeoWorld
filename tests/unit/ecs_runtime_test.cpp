#include "geoworld/ecs/runtime.hpp"
#include "geoworld/ecs/schema.hpp"
#include "geoworld/ecs/snapshot_codec.hpp"

int main() {
    using geoworld::foundation::WorldId;

    const auto& schemas = geoworld::ecs::component_schemas();
    if (schemas.size() != 3 || schemas[1].fields.size() != 3) {
        return 1;
    }

    geoworld::ecs::Runtime runtime;
    geoworld::world::WorldObject object;
    object.id = WorldId{91};
    object.position = {10.0, 20.0, 30.0};
    object.semantic_type = "test.vehicle";

    const auto first = runtime.activate(object);
    if (!first.has_value() || runtime.size() != 1 || runtime.world_id(*first) != object.id) {
        return 1;
    }
    if (runtime.activate(object).has_value() || runtime.snapshot().entities.size() != 1) {
        return 1;
    }
    if (!runtime.destroy(*first) || runtime.valid(*first)) {
        return 1;
    }

    const auto second = runtime.activate(object);
    if (!second.has_value() || second->slot != first->slot || second->generation == first->generation) {
        return 1;
    }

    const auto snapshot = runtime.snapshot();
    if (!snapshot.valid() || snapshot.format_version != 1) {
        return 1;
    }
    geoworld::ecs::Runtime restored;
    const auto encoded = geoworld::ecs::encode_snapshot(snapshot);
    const auto decoded = geoworld::ecs::decode_snapshot(encoded);
    if (encoded.empty() || !decoded.has_value() || !restored.restore(*decoded)
        || restored.size() != 1) {
        return 1;
    }
    auto incompatible = snapshot;
    incompatible.schema_version = 2;
    if (!geoworld::ecs::encode_snapshot(incompatible).empty()) {
        return 1;
    }
    const auto restored_id = restored.find(WorldId{91});
    return restored_id.has_value() && restored.world_id(*restored_id) == WorldId{91} ? 0 : 1;
}
