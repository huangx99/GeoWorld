#include "geoworld/projection/connection.hpp"

namespace geoworld::projection {

ConnectionProjection::ConnectionProjection(ConnectionId id) noexcept : id_(id) {}

[[nodiscard]] ConnectionId ConnectionProjection::id() const noexcept {
    return id_;
}

[[nodiscard]] std::uint64_t ConnectionProjection::stream_epoch() const noexcept {
    return stream_epoch_;
}

[[nodiscard]] std::uint64_t ConnectionProjection::next_stream_sequence() const noexcept {
    return stream_sequence_ + 1;
}

[[nodiscard]] EpochResetReason ConnectionProjection::last_epoch_reset_reason() const noexcept {
    return last_epoch_reset_reason_;
}

[[nodiscard]] const Subscription& ConnectionProjection::subscription() const noexcept {
    return subscription_;
}

void ConnectionProjection::set_subscription(Subscription subscription) {
    subscription_ = std::move(subscription);
    needs_keyframe_ = true;
}

[[nodiscard]] foundation::FeatureId ConnectionProjection::allocate_fid() {
    foundation::FeatureId fid{next_fid_};
    ++next_fid_;
    return fid;
}

[[nodiscard]] std::size_t ConnectionProjection::fid_count() const noexcept {
    return fid_by_wid_.size();
}

void ConnectionProjection::map_fid(foundation::WorldId wid, foundation::FeatureId fid) {
    fid_by_wid_[wid] = fid;
    wid_by_fid_[fid.value] = wid;
}

[[nodiscard]] std::optional<foundation::FeatureId> ConnectionProjection::fid_of(
    foundation::WorldId wid) const {
    const auto found = fid_by_wid_.find(wid);
    if (found == fid_by_wid_.end()) {
        return std::nullopt;
    }
    return found->second;
}

[[nodiscard]] std::optional<foundation::WorldId> ConnectionProjection::wid_of(
    foundation::FeatureId fid) const {
    const auto found = wid_by_fid_.find(fid.value);
    if (found == wid_by_fid_.end()) {
        return std::nullopt;
    }
    return found->second;
}

[[nodiscard]] std::unordered_map<foundation::WorldId, TrackedEntity,
                                 foundation::WorldIdHash>&
ConnectionProjection::current_view() noexcept {
    return current_view_;
}

[[nodiscard]] const std::unordered_map<foundation::WorldId, TrackedEntity,
                                       foundation::WorldIdHash>&
ConnectionProjection::current_view() const noexcept {
    return current_view_;
}

[[nodiscard]] std::unordered_map<foundation::WorldId, TrackedEntity,
                                 foundation::WorldIdHash>&
ConnectionProjection::acked_view() noexcept {
    return acked_view_;
}

[[nodiscard]] std::uint64_t ConnectionProjection::acked_baseline() const noexcept {
    return acked_baseline_;
}

void ConnectionProjection::set_acked_baseline(std::uint64_t snapshot_id) noexcept {
    acked_baseline_ = snapshot_id;
}

[[nodiscard]] std::uint64_t ConnectionProjection::policy_version() const noexcept {
    return policy_version_;
}

void ConnectionProjection::set_policy_version(std::uint64_t version) noexcept {
    policy_version_ = version;
}

[[nodiscard]] bool ConnectionProjection::needs_keyframe() const noexcept {
    return needs_keyframe_;
}

void ConnectionProjection::set_needs_keyframe(bool needed) noexcept {
    needs_keyframe_ = needed;
}

[[nodiscard]] std::uint64_t ConnectionProjection::last_keyframe_tick() const noexcept {
    return last_keyframe_tick_;
}

void ConnectionProjection::set_last_keyframe_tick(std::uint64_t tick) noexcept {
    last_keyframe_tick_ = tick;
}

[[nodiscard]] std::uint64_t ConnectionProjection::next_keyframe_tick() const noexcept {
    return next_keyframe_tick_;
}

void ConnectionProjection::set_next_keyframe_tick(std::uint64_t tick) noexcept {
    next_keyframe_tick_ = tick;
}

void ConnectionProjection::record_sent(std::uint64_t snapshot_id, std::vector<SentOp> ops) {
    SentFrameRecord record;
    record.snapshot_id = snapshot_id;
    record.bytes = ops.size() * sizeof(SentOp);
    record.ops = std::move(ops);
    unacked_bytes_ += record.bytes;
    for (const SentOp& op : record.ops) {
        ++unacked_wid_counts_[op.wid];
    }
    unacked_records_.push_back(std::move(record));
}

void ConnectionProjection::advance_stream_sequence() noexcept {
    ++stream_sequence_;
}

void ConnectionProjection::release_record_wids(const SentFrameRecord& record) {
    for (const SentOp& op : record.ops) {
        const auto found = unacked_wid_counts_.find(op.wid);
        if (found != unacked_wid_counts_.end() && --found->second == 0) {
            unacked_wid_counts_.erase(found);
        }
    }
}

[[nodiscard]] bool ConnectionProjection::evict_unacked(std::size_t max_frames,
                                                       std::size_t max_bytes) {
    bool evicted_unacked = false;
    while (unacked_records_.size() > max_frames || unacked_bytes_ > max_bytes) {
        const bool was_unacked =
            unacked_records_.front().snapshot_id > acked_baseline_;
        unacked_bytes_ -= unacked_records_.front().bytes;
        release_record_wids(unacked_records_.front());
        unacked_records_.pop_front();
        if (was_unacked) {
            // 未确认记录被上限淘汰后，已确认基线无法安全推进，必须 keyframe 回退。
            evicted_unacked = true;
        }
    }
    return evicted_unacked;
}

void ConnectionProjection::drop_records_through(std::uint64_t snapshot_id) {
    while (!unacked_records_.empty()
           && unacked_records_.front().snapshot_id <= snapshot_id) {
        unacked_bytes_ -= unacked_records_.front().bytes;
        release_record_wids(unacked_records_.front());
        unacked_records_.pop_front();
    }
}

[[nodiscard]] const std::deque<SentFrameRecord>&
ConnectionProjection::unacked_records() const noexcept {
    return unacked_records_;
}

[[nodiscard]] std::size_t ConnectionProjection::unacked_bytes() const noexcept {
    return unacked_bytes_;
}

[[nodiscard]] const std::unordered_map<foundation::WorldId, std::uint32_t,
                                       foundation::WorldIdHash>&
ConnectionProjection::unacked_wid_counts() const noexcept {
    return unacked_wid_counts_;
}

[[nodiscard]] const std::vector<foundation::WorldId>&
ConnectionProjection::acked_dirty() const noexcept {
    return acked_dirty_;
}

void ConnectionProjection::mark_acked_dirty(foundation::WorldId wid) {
    acked_dirty_.push_back(wid);
}

void ConnectionProjection::clear_acked_dirty() noexcept {
    acked_dirty_.clear();
}

void ConnectionProjection::reset_epoch(EpochResetReason reason,
                                       std::uint64_t snapshot_floor) {
    ++stream_epoch_;
    stream_sequence_ = 0;
    epoch_snapshot_floor_ = snapshot_floor;
    last_epoch_reset_reason_ = reason;
    next_fid_ = 1;
    fid_by_wid_.clear();
    wid_by_fid_.clear();
    current_view_.clear();
    acked_view_.clear();
    acked_baseline_ = 0;
    needs_keyframe_ = true;
    unacked_records_.clear();
    unacked_bytes_ = 0;
    unacked_wid_counts_.clear();
    acked_dirty_.clear();
}

[[nodiscard]] std::uint64_t ConnectionProjection::epoch_snapshot_floor() const noexcept {
    return epoch_snapshot_floor_;
}

} // namespace geoworld::projection
