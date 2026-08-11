#include "geoworld/ecs/snapshot_codec.hpp"

#include "runtime_snapshot_generated.h"

#include <flatbuffers/flatbuffers.h>

namespace geoworld::ecs {

std::vector<std::uint8_t> encode_snapshot(const RuntimeSnapshot& snapshot) {
    if (!snapshot.valid()) {
        return {};
    }

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<geoworld::snapshot::RuntimeEntity>> entities;
    entities.reserve(snapshot.entities.size());
    for (const auto& entity : snapshot.entities) {
        const auto position = geoworld::snapshot::CreatePositionEcef(
            builder, entity.position.x, entity.position.y, entity.position.z);
        const auto semantic_type = builder.CreateString(entity.semantic_type);
        entities.push_back(geoworld::snapshot::CreateRuntimeEntity(
            builder, entity.world_id.value, position, semantic_type));
    }

    const auto root = geoworld::snapshot::CreateRuntimeSnapshot(
        builder,
        snapshot.format_version,
        snapshot.schema_version,
        builder.CreateVector(entities));
    geoworld::snapshot::FinishRuntimeSnapshotBuffer(builder, root);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

std::optional<RuntimeSnapshot> decode_snapshot(std::span<const std::uint8_t> buffer) {
    flatbuffers::Verifier verifier{buffer.data(), buffer.size()};
    if (!geoworld::snapshot::VerifyRuntimeSnapshotBuffer(verifier)) {
        return std::nullopt;
    }

    const auto* source = geoworld::snapshot::GetRuntimeSnapshot(buffer.data());
    RuntimeSnapshot snapshot;
    snapshot.format_version = source->format_version();
    snapshot.schema_version = source->schema_version();
    if (!snapshot.valid()) {
        return std::nullopt;
    }

    if (const auto* entities = source->entities(); entities != nullptr) {
        snapshot.entities.reserve(entities->size());
        for (const auto* entity : *entities) {
            if (entity == nullptr || entity->position() == nullptr || entity->semantic_type() == nullptr) {
                return std::nullopt;
            }
            snapshot.entities.push_back({
                foundation::WorldId{entity->world_id()},
                {entity->position()->x(), entity->position()->y(), entity->position()->z()},
                entity->semantic_type()->str()
            });
        }
    }
    return snapshot;
}

} // namespace geoworld::ecs
