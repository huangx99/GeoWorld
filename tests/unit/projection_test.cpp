#include "geoworld/projection/canonical.hpp"
#include "geoworld/foundation/thread_pool.hpp"
#include "geoworld/projection/config.hpp"
#include "geoworld/projection/engine.hpp"
#include "geoworld/projection/frame.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
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

[[nodiscard]] geoworld::spatial::Ecef origin_ecef() {
    return geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
}

[[nodiscard]] geoworld::world::PositionEcef at_enu(double east, double north, double up) {
    const geoworld::spatial::Ecef point = geoworld::spatial::enu_to_ecef(
        origin_ecef(), kOriginGeodetic, geoworld::spatial::Enu{east, north, up});
    return geoworld::world::PositionEcef{point.x, point.y, point.z};
}

[[nodiscard]] geoworld::world::WorldObject make_object(std::uint64_t id,
                                                       double east, double north) {
    geoworld::world::WorldObject object;
    object.id = WorldId{id};
    object.position = at_enu(east, north, 0.0);
    object.semantic_type = "test.entity";
    object.geometry_ref = "asset/test";
    object.lifecycle = geoworld::world::LifecycleState::active;
    object.properties.emplace("speed", 12.5);
    object.properties.emplace("secret", std::int64_t{7});
    object.state.emplace("mode", std::string{"idle"});
    object.capabilities.push_back("move");
    object.capabilities.push_back("move");
    object.capabilities.push_back("sense");
    return object;
}

[[nodiscard]] ProjectionConfig test_config() {
    ProjectionConfig config;
    config.enu_origin = origin_ecef();
    return config;
}

[[nodiscard]] ProjectionPolicy test_policy() {
    ProjectionPolicy policy;
    policy.allow_property("speed");
    policy.allow_state_key("mode");
    policy.allow_capability("move");
    return policy;
}

[[nodiscard]] Subscription area_subscription() {
    Subscription subscription;
    subscription.area = geoworld::spatial::Aabb{
        geoworld::spatial::Enu{-100.0, -100.0, -100.0},
        geoworld::spatial::Enu{100.0, 100.0, 100.0},
    };
    return subscription;
}

// 参考 replica：应用 keyframe/delta 后计算与服务端一致的规范化视图 hash。
class Replica {
public:
    void apply(const StateFrame& frame, const ProjectionPolicy& policy) {
        static_cast<void>(policy);
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
        std::vector<std::pair<WorldId, std::pair<FeatureId, std::uint64_t>>> keyed;
        keyed.reserve(entities_.size());
        for (const auto& [wid, entry] : entities_) {
            keyed.emplace_back(wid, entry);
        }
        std::sort(keyed.begin(), keyed.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second.first < rhs.second.first;
        });
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto& [wid, entry] : keyed) {
            fold(hash, entry.first.value);
            fold(hash, wid.value);
            fold(hash, entry.second);
        }
        return hash;
    }

private:
    static void fold(std::uint64_t& hash, std::uint64_t value) {
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            hash = (hash ^ ((value >> shift) & 0xFFU)) * kFnvPrime;
        }
    }

    std::unordered_map<WorldId, std::pair<FeatureId, std::uint64_t>,
                       geoworld::foundation::WorldIdHash> entities_;
};

[[nodiscard]] const Keyframe* as_keyframe(const std::optional<StateFrame>& frame) {
    if (!frame.has_value()) {
        return nullptr;
    }
    return std::get_if<Keyframe>(&*frame);
}

[[nodiscard]] const Delta* as_delta(const std::optional<StateFrame>& frame) {
    if (!frame.has_value()) {
        return nullptr;
    }
    return std::get_if<Delta>(&*frame);
}

[[nodiscard]] bool policy_trims_fields() {
    ProjectionPolicy policy = test_policy();
    const geoworld::projection::ProjectedEntity projected =
        policy.project(make_object(1, 0.0, 0.0));
    if (projected.properties.size() != 1 || !projected.properties.contains("speed")) {
        return false;
    }
    if (projected.state.size() != 1 || !projected.state.contains("mode")) {
        return false;
    }
    if (projected.capabilities.size() != 1 || projected.capabilities.front() != "move") {
        return false;
    }
    return policy.version() > 1;
}

