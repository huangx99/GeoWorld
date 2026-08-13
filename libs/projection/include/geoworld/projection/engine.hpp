#pragma once

#include "geoworld/foundation/thread_pool.hpp"
#include "geoworld/projection/canonical.hpp"
#include "geoworld/projection/config.hpp"
#include "geoworld/projection/connection.hpp"
#include "geoworld/projection/frame.hpp"
#include "geoworld/projection/policy.hpp"
#include "geoworld/spatial/spatial_query.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace geoworld::projection {

// 共享规范化变化记录：全局按 snapshot 组织一份，所有连接引用同一份不可变投影。
struct SnapshotChangeSet {
    std::uint64_t snapshot_id{};
    std::vector<std::pair<foundation::WorldId,
                          std::shared_ptr<const ProjectedEntity>>> changed;
    std::vector<foundation::WorldId> removed;
};

// 客户端投影内核。只在稳定 tick 边界消费只读世界，不持有网络连接，不修改世界。
class ProjectionEngine {
public:
    using RelevanceFilter = std::function<bool(ConnectionId, const world::WorldObject&)>;

    ProjectionEngine(ProjectionConfig config, ProjectionPolicy policy);

    [[nodiscard]] const ProjectionConfig& config() const noexcept;
    [[nodiscard]] ProjectionPolicy& policy() noexcept;
    void set_relevance_filter(RelevanceFilter filter);

    [[nodiscard]] bool add_connection(ConnectionId id, Subscription subscription);
    [[nodiscard]] bool remove_connection(ConnectionId id);
    [[nodiscard]] bool update_subscription(ConnectionId id, Subscription subscription);
    // 传输重连：建立新 epoch、重置 FID，旧 epoch 的帧和 ack 一律拒绝。
    [[nodiscard]] bool reset_stream_epoch(ConnectionId id);

    // 只读投影观察入口：Track A/B/C 完成、状态 hash 已计算、clock.advance 之前调用。
    void on_projection(const world::World& world, std::uint64_t tick, std::uint64_t state_hash);

    // 取走该连接当前 snapshot 的待发帧；无变化且无 keyframe 到期时返回 nullopt。
    [[nodiscard]] std::optional<StateFrame> next_frame(ConnectionId id);

    // ack 只接受当前 epoch 内已发送的 snapshot；重复或较旧 ack 幂等忽略。
    [[nodiscard]] AckResult acknowledge(ConnectionId id, std::uint64_t snapshot_id);

    // 安排该连接的下一状态帧为 keyframe（基线过期、重同步、客户端请求）。
    [[nodiscard]] bool schedule_keyframe(ConnectionId id);

    // 服务端该连接投影视图的规范化 hash；客户端应用帧后的 replica hash 必须一致。
    [[nodiscard]] std::uint64_t connection_view_hash(ConnectionId id) const;

    [[nodiscard]] std::uint64_t latest_snapshot_id() const noexcept;
    [[nodiscard]] std::size_t history_size() const noexcept;
    [[nodiscard]] std::size_t connection_count() const noexcept;
    [[nodiscard]] const ConnectionProjection* connection(ConnectionId id) const noexcept;

    // 注入共享线程池后，on_projection 的 project+hash 与逐连接帧构建并行执行
    //（各槽位独立，输出与串行逐字节一致）；帧构建在设置相关性过滤器时保持串行。
    // 并行期间策略的 frequency/priority resolver 可能被并发调用，必须保持纯净。
    void set_thread_pool(std::shared_ptr<foundation::ThreadPool> pool);

private:
    struct EntityState {
        std::shared_ptr<const ProjectedEntity> projected;
        std::uint64_t hash{};
        spatial::Enu enu;
        // 脏检查基线：来源对象的插入序号、内容版本、位置与投影时策略版本。
        std::uint64_t source_revision{};
        std::uint64_t source_version{};
        world::PositionEcef source_position{};
        std::uint64_t policy_version{};
    };

    // 每连接 AOI 结果缓存：空间修订号与 bounds 都未变时可见集与重算一致。
    struct VisibleCache {
        bool valid{false};
        bool has_bounds{false};
        spatial::Aabb bounds{};
        std::uint64_t spatial_revision{};
        std::vector<foundation::WorldId> visible;
    };

