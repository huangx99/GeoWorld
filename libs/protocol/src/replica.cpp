#include "geoworld/protocol/replica.hpp"

#include <algorithm>
#include <bit>
#include <string_view>
#include <utility>
#include <vector>

namespace geoworld::protocol {

namespace {

// FNV-1a 64，与 libs/projection/src/canonical.cpp 同一算法；
// 整数按小端字节序逐字节折叠，与平台字节序无关。
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void fold_u8(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash = (hash ^ value) * kFnvPrime;
}

void fold_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        fold_u8(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void fold_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        fold_u8(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void fold_f64(std::uint64_t& hash, double value) noexcept {
    fold_u64(hash, std::bit_cast<std::uint64_t>(value));
}

void fold_text(std::uint64_t& hash, std::string_view value) noexcept {
    fold_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const char character : value) {
        fold_u8(hash, static_cast<std::uint8_t>(character));
    }
}

void fold_value(std::uint64_t& hash, const WireValue& value) noexcept {
    fold_u8(hash, static_cast<std::uint8_t>(value.index()));
    switch (value.index()) {
    case 0:
        fold_u64(hash, std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value)));
        break;
    case 1:
        fold_f64(hash, std::get<double>(value));
        break;
    case 2:
        fold_u8(hash, std::get<bool>(value) ? 1U : 0U);
        break;
    case 3:
        fold_text(hash, std::get<std::string>(value));
        break;
    default:
        break;
    }
}

void fold_bag(std::uint64_t& hash, const WireAttributes& bag) noexcept {
    fold_u64(hash, static_cast<std::uint64_t>(bag.size()));
    for (const auto& [key, value] : bag) {
        fold_text(hash, key);
        fold_value(hash, value);
    }
}

} // namespace

std::uint64_t wire_entity_hash(const WireEntity& entity) noexcept {
    std::uint64_t hash = kFnvOffset;
    fold_u64(hash, entity.wid.value);
    fold_u64(hash, entity.version);
    fold_f64(hash, entity.position.x);
    fold_f64(hash, entity.position.y);
    fold_f64(hash, entity.position.z);
    fold_text(hash, entity.semantic_type);
    fold_text(hash, entity.geometry_ref);
    fold_u8(hash, static_cast<std::uint8_t>(entity.lifecycle));
    fold_bag(hash, entity.properties);
    fold_bag(hash, entity.state);

    fold_u64(hash, static_cast<std::uint64_t>(entity.relations.size()));
    for (const auto& relation : entity.relations) {
        fold_text(hash, relation.type);
        fold_u64(hash, relation.target_wid);
        // fbs RelationEntry 不携带 attributes，按 canonical.cpp 的空 bag 序列化。
        fold_u64(hash, 0);
    }

    fold_u64(hash, static_cast<std::uint64_t>(entity.capabilities.size()));
    for (const auto& capability : entity.capabilities) {
        fold_text(hash, capability);
    }

    fold_u8(hash, static_cast<std::uint8_t>(entity.frequency));
    fold_u32(hash, entity.priority);
    fold_u64(hash, static_cast<std::uint64_t>(entity.visibility_tags.size()));
    for (const auto& tag : entity.visibility_tags) {
        fold_text(hash, tag);
    }
    return hash;
}

void ReplicaAccumulator::apply(const WireFrame& frame) {
    if (const auto* keyframe = std::get_if<WireKeyframe>(&frame)) {
        apply_keyframe(*keyframe);
        return;
    }
    if (const auto* delta = std::get_if<WireDelta>(&frame)) {
        apply_delta(*delta);
    }
    // ReliableEvent 与 Heartbeat 不参与 replica 状态。
}

void ReplicaAccumulator::apply_keyframe(const WireKeyframe& keyframe) {
    entities_.clear();
    for (const auto& enter : keyframe.entities) {
        entities_[enter.wid] = Entry{enter.fid, wire_entity_hash(enter.entity)};
    }
}

void ReplicaAccumulator::apply_delta(const WireDelta& delta) {
    for (const auto& leave : delta.leaves) {
        entities_.erase(leave.wid);
    }
    for (const auto& enter : delta.enters) {
        entities_[enter.wid] = Entry{enter.fid, wire_entity_hash(enter.entity)};
    }
    for (const auto& update : delta.updates) {
        entities_[update.wid] = Entry{update.fid, wire_entity_hash(update.entity)};
    }
}