[[nodiscard]] bool canonical_hash_is_stable() {
    ProjectionPolicy policy = test_policy();
    geoworld::world::WorldObject object = make_object(2, 10.0, 10.0);
    const auto first = geoworld::projection::projected_entity_hash(policy.project(object));
    const auto second = geoworld::projection::projected_entity_hash(policy.project(object));
    if (first != second) {
        return false;
    }
    object.position = at_enu(11.0, 10.0, 0.0);
    const auto moved = geoworld::projection::projected_entity_hash(policy.project(object));
    if (moved == first) {
        return false;
    }
    // fid 是每连接局部映射，不参与规范化 hash。
    auto projected = policy.project(make_object(2, 10.0, 10.0));
    projected.fid = FeatureId{42};
    return geoworld::projection::projected_entity_hash(projected) == first;
}

[[nodiscard]] bool keyframe_then_delta_updates() {
    geoworld::world::World world;
    static_cast<void>(world.insert(make_object(1, 10.0, 10.0)));
    static_cast<void>(world.insert(make_object(2, 20.0, 20.0)));
    static_cast<void>(world.insert(make_object(3, 500.0, 500.0)));

    ProjectionPolicy policy = test_policy();
    ProjectionEngine engine{test_config(), policy};
    if (!engine.add_connection(ConnectionId{1}, area_subscription())) {
        return false;
    }
    engine.on_projection(world, 0, 0);

    const std::optional<StateFrame> first = engine.next_frame(ConnectionId{1});
    const Keyframe* keyframe = as_keyframe(first);
    if (keyframe == nullptr || keyframe->entities.size() != 2) {
        return false;
    }
    if (keyframe->baseline_snapshot_id != 0 || keyframe->stream_epoch != 1
        || keyframe->stream_sequence != 1) {
        return false;
    }
    // enter 按 WID 升序，FID 按 WID 升序从 1 分配。
    if (keyframe->entities[0].wid != WorldId{1} || keyframe->entities[0].fid != FeatureId{1}
        || keyframe->entities[1].wid != WorldId{2}
        || keyframe->entities[1].fid != FeatureId{2}) {
        return false;
    }

    Replica replica;
    replica.apply(*first, policy);
    if (replica.hash() != engine.connection_view_hash(ConnectionId{1})) {
        return false;
    }

    static_cast<void>(engine.acknowledge(ConnectionId{1}, keyframe->snapshot_id));

    // 通过可变指针直接改位置（不递增 version），变化检测仍必须产生 update。
    static_cast<void>(world.update(WorldId{2}, [](auto& object) {
        object.position = at_enu(25.0, 20.0, 0.0);
    }));
    engine.on_projection(world, 1, 0);
    const std::optional<StateFrame> second = engine.next_frame(ConnectionId{1});
    const Delta* delta = as_delta(second);
    if (delta == nullptr || delta->updates.size() != 1 || !delta->enters.empty()
        || !delta->leaves.empty()) {
        return false;
    }
    if (delta->baseline_snapshot_id != keyframe->snapshot_id) {
        return false;
    }
    // update 携带完整最新投影，不做字段 patch。
    if (delta->updates[0].wid != WorldId{2}
        || delta->updates[0].entity.properties.size() != 1
        || delta->updates[0].entity.state.size() != 1) {
        return false;
    }
    replica.apply(*second, policy);
    if (replica.hash() != engine.connection_view_hash(ConnectionId{1})) {
        return false;
    }

    // 对象离开 AOI 产生 leave；销毁产生 destroyed。
    static_cast<void>(world.update(WorldId{1}, [](auto& object) {
        object.position = at_enu(500.0, 500.0, 0.0);
    }));
    static_cast<void>(world.erase(WorldId{2}));
    engine.on_projection(world, 2, 0);
    const std::optional<StateFrame> third = engine.next_frame(ConnectionId{1});
    const Delta* third_delta = as_delta(third);
    if (third_delta == nullptr || third_delta->leaves.size() != 2) {
        return false;
    }
    // leave 按 FID 升序。
    if (third_delta->leaves[0].reason != geoworld::projection::LeaveReason::out_of_area
        || third_delta->leaves[1].reason != geoworld::projection::LeaveReason::destroyed) {
        return false;
    }
    replica.apply(*third, policy);
    return replica.hash() == engine.connection_view_hash(ConnectionId{1});
}

