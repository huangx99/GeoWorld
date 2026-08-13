#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/projection/entity.hpp"
#include "geoworld/spatial/spatial_query.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace geoworld::projection {

// 虚拟连接的独立强类型。连接不一定对应世界实体，严禁用 WID 充当连接 ID。
struct ConnectionId {
    std::uint64_t value{};

    constexpr auto operator<=>(const ConnectionId&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};

struct ConnectionIdHash {
    std::size_t operator()(ConnectionId id) const noexcept {
        return static_cast<std::size_t>(id.value ^ (id.value >> 32U));
    }
};

// 订阅以 ENU AABB 或“跟随 WID + 半径”描述兴趣区域；跟随是订阅参数，不是连接身份。
struct Subscription {
    std::optional<spatial::Aabb> area;
    std::optional<foundation::WorldId> follow;
    double follow_radius_meters{};
};

struct TrackedEntity {
    foundation::FeatureId fid;
    std::uint64_t entity_hash{};
    FrequencyClass frequency{FrequencyClass::on_change};
    std::uint64_t last_sent_tick{};
};

struct SentOp {
    enum class Kind { enter, update, leave };

    Kind kind{Kind::enter};
    foundation::WorldId wid;
    foundation::FeatureId fid;
    std::uint64_t entity_hash{};
};

struct SentFrameRecord {
    std::uint64_t snapshot_id{};
    std::vector<SentOp> ops;
    std::size_t bytes{};
};

enum class EpochResetReason { none, reconnect, fid_exhausted };

// 每连接投影状态：WID↔FID 映射、可见集、已确认基线、未确认发送记录。
// 不为每个连接复制完整世界；enter/update 只记录推进已确认 replica 所需的操作。
class ConnectionProjection {
public:
    ConnectionProjection() = default;
    explicit ConnectionProjection(ConnectionId id) noexcept;

    [[nodiscard]] ConnectionId id() const noexcept;
    [[nodiscard]] std::uint64_t stream_epoch() const noexcept;
    [[nodiscard]] std::uint64_t next_stream_sequence() const noexcept;
    [[nodiscard]] EpochResetReason last_epoch_reset_reason() const noexcept;

    [[nodiscard]] const Subscription& subscription() const noexcept;
    void set_subscription(Subscription subscription);

    // FID 在 epoch 内只增不复用；耗尽返回无效 FeatureId，由引擎重建 epoch。
    [[nodiscard]] foundation::FeatureId allocate_fid();
    [[nodiscard]] std::size_t fid_count() const noexcept;
    void map_fid(foundation::WorldId wid, foundation::FeatureId fid);
    [[nodiscard]] std::optional<foundation::FeatureId> fid_of(foundation::WorldId wid) const;
    [[nodiscard]] std::optional<foundation::WorldId> wid_of(foundation::FeatureId fid) const;

    [[nodiscard]] std::unordered_map<foundation::WorldId, TrackedEntity,
                                     foundation::WorldIdHash>& current_view() noexcept;
    [[nodiscard]] const std::unordered_map<foundation::WorldId, TrackedEntity,
                                           foundation::WorldIdHash>& current_view() const noexcept;
    [[nodiscard]] std::unordered_map<foundation::WorldId, TrackedEntity,
                                     foundation::WorldIdHash>& acked_view() noexcept;

    [[nodiscard]] std::uint64_t acked_baseline() const noexcept;
    void set_acked_baseline(std::uint64_t snapshot_id) noexcept;

    [[nodiscard]] std::uint64_t policy_version() const noexcept;
    void set_policy_version(std::uint64_t version) noexcept;

    [[nodiscard]] bool needs_keyframe() const noexcept;
    void set_needs_keyframe(bool needed) noexcept;

    [[nodiscard]] std::uint64_t last_keyframe_tick() const noexcept;
    void set_last_keyframe_tick(std::uint64_t tick) noexcept;

    // 下一个周期 keyframe 的到期 tick（0=尚未安排，首轮由错峰公式决定）。
    [[nodiscard]] std::uint64_t next_keyframe_tick() const noexcept;
    void set_next_keyframe_tick(std::uint64_t tick) noexcept;

    void record_sent(std::uint64_t snapshot_id, std::vector<SentOp> ops);
    void advance_stream_sequence() noexcept;
    // 淘汰最旧未确认记录；返回是否有未确认记录被淘汰（发生则必须 keyframe 回退）。
    [[nodiscard]] bool evict_unacked(std::size_t max_frames, std::size_t max_bytes);
    void drop_records_through(std::uint64_t snapshot_id);
    [[nodiscard]] const std::deque<SentFrameRecord>& unacked_records() const noexcept;
    [[nodiscard]] std::size_t unacked_bytes() const noexcept;

    // 增量视图收敛支撑：unacked_wid_counts 统计每个 WID 出现在几条未确认记录中；
    // acked_dirty 记录上次帧构建后被 ack 推进过的 WID。两者使引擎能把
    // current_view 就地收敛到「acked ∪ 本帧操作」，与整表拷贝语义逐字节一致。
    [[nodiscard]] const std::unordered_map<foundation::WorldId, std::uint32_t,
                                           foundation::WorldIdHash>&
    unacked_wid_counts() const noexcept;
    [[nodiscard]] const std::vector<foundation::WorldId>& acked_dirty() const noexcept;
    void mark_acked_dirty(foundation::WorldId wid);
    void clear_acked_dirty() noexcept;

    void reset_epoch(EpochResetReason reason, std::uint64_t snapshot_floor);

    [[nodiscard]] std::uint64_t epoch_snapshot_floor() const noexcept;

private:
    void release_record_wids(const SentFrameRecord& record);

    ConnectionId id_{};
    Subscription subscription_{};
    std::uint64_t stream_epoch_{1};
    std::uint64_t stream_sequence_{};
    std::uint64_t epoch_snapshot_floor_{};
    EpochResetReason last_epoch_reset_reason_{EpochResetReason::none};
    std::uint32_t next_fid_{1};
    std::unordered_map<foundation::WorldId, foundation::FeatureId,
                       foundation::WorldIdHash> fid_by_wid_;
    std::unordered_map<std::uint32_t, foundation::WorldId> wid_by_fid_;
    std::unordered_map<foundation::WorldId, TrackedEntity,
                       foundation::WorldIdHash> current_view_;
    std::unordered_map<foundation::WorldId, TrackedEntity,
                       foundation::WorldIdHash> acked_view_;
    std::uint64_t acked_baseline_{};
    std::uint64_t policy_version_{};
    bool needs_keyframe_{true};
    std::uint64_t last_keyframe_tick_{};
    std::uint64_t next_keyframe_tick_{};
    std::deque<SentFrameRecord> unacked_records_;
    std::size_t unacked_bytes_{};
    std::unordered_map<foundation::WorldId, std::uint32_t,
                       foundation::WorldIdHash> unacked_wid_counts_;
    std::vector<foundation::WorldId> acked_dirty_;
};

} // namespace geoworld::projection
