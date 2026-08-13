#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/protocol/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace geoworld::protocol {

// 参考客户端渲染采样延迟，冻结于 docs/M4.md 配置基线（interpolation_delay = 100 ms）。
inline constexpr std::int64_t default_interpolation_delay_us = 100'000;

// 与 libs/projection/src/canonical.cpp 完全相同的 FNV-1a 字段序列化算法；
// fid 不参与 hash（它是每连接 epoch 内的局部映射）。
[[nodiscard]] std::uint64_t wire_entity_hash(const WireEntity& entity) noexcept;

// 参考客户端 replica accumulator：keyframe 整表替换；delta enter 插入、
// update 整实体替换、leave 删除；ReliableEvent 与 Heartbeat 不参与 replica 状态。
// replica hash 按 FID 升序折叠 (fid, wid, entity_hash)，与服务端连接投影视图 hash 一致。
class ReplicaAccumulator {
public:
    void apply(const WireFrame& frame);
    void apply_keyframe(const WireKeyframe& keyframe);
    void apply_delta(const WireDelta& delta);

    [[nodiscard]] std::uint64_t hash() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    void clear() noexcept;

private:
    struct Entry {
        foundation::FeatureId fid{};
        std::uint64_t entity_hash{};
    };

    std::unordered_map<foundation::WorldId, Entry, foundation::WorldIdHash> entities_;
};

// 插值输入缓冲：按 (tick, simulation_time_us) 保存最近快照的实体位置，
// 支持按渲染时刻（默认最新仿真时间 - interpolation_delay）在两个快照间线性插值。
// 超出缓冲范围的采样时刻钳制到最近快照，缺失实体返回空。
class InterpolationBuffer {
public:
    explicit InterpolationBuffer(std::int64_t delay_us = default_interpolation_delay_us) noexcept;

    void observe(const WireKeyframe& keyframe);
    void observe(const WireDelta& delta);

    [[nodiscard]] std::int64_t render_time_us() const noexcept;
    [[nodiscard]] std::optional<WireVec3> sample(foundation::WorldId wid,
                                                 std::int64_t time_us) const;
    [[nodiscard]] std::size_t snapshot_count() const noexcept;

private:
    struct Snapshot {
        std::uint64_t tick{};
        std::int64_t simulation_time_us{};
        std::unordered_map<foundation::WorldId, WireVec3, foundation::WorldIdHash> positions;
    };

    void retain(Snapshot snapshot);

    std::int64_t delay_us_;
    std::deque<Snapshot> snapshots_;
};

} // namespace geoworld::protocol