[[nodiscard]] bool ack_rules() {
    geoworld::world::World world;
    static_cast<void>(world.insert(make_object(1, 10.0, 10.0)));

    ProjectionEngine engine{test_config(), test_policy()};
    static_cast<void>(engine.add_connection(ConnectionId{1}, area_subscription()));
    engine.on_projection(world, 0, 0);
    const auto first = engine.next_frame(ConnectionId{1});
    const Keyframe* keyframe = as_keyframe(first);
    if (keyframe == nullptr) {
        return false;
    }

    using geoworld::projection::AckResult;
    if (engine.acknowledge(ConnectionId{1}, keyframe->snapshot_id) != AckResult::accepted) {
        return false;
    }
    if (engine.acknowledge(ConnectionId{1}, keyframe->snapshot_id)
        != AckResult::duplicate_ignored) {
        return false;
    }
    if (engine.acknowledge(ConnectionId{1}, keyframe->snapshot_id + 100)
        != AckResult::error_unknown_snapshot) {
        return false;
    }

    // 重连建立新 epoch，旧 epoch 的 ack 一律拒绝，第一帧必须是 keyframe。
    static_cast<void>(engine.reset_stream_epoch(ConnectionId{1}));
    engine.on_projection(world, 1, 0);
    const auto second = engine.next_frame(ConnectionId{1});
    const Keyframe* second_keyframe = as_keyframe(second);
    if (second_keyframe == nullptr || second_keyframe->stream_epoch != 2
        || second_keyframe->stream_sequence != 1) {
        return false;
    }
    if (second_keyframe->entities.size() != 1
        || second_keyframe->entities[0].fid != FeatureId{1}) {
        return false;
    }
    return engine.acknowledge(ConnectionId{1}, keyframe->snapshot_id)
        == AckResult::error_epoch_mismatch;
}

[[nodiscard]] bool baseline_eviction_forces_keyframe() {
    geoworld::world::World world;
    static_cast<void>(world.insert(make_object(1, 10.0, 10.0)));

    ProjectionConfig config = test_config();
    config.max_unacked_frames = 3;
    config.keyframe_interval_seconds = 3600;
    ProjectionEngine engine{config, test_policy()};
    static_cast<void>(engine.add_connection(ConnectionId{1}, area_subscription()));

    engine.on_projection(world, 0, 0);
    if (as_keyframe(engine.next_frame(ConnectionId{1})) == nullptr) {
        return false;
    }
    // 不 ack，持续产生 delta 直到未确认记录超过上限。
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        static_cast<void>(world.set_property(WorldId{1}, "speed",
                                             12.5 + static_cast<double>(tick)));
        engine.on_projection(world, tick, 0);
        if (as_delta(engine.next_frame(ConnectionId{1})) == nullptr) {
            return false;
        }
    }
    // 第三条 delta 淘汰了未确认记录，下一帧必须回退 keyframe。
    static_cast<void>(world.set_property(WorldId{1}, "speed", 99.0));
    engine.on_projection(world, 4, 0);
    return as_keyframe(engine.next_frame(ConnectionId{1})) != nullptr;
}

[[nodiscard]] bool fid_exhaustion_rebuilds_epoch() {
    geoworld::world::World world;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        static_cast<void>(world.insert(make_object(id, 10.0 * id, 10.0)));
    }

    ProjectionConfig config = test_config();
    config.max_feature_ids_per_epoch = 4;
    ProjectionEngine engine{config, test_policy()};
    static_cast<void>(engine.add_connection(ConnectionId{1}, area_subscription()));
    engine.on_projection(world, 0, 0);
    if (as_keyframe(engine.next_frame(ConnectionId{1})) == nullptr) {
        return false;
    }

    static_cast<void>(world.insert(make_object(4, 50.0, 10.0)));
    static_cast<void>(world.insert(make_object(5, 60.0, 10.0)));
    engine.on_projection(world, 1, 0);
    const auto frame = engine.next_frame(ConnectionId{1});
    const Keyframe* keyframe = as_keyframe(frame);
    if (keyframe == nullptr || keyframe->stream_epoch != 2) {
        return false;
    }
    const auto* connection = engine.connection(ConnectionId{1});
    return connection != nullptr
        && connection->last_epoch_reset_reason()
            == geoworld::projection::EpochResetReason::fid_exhausted;
}

