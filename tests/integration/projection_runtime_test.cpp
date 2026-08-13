#include "geoworld/projection/engine.hpp"
#include "geoworld/projection/frame.hpp"
#include "geoworld/runtime/world_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

using geoworld::foundation::FeatureId;
using geoworld::foundation::WorldId;
using geoworld::projection::ConnectionId;
using geoworld::projection::Delta;
using geoworld::projection::Keyframe;
using geoworld::projection::ProjectionConfig;
using geoworld::projection::ProjectionEngine;
using geoworld::projection::ProjectionPolicy;
using geoworld::projection::StateFrame;
using geoworld::projection::Subscription;

const geoworld::spatial::Geodetic kOriginGeodetic{31.0, 121.0, 0.0};

[[nodiscard]] geoworld::world::PositionEcef at_enu(double east, double north) {
    const geoworld::spatial::Ecef origin =
        geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
    const geoworld::spatial::Ecef point = geoworld::spatial::enu_to_ecef(
        origin, kOriginGeodetic, geoworld::spatial::Enu{east, north, 0.0});
    return geoworld::world::PositionEcef{point.x, point.y, point.z};
}

void fold(std::uint64_t& hash, std::uint64_t value) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        hash = (hash ^ ((value >> shift) & 0xFFU)) * kFnvPrime;
    }
}

class Replica {
public:
    void apply(const StateFrame& frame) {
        if (const auto* keyframe = std::get_if<Keyframe>(&frame)) {
            entities_.clear();
            for (const auto& enter : keyframe->entities) {
                entities_[enter.wid] = {
                    enter.fid, geoworld::projection::projected_entity_hash(enter.entity)};
            }
            return;
        }
        const auto& delta = std::get<Delta>(frame);
        for (const auto& leave : delta.leaves) {
            entities_.erase(leave.wid);
        }
        for (const auto& enter : delta.enters) {
            entities_[enter.wid] = {
                enter.fid, geoworld::projection::projected_entity_hash(enter.entity)};
        }
        for (const auto& update : delta.updates) {
            entities_[update.wid] = {
                update.fid, geoworld::projection::projected_entity_hash(update.entity)};
        }
    }

    [[nodiscard]] std::uint64_t hash() const {
        std::vector<std::pair<WorldId, FeatureId>> order;
        order.reserve(entities_.size());
        for (const auto& [wid, entry] : entities_) {
            order.emplace_back(wid, entry.first);
        }
        std::sort(order.begin(), order.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second < rhs.second;
        });
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto& [wid, fid] : order) {
            fold(hash, fid.value);
            fold(hash, wid.value);
            fold(hash, entities_.at(wid).second);
        }
        return hash;
    }

private:
    std::unordered_map<WorldId, std::pair<FeatureId, std::uint64_t>,
                       geoworld::foundation::WorldIdHash> entities_;
};

} // namespace

// WorldRuntime projection phase -> keyframe -> 参考客户端 -> 世界命令 -> delta -> hash 一致。
int main() {
    ProjectionConfig config;
    config.enu_origin = geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
    ProjectionPolicy policy;
    policy.allow_property("speed");

    ProjectionEngine engine{config, policy};
    Subscription subscription;
    subscription.area = geoworld::spatial::Aabb{
        geoworld::spatial::Enu{-100.0, -100.0, -100.0},
        geoworld::spatial::Enu{100.0, 100.0, 100.0},
    };
    if (!engine.add_connection(ConnectionId{1}, subscription)) {
        return 1;
    }

    geoworld::runtime::WorldRuntime runtime;
    bool observer_fired = false;
    std::uint64_t observed_hash = 0;
    runtime.add_projection_observer(
        [&engine, &observer_fired, &observed_hash](
            const geoworld::world::World& world, std::uint64_t tick,
            std::uint64_t state_hash, const geoworld::spatial::SpatialQuery* spatial) {
            static_cast<void>(spatial);
            observer_fired = true;
            observed_hash = state_hash;
            engine.on_projection(world, tick, state_hash);
        });

    geoworld::world::WorldObject object;
    object.id = WorldId{7};
    object.position = at_enu(10.0, 10.0);
    object.semantic_type = "test.entity";
    object.lifecycle = geoworld::world::LifecycleState::active;
    object.properties.emplace("speed", 5.0);

    static_cast<void>(runtime.submit(
        0, geoworld::simulation::CreateObjectCommand{object}));
    const auto first_step = runtime.step();
    if (!observer_fired || observed_hash != first_step.state_hash) {
        return 2;
    }

    const std::optional<StateFrame> first = engine.next_frame(ConnectionId{1});
    const auto* keyframe = first.has_value() ? std::get_if<Keyframe>(&*first) : nullptr;
    if (keyframe == nullptr || keyframe->entities.size() != 1
        || keyframe->entities[0].wid != WorldId{7}) {
        return 3;
    }
    Replica replica;
    replica.apply(*first);
    static_cast<void>(engine.acknowledge(ConnectionId{1}, keyframe->snapshot_id));
    if (replica.hash() != engine.connection_view_hash(ConnectionId{1})) {
        return 4;
    }

    // 世界命令经相位边界提交后，下一投影产生 delta 且 replica hash 一致。
    static_cast<void>(runtime.submit(
        1, geoworld::simulation::SetPropertyCommand{WorldId{7}, "speed", 9.5}));
    static_cast<void>(runtime.step());

    const std::optional<StateFrame> second = engine.next_frame(ConnectionId{1});
    const auto* delta = second.has_value() ? std::get_if<Delta>(&*second) : nullptr;
    if (delta == nullptr || delta->updates.size() != 1
        || delta->updates[0].wid != WorldId{7}) {
        return 5;
    }
    replica.apply(*second);
    if (replica.hash() != engine.connection_view_hash(ConnectionId{1})) {
        return 6;
    }
    return 0;
}