    [[nodiscard]] std::uint64_t interval_ticks(std::uint32_t frequency_hz) const noexcept;
    [[nodiscard]] std::optional<spatial::Aabb> resolve_bounds(
        const ConnectionProjection& connection) const;
    [[nodiscard]] const std::vector<foundation::WorldId>& resolve_visible(
        const ConnectionProjection& connection, VisibleCache& cache,
        const std::optional<spatial::Aabb>& bounds,
        const world::World& world, bool& cache_hit);
    [[nodiscard]] std::optional<StateFrame> build_frame(
        ConnectionProjection& connection, VisibleCache& cache,
        std::unordered_set<foundation::WorldId, foundation::WorldIdHash>& throttle_pending,
        const world::World& world, std::uint64_t tick);
    [[nodiscard]] bool keyframe_due(const ConnectionProjection& connection,
                                    std::uint64_t tick) const;
    [[nodiscard]] std::uint64_t keyframe_interval_ticks() const noexcept;
    void apply_epoch_reset(ConnectionProjection& connection, EpochResetReason reason);

    // 并行派发槽位：主线程在派发前取好每连接状态指针，工作线程只写自己的槽位。
    struct ParallelSlot {
        ConnectionProjection* connection{};
        VisibleCache* cache{};
        std::unordered_set<foundation::WorldId, foundation::WorldIdHash>* throttle_pending{};
        ConnectionId id{};
    };

    // project+hash 并行槽位：工作线程按索引写入，主线程按发现顺序串行合流。
    struct ProjectedSlot {
        std::shared_ptr<const ProjectedEntity> projected;
        std::uint64_t hash{};
        spatial::Enu enu{};
    };

    // 脏扫描缓存条目：镜像 EntityState 的脏检查基线，按 WID 升序纯顺序读。
    struct ScanEntry {
        foundation::WorldId wid{};
        std::uint64_t source_revision{};
        std::uint64_t source_version{};
        world::PositionEcef source_position{};
        std::uint64_t policy_version{};
    };

    // 脏对象在扫描缓存中无对应条目（全局视图尚未见过的新对象）。
    static constexpr std::size_t kNoScanEntry =
        std::numeric_limits<std::size_t>::max();

    ProjectionConfig config_;
    ProjectionPolicy policy_;
    RelevanceFilter relevance_filter_;
    spatial::Geodetic enu_origin_geodetic_{};
    spatial::SpatialQuery spatial_;
    std::uint64_t latest_snapshot_id_{};
    std::uint64_t last_processed_tick_{};
    std::uint64_t spatial_revision_{};
    std::uint64_t seen_erase_revision_{};
    std::unordered_map<foundation::WorldId, EntityState,
                       foundation::WorldIdHash> global_view_;
    std::deque<SnapshotChangeSet> history_;
    std::unordered_map<ConnectionId, ConnectionProjection, ConnectionIdHash> connections_;
    std::unordered_map<ConnectionId, std::optional<StateFrame>, ConnectionIdHash> pending_frames_;
    std::unordered_map<ConnectionId, VisibleCache, ConnectionIdHash> visible_caches_;
    std::unordered_map<ConnectionId, std::unordered_set<foundation::WorldId,
                                                        foundation::WorldIdHash>,
                       ConnectionIdHash> throttle_pending_;
    std::vector<foundation::WorldId> tick_changed_wids_;
    std::vector<foundation::WorldId> scratch_visible_;
    std::vector<const world::WorldObject*> world_snapshot_;
    std::uint64_t world_snapshot_erase_revision_{};
    std::vector<const world::WorldObject*> dirty_objects_;
    std::vector<char> dirty_moved_;
    std::vector<std::size_t> dirty_cache_index_;
    std::vector<ScanEntry> scan_cache_;
    bool scan_cache_dirty_{true};
    std::vector<ProjectedSlot> projected_slots_;
    std::vector<std::vector<std::pair<foundation::WorldId,
                                      std::shared_ptr<const ProjectedEntity>>>>
        retired_changes_;
    std::shared_ptr<foundation::ThreadPool> thread_pool_;
    std::vector<ParallelSlot> parallel_slots_;
    std::vector<std::optional<StateFrame>> parallel_frames_;
};

} // namespace geoworld::projection