[[nodiscard]] bool slow_frequency_gates_updates() {
    geoworld::world::World world;
    static_cast<void>(world.insert(make_object(1, 10.0, 10.0)));

    ProjectionPolicy policy = test_policy();
    policy.set_frequency_resolver(
        [](const geoworld::world::WorldObject&) {
            return geoworld::projection::FrequencyClass::slow;
        });
    ProjectionConfig config = test_config();
    config.slow_frequency_hz = 2;
    ProjectionEngine engine{config, policy};
    static_cast<void>(engine.add_connection(ConnectionId{1}, area_subscription()));

    engine.on_projection(world, 0, 0);
    const auto first = engine.next_frame(ConnectionId{1});
    const Keyframe* keyframe = as_keyframe(first);
    if (keyframe == nullptr) {
        return false;
    }
    static_cast<void>(engine.acknowledge(ConnectionId{1}, keyframe->snapshot_id));

    // slow 档位 2 Hz、20 ms tick 对应 25 tick 间隔，到期前的变化不发送。
    static_cast<void>(world.set_property(WorldId{1}, "speed", 13.0));
    engine.on_projection(world, 1, 0);
    if (engine.next_frame(ConnectionId{1}).has_value()) {
        return false;
    }
    engine.on_projection(world, 30, 0);
    const std::optional<StateFrame> frame = engine.next_frame(ConnectionId{1});
    const Delta* delta = as_delta(frame);
    return delta != nullptr && delta->updates.size() == 1;
}

[[nodiscard]] bool history_and_determinism() {
    geoworld::world::World world;
    for (std::uint64_t id = 1; id <= 4; ++id) {
        static_cast<void>(world.insert(make_object(id, 10.0 * id, 10.0)));
    }

    ProjectionConfig config = test_config();
    config.snapshot_history_frames = 3;
    ProjectionEngine first_engine{config, test_policy()};
    ProjectionEngine second_engine{config, test_policy()};
    static_cast<void>(first_engine.add_connection(ConnectionId{1}, area_subscription()));
    static_cast<void>(second_engine.add_connection(ConnectionId{2}, area_subscription()));

    for (std::uint64_t tick = 0; tick < 5; ++tick) {
        first_engine.on_projection(world, tick, 0);
        second_engine.on_projection(world, tick, 0);
    }
    if (first_engine.history_size() > 3) {
        return false;
    }
    // 相同世界状态、策略和订阅得到逐字节一致的规范化视图。
    return first_engine.connection_view_hash(ConnectionId{1})
        == second_engine.connection_view_hash(ConnectionId{2});
}

[[nodiscard]] bool invalid_config_is_rejected() {
    ProjectionConfig config = test_config();
    config.data_frequency_hz = 0;
    std::string diagnostic;
    if (validate(config, diagnostic) || diagnostic.empty()) {
        return false;
    }
    config = test_config();
    config.enu_origin = geoworld::spatial::Ecef{};
    return !validate(config, diagnostic);
}