std::uint64_t ReplicaAccumulator::hash() const noexcept {
    struct Keyed {
        foundation::FeatureId fid;
        foundation::WorldId wid;
        std::uint64_t entity_hash;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(entities_.size());
    for (const auto& [wid, entry] : entities_) {
        keyed.push_back(Keyed{entry.fid, wid, entry.entity_hash});
    }
    std::sort(keyed.begin(), keyed.end(), [](const Keyed& lhs, const Keyed& rhs) {
        return lhs.fid < rhs.fid;
    });
    std::uint64_t hash = kFnvOffset;
    for (const auto& entry : keyed) {
        fold_u64(hash, entry.fid.value);
        fold_u64(hash, entry.wid.value);
        fold_u64(hash, entry.entity_hash);
    }
    return hash;
}

std::size_t ReplicaAccumulator::size() const noexcept {
    return entities_.size();
}

void ReplicaAccumulator::clear() noexcept {
    entities_.clear();
}

InterpolationBuffer::InterpolationBuffer(std::int64_t delay_us) noexcept
    : delay_us_(delay_us < 0 ? 0 : delay_us) {}

void InterpolationBuffer::observe(const WireKeyframe& keyframe) {
    Snapshot snapshot;
    snapshot.tick = keyframe.tick;
    snapshot.simulation_time_us = keyframe.simulation_time_us;
    for (const auto& enter : keyframe.entities) {
        snapshot.positions.emplace(enter.wid, enter.entity.position);
    }
    retain(std::move(snapshot));
}

void InterpolationBuffer::observe(const WireDelta& delta) {
    Snapshot snapshot;
    snapshot.tick = delta.tick;
    snapshot.simulation_time_us = delta.simulation_time_us;
    if (!snapshots_.empty()) {
        snapshot.positions = snapshots_.back().positions;
    }
    for (const auto& leave : delta.leaves) {
        snapshot.positions.erase(leave.wid);
    }
    for (const auto& enter : delta.enters) {
        snapshot.positions[enter.wid] = enter.entity.position;
    }
    for (const auto& update : delta.updates) {
        snapshot.positions[update.wid] = update.entity.position;
    }
    retain(std::move(snapshot));
}

std::int64_t InterpolationBuffer::render_time_us() const noexcept {
    if (snapshots_.empty()) {
        return 0;
    }
    return snapshots_.back().simulation_time_us - delay_us_;
}

std::optional<WireVec3> InterpolationBuffer::sample(foundation::WorldId wid,
                                                    std::int64_t time_us) const {
    if (snapshots_.empty()) {
        return std::nullopt;
    }
    const auto lookup = [wid](const Snapshot& snapshot) -> std::optional<WireVec3> {
        const auto found = snapshot.positions.find(wid);
        if (found == snapshot.positions.end()) {
            return std::nullopt;
        }
        return found->second;
    };
    if (time_us <= snapshots_.front().simulation_time_us) {
        return lookup(snapshots_.front());
    }
    if (time_us >= snapshots_.back().simulation_time_us) {
        return lookup(snapshots_.back());
    }
    // 找到 bracket：older.time <= time_us < newer.time。
    std::size_t newer_index = 1;
    while (newer_index < snapshots_.size()
           && snapshots_[newer_index].simulation_time_us <= time_us) {
        ++newer_index;
    }
    const Snapshot& older = snapshots_[newer_index - 1];
    const Snapshot& newer = snapshots_[newer_index];
    const auto older_position = lookup(older);
    const auto newer_position = lookup(newer);
    if (older_position.has_value() && newer_position.has_value()) {
        const double span =
            static_cast<double>(newer.simulation_time_us - older.simulation_time_us);
        const double alpha =
            static_cast<double>(time_us - older.simulation_time_us) / span;
        return WireVec3{
            older_position->x + (newer_position->x - older_position->x) * alpha,
            older_position->y + (newer_position->y - older_position->y) * alpha,
            older_position->z + (newer_position->z - older_position->z) * alpha,
        };
    }
    if (older_position.has_value()) {
        return older_position;
    }
    return newer_position;
}

std::size_t InterpolationBuffer::snapshot_count() const noexcept {
    return snapshots_.size();
}

void InterpolationBuffer::retain(Snapshot snapshot) {
    if (!snapshots_.empty()
        && snapshot.simulation_time_us <= snapshots_.back().simulation_time_us) {
        return; // 乱序或重复快照不参与插值
    }
    snapshots_.push_back(std::move(snapshot));
    const std::int64_t render = render_time_us();
    // 只淘汰早于渲染时刻且不再构成 bracket 左端点的快照。
    while (snapshots_.size() >= 2 && snapshots_[1].simulation_time_us <= render) {
        snapshots_.pop_front();
    }
}

} // namespace geoworld::protocol