// 脏检查覆盖全部修改路径：版本递增、原始指针位置变化、白名单外修改、
// 同 version/position 删除重建（插入序号区分），结果与逐实体全量重算一致。
[[nodiscard]] bool dirty_check_tracks_all_mutation_paths() {
    geoworld::world::World world;
    static_cast<void>(world.insert(make_object(1, 10.0, 10.0)));
    static_cast<void>(world.insert(make_object(2, 20.0, 20.0)));

    ProjectionEngine engine{test_config(), test_policy()};
    static_cast<void>(engine.add_connection(ConnectionId{1}, area_subscription()));
    Replica replica;

    const auto drain = [&engine, &replica](ConnectionId id) {
        const std::optional<StateFrame> frame = engine.next_frame(id);
        if (frame.has_value()) {
            replica.apply(*frame, engine.policy());
        }
        return frame;
    };

    engine.on_projection(world, 0, 0);
    const std::optional<StateFrame> keyframe = drain(ConnectionId{1});
    if (as_keyframe(keyframe) == nullptr) {
        return false;
    }
    static_cast<void>(engine.acknowledge(
        ConnectionId{1}, as_keyframe(keyframe)->snapshot_id));

    // 版本递增的属性修改产生 update。
    static_cast<void>(world.set_property(WorldId{1}, "speed", 13.0));
    engine.on_projection(world, 1, 0);
    const std::optional<StateFrame> property_delta = drain(ConnectionId{1});
    const Delta* delta = as_delta(property_delta);
    if (delta == nullptr || delta->updates.size() != 1
        || delta->updates.front().wid != WorldId{1}) {
        return false;
    }
    static_cast<void>(engine.acknowledge(ConnectionId{1}, delta->snapshot_id));

    // 原始指针位置变化（不递增 version）同样被检测为变化。
    static_cast<void>(world.update(WorldId{1}, [](auto& object) {
        object.position = at_enu(15.0, 10.0, 0.0);
    }));
    engine.on_projection(world, 2, 0);
    const std::optional<StateFrame> moved_delta = drain(ConnectionId{1});
    delta = as_delta(moved_delta);
    if (delta == nullptr || delta->updates.size() != 1
        || delta->updates.front().wid != WorldId{1}) {
        return false;
    }
    static_cast<void>(engine.acknowledge(ConnectionId{1}, delta->snapshot_id));

    // 白名单外字段的原始指针修改（不递增 version/位置）不改变规范化投影，
    // 与全量重算一样不产生任何帧。
    static_cast<void>(world.update(WorldId{1}, [](auto& object) {
        object.properties["secret"] = std::int64_t{8};
    }));
    engine.on_projection(world, 3, 0);
    if (drain(ConnectionId{1}).has_value()) {
        return false;
    }

    // 同 version、同 position 删除重建：插入序号不同，白名单属性变化必须被检测。
    static_cast<void>(world.erase(WorldId{2}));
    geoworld::world::WorldObject recreated = make_object(2, 20.0, 20.0);
    recreated.properties["speed"] = 99.0;
    static_cast<void>(world.insert(std::move(recreated)));
    engine.on_projection(world, 4, 0);
    const std::optional<StateFrame> recreated_delta = drain(ConnectionId{1});
    delta = as_delta(recreated_delta);
    if (delta == nullptr || !delta->enters.empty() || !delta->leaves.empty()
        || delta->updates.size() != 1 || delta->updates.front().wid != WorldId{2}) {
        return false;
    }
    static_cast<void>(engine.acknowledge(ConnectionId{1}, delta->snapshot_id));

    return replica.hash() == engine.connection_view_hash(ConnectionId{1});
}

// 并行派发与串行逐帧一致：相同输入下帧序列、FID 分配与连接视图 hash 完全相同。
[[nodiscard]] bool parallel_matches_sequential() {
    constexpr std::uint64_t kEntities = 400;
    constexpr std::uint64_t kConnections = 8;
    constexpr std::uint64_t kTicks = 12;

    geoworld::world::World world;
    for (std::uint64_t index = 1; index <= kEntities; ++index) {
        const double east = static_cast<double>(index % 20U) * 10.0 - 95.0;
        const double north = static_cast<double>(index / 20U) * 10.0 - 95.0;
        static_cast<void>(world.insert(make_object(index, east, north)));
    }

    ProjectionEngine sequential{test_config(), test_policy()};
    ProjectionEngine parallel{test_config(), test_policy()};
    parallel.set_thread_pool(std::make_shared<geoworld::foundation::ThreadPool>(4));
    for (std::uint64_t id = 1; id <= kConnections; ++id) {
        static_cast<void>(sequential.add_connection(ConnectionId{id}, area_subscription()));
        static_cast<void>(parallel.add_connection(ConnectionId{id}, area_subscription()));
    }

    // 帧指纹：kind/snapshot + 各操作集合的 (wid, fid, 实体规范化 hash)。
    const auto fingerprint = [](const std::optional<StateFrame>& frame) {
        std::uint64_t hash = 14695981039346656037ULL;
        const auto fold = [&hash](std::uint64_t value) {
            constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
            for (unsigned shift = 0; shift < 64U; shift += 8U) {
                hash = (hash ^ ((value >> shift) & 0xFFU)) * kFnvPrime;
            }
        };
        if (!frame.has_value()) {
            return hash;
        }
        fold(frame->index());
        const auto fold_entity = [&fold](const auto& entry) {
            fold(entry.wid.value);
            fold(entry.fid.value);
            fold(geoworld::projection::projected_entity_hash(entry.entity));
        };
        if (const auto* keyframe = std::get_if<Keyframe>(&*frame)) {
            fold(keyframe->snapshot_id);
            fold(keyframe->stream_epoch);
            for (const auto& enter : keyframe->entities) {
                fold_entity(enter);
            }
            return hash;
        }
        const auto& delta = std::get<Delta>(*frame);
        fold(delta.snapshot_id);
        fold(delta.baseline_snapshot_id);
        for (const auto& enter : delta.enters) {
            fold_entity(enter);
        }
        for (const auto& update : delta.updates) {
            fold_entity(update);
        }
        for (const auto& leave : delta.leaves) {
            fold(leave.wid.value);
            fold(leave.fid.value);
        }
        return hash;
    };

    for (std::uint64_t tick = 0; tick < kTicks; ++tick) {
        static_cast<void>(world.set_property(
            WorldId{1 + (tick * 37U) % kEntities}, "speed", 12.5 + tick));
        static_cast<void>(world.update(
            WorldId{1 + (tick * 11U) % kEntities}, [tick](auto& object) {
                object.position = at_enu(static_cast<double>(tick % 10U), 10.0, 0.0);
            }));

        sequential.on_projection(world, tick, 0);
        parallel.on_projection(world, tick, 0);
        for (std::uint64_t id = 1; id <= kConnections; ++id) {
            const std::optional<StateFrame> expected =
                sequential.next_frame(ConnectionId{id});
            const std::optional<StateFrame> actual =
                parallel.next_frame(ConnectionId{id});
            if (fingerprint(expected) != fingerprint(actual)) {
                return false;
            }
            const auto snapshot_of = [](const std::optional<StateFrame>& frame) {
                if (!frame.has_value()) {
                    return std::uint64_t{0};
                }
                if (const auto* keyframe = std::get_if<Keyframe>(&*frame)) {
                    return keyframe->snapshot_id;
                }
                return std::get<Delta>(*frame).snapshot_id;
            };
            const std::uint64_t snapshot = snapshot_of(expected);
            if (snapshot != 0) {
                static_cast<void>(sequential.acknowledge(ConnectionId{id}, snapshot));
                static_cast<void>(parallel.acknowledge(ConnectionId{id}, snapshot));
            }
        }
    }
    for (std::uint64_t id = 1; id <= kConnections; ++id) {
        if (sequential.connection_view_hash(ConnectionId{id})
            != parallel.connection_view_hash(ConnectionId{id})) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!policy_trims_fields()) {
        return 1;
    }
    if (!canonical_hash_is_stable()) {
        return 2;
    }
    if (!keyframe_then_delta_updates()) {
        return 3;
    }
    if (!ack_rules()) {
        return 4;
    }
    if (!baseline_eviction_forces_keyframe()) {
        return 5;
    }
    if (!fid_exhaustion_rebuilds_epoch()) {
        return 6;
    }
    if (!slow_frequency_gates_updates()) {
        return 7;
    }
    if (!history_and_determinism()) {
        return 8;
    }
    if (!invalid_config_is_rejected()) {
        return 9;
    }
    if (!dirty_check_tracks_all_mutation_paths()) {
        return 10;
    }
    if (!parallel_matches_sequential()) {
        return 11;
    }
    return 0;
}
