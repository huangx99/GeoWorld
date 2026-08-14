#include "geoworld/projection/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace geoworld::projection {

namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

// 并行派发粒度：按连接逐任务派发，原子领取天然均衡 keyframe 尖峰连接的负载。
constexpr std::size_t kChunksPerWorker = 16;

[[nodiscard]] spatial::Ecef to_ecef(world::PositionEcef position) noexcept {
    return spatial::Ecef{position.x, position.y, position.z};
}

// 位置按位比较：NaN 等异常值一律视为已变化，退化为全量重算，保证输出不变。
[[nodiscard]] bool position_equal(world::PositionEcef lhs,
                                  world::PositionEcef rhs) noexcept {
    return std::memcmp(&lhs, &rhs, sizeof(world::PositionEcef)) == 0;
}

[[nodiscard]] bool aabb_equal(const spatial::Aabb& lhs, const spatial::Aabb& rhs) noexcept {
    return std::memcmp(&lhs, &rhs, sizeof(spatial::Aabb)) == 0;
}

void hash_view_entry(std::uint64_t& hash, foundation::FeatureId fid,
                     foundation::WorldId wid, std::uint64_t entity_hash) noexcept {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    const auto fold = [&hash](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            hash = (hash ^ ((value >> shift) & 0xFFU)) * kFnvPrime;
        }
    };
    fold(fid.value);
    fold(wid.value);
    fold(entity_hash);
}

} // namespace

ProjectionEngine::ProjectionEngine(ProjectionConfig config, ProjectionPolicy policy)
    : config_(std::move(config)),
      policy_(std::move(policy)),
      spatial_(config_.cell_grid) {
    enu_origin_geodetic_ = spatial::ecef_to_geodetic(config_.enu_origin);
}

[[nodiscard]] const ProjectionConfig& ProjectionEngine::config() const noexcept {
    return config_;
}

[[nodiscard]] ProjectionPolicy& ProjectionEngine::policy() noexcept {
    return policy_;
}

void ProjectionEngine::set_relevance_filter(RelevanceFilter filter) {
    relevance_filter_ = std::move(filter);
}

[[nodiscard]] bool ProjectionEngine::add_connection(ConnectionId id,
                                                    Subscription subscription) {
    if (!id.valid() || connections_.contains(id)) {
        return false;
    }
    ConnectionProjection connection{id};
    connection.set_subscription(std::move(subscription));
    connections_.emplace(id, std::move(connection));
    return true;
}

[[nodiscard]] bool ProjectionEngine::remove_connection(ConnectionId id) {
    pending_frames_.erase(id);
    visible_caches_.erase(id);
    throttle_pending_.erase(id);
    return connections_.erase(id) != 0;
}

[[nodiscard]] bool ProjectionEngine::update_subscription(ConnectionId id,
                                                         Subscription subscription) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return false;
    }
    found->second.set_subscription(std::move(subscription));
    return true;
}

[[nodiscard]] bool ProjectionEngine::reset_stream_epoch(ConnectionId id) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return false;
    }
    found->second.reset_epoch(EpochResetReason::reconnect, latest_snapshot_id_);
    return true;
}

void ProjectionEngine::on_projection(const world::World& world, std::uint64_t tick,
                                     std::uint64_t state_hash) {
    static_cast<void>(state_hash);
    ++latest_snapshot_id_;
    last_processed_tick_ = tick;

    SnapshotChangeSet changes;
    changes.snapshot_id = latest_snapshot_id_;

    // 脏检查分两层：世界指针快照与扫描缓存都按 WID 升序，扫描是两路归并的
    // 纯顺序读，替代逐实体哈希查表。世界快照在世界成员变化（数量或 erase
    // 修订号变化）后重建；扫描缓存镜像 global_view_ 的脏检查基线，在全局
    // 视图成员变化（新对象进入、失效擦除）后重建。版本、插入序号、位置与
    // 策略版本全部未变的实体跳过，直接复用全局视图中的共享投影与 hash；
    // 语义与逐实体全量重算一致（空间查询结果内部排序，扫描顺序不影响输出）。
    const std::uint64_t policy_version = policy_.version();
    if (world_snapshot_.size() != world.size()
        || world_snapshot_erase_revision_ != world.erase_revision()
        || world_snapshot_storage_revision_ != world.storage_revision()) {
        world_snapshot_.clear();
        world_snapshot_.reserve(world.size());
        world.for_each_object([this](const world::WorldObject& object) {
            world_snapshot_.push_back(&object);
        });
        std::sort(world_snapshot_.begin(), world_snapshot_.end(),
            [](const world::WorldObject* lhs, const world::WorldObject* rhs) {
                return lhs->id < rhs->id;
            });
        world_snapshot_erase_revision_ = world.erase_revision();
        world_snapshot_storage_revision_ = world.storage_revision();
    }
    if (scan_cache_dirty_) {
        scan_cache_.clear();
        scan_cache_.reserve(global_view_.size());
        for (const auto& [wid, state] : global_view_) {
            scan_cache_.push_back(ScanEntry{wid, state.source_revision,
                                            state.source_version,
                                            state.source_position,
                                            state.policy_version});
        }
        std::sort(scan_cache_.begin(), scan_cache_.end(),
            [](const ScanEntry& lhs, const ScanEntry& rhs) { return lhs.wid < rhs.wid; });
        scan_cache_dirty_ = false;
    }
    dirty_objects_.clear();
    dirty_moved_.clear();
    dirty_cache_index_.clear();
    std::size_t cursor = 0;
    for (const world::WorldObject* object_ptr : world_snapshot_) {
        const world::WorldObject& object = *object_ptr;
        while (cursor < scan_cache_.size() && scan_cache_[cursor].wid < object.id) {
            ++cursor;
        }
        const bool known = cursor < scan_cache_.size()
            && scan_cache_[cursor].wid == object.id;
        if (known) {
            const ScanEntry& entry = scan_cache_[cursor];
            if (entry.source_revision == object.revision
                && entry.source_version == object.version
                && entry.policy_version == policy_version
                && position_equal(entry.source_position, object.position)) {
                continue;
            }
        }
        dirty_objects_.push_back(object_ptr);
        // 空间索引只随真实位置变化更新：属性变化不触碰索引，AOI 缓存保持有效。
        dirty_moved_.push_back(!known
            || !position_equal(scan_cache_[cursor].source_position, object.position));
        dirty_cache_index_.push_back(known ? cursor : kNoScanEntry);
    }

    // project+hash 与 ENU 换算是纯函数（只读策略白名单、resolver 与原点）：
    // 注入线程池时分块并行，各工作线程只写自己的槽位；未注入时串行计算，
    // 两种路径结果一致。
    projected_slots_.clear();
    projected_slots_.resize(dirty_objects_.size());
    const auto compute_projected = [this](std::size_t index) {
        const world::WorldObject& object = *dirty_objects_[index];
        ProjectedSlot& slot = projected_slots_[index];
        ProjectedEntity projected = policy_.project(object);
        slot.hash = projected_entity_hash(projected);
        slot.projected = std::make_shared<const ProjectedEntity>(std::move(projected));
        if (dirty_moved_[index]) {
            slot.enu = spatial::ecef_to_enu(
                config_.enu_origin, enu_origin_geodetic_, to_ecef(object.position));
        }
    };
    const std::size_t dirty_count = dirty_objects_.size();
    if (thread_pool_ != nullptr && dirty_count > 1) {
        const std::size_t workers =
            static_cast<std::size_t>(thread_pool_->worker_count()) + 1;
        const std::size_t chunk_count =
            std::max<std::size_t>(1, std::min(dirty_count, workers * kChunksPerWorker));
        const std::size_t chunk_size = (dirty_count + chunk_count - 1) / chunk_count;
        thread_pool_->run(chunk_count, [&compute_projected, chunk_size, dirty_count](
                                           std::size_t chunk) {
            const std::size_t first = chunk * chunk_size;
            const std::size_t last = std::min(dirty_count, first + chunk_size);
            for (std::size_t index = first; index < last; ++index) {
                compute_projected(index);
            }
        });
    } else {
        for (std::size_t index = 0; index < dirty_count; ++index) {
            compute_projected(index);
        }
    }

    // 合流保持串行且按发现顺序：空间索引 upsert、全局视图与变化记录的写入
    // 顺序与全串行执行完全一致。命中的扫描缓存条目就地同步基线；新对象没有
    // 缓存条目，标记成员已变化，下一 tick 重建缓存。
    for (std::size_t index = 0; index < dirty_count; ++index) {
        const world::WorldObject& object = *dirty_objects_[index];
        ProjectedSlot& slot = projected_slots_[index];
        const std::size_t cache_index = dirty_cache_index_[index];
        const auto sync_cache_entry = [this, &object, policy_version, cache_index]() {
            if (cache_index == kNoScanEntry) {
                return;
            }
            ScanEntry& entry = scan_cache_[cache_index];
            entry.source_revision = object.revision;
            entry.source_version = object.version;
            entry.source_position = object.position;
            entry.policy_version = policy_version;
        };
        const auto existing = global_view_.find(object.id);
        if (existing != global_view_.end() && existing->second.hash == slot.hash) {
            // 内容版本变化但规范化结果未变：只推进脏检查基线，不产生变化记录。
            EntityState& state = existing->second;
            state.source_revision = object.revision;
            state.source_version = object.version;
            state.policy_version = policy_version;
            sync_cache_entry();
            continue;
        }

        spatial::Enu enu;
        if (dirty_moved_[index]) {
            enu = slot.enu;
            static_cast<void>(spatial_.dynamic().upsert(object.id, enu));
            ++spatial_revision_;
        } else {
            enu = existing->second.enu;
        }
        global_view_[object.id] = EntityState{slot.projected, slot.hash, enu,
                                              object.revision, object.version,
                                              object.position, policy_version};
        if (cache_index == kNoScanEntry) {
            scan_cache_dirty_ = true;
        } else {
            sync_cache_entry();
        }
        changes.changed.emplace_back(object.id, std::move(slot.projected));
    }
    // 变化记录按 WID 升序冻结，与按序 snapshot 全量重算的可观察行为一致。
    std::sort(changes.changed.begin(), changes.changed.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    tick_changed_wids_.clear();
    tick_changed_wids_.reserve(changes.changed.size());
    for (const auto& [wid, stored] : changes.changed) {
        static_cast<void>(stored);
        tick_changed_wids_.push_back(wid);
    }

    // 失效扫描只在发生过 erase 时进行：insert 不会产生失效实体。
    if (world.erase_revision() != seen_erase_revision_) {
        seen_erase_revision_ = world.erase_revision();
        std::vector<foundation::WorldId> stale;
        for (const auto& [wid, state] : global_view_) {
            static_cast<void>(state);
            if (world.find(wid) == nullptr) {
                stale.push_back(wid);
            }
        }
        std::sort(stale.begin(), stale.end());
        for (const foundation::WorldId wid : stale) {
            global_view_.erase(wid);
            static_cast<void>(spatial_.dynamic().erase(wid));
            ++spatial_revision_;
            changes.removed.push_back(wid);
        }
        if (!stale.empty()) {
            scan_cache_dirty_ = true;
        }
    }

    history_.push_back(std::move(changes));
    while (history_.size() > config_.snapshot_history_frames) {
        retired_changes_.push_back(std::move(history_.front().changed));
        history_.pop_front();
    }

    if (thread_pool_ != nullptr && relevance_filter_ == nullptr
        && connections_.size() > 1) {
        // 并行边界：每连接的帧构建只写自己的 ConnectionProjection/缓存槽位，
        // 全局视图、空间索引、本 tick 变化集在构建阶段只读，输出与串行一致。
        parallel_slots_.clear();
        parallel_slots_.reserve(connections_.size());
        for (auto& [id, connection] : connections_) {
            parallel_slots_.push_back(ParallelSlot{&connection, &visible_caches_[id],
                                                   &throttle_pending_[id], id});
        }
        parallel_frames_.assign(parallel_slots_.size(), std::nullopt);
        const std::size_t slot_count = parallel_slots_.size();
        const std::size_t workers =
            static_cast<std::size_t>(thread_pool_->worker_count()) + 1;
        const std::size_t chunk_count =
            std::max<std::size_t>(1, std::min(slot_count, workers * kChunksPerWorker));
        const std::size_t chunk_size = (slot_count + chunk_count - 1) / chunk_count;
        // 任务空间 [0, chunk_count) 构建连接帧；[chunk_count, 总数) 并行销毁历史环
        // 淘汰的共享投影（map/string 析构无观察效应），两批共用一次派发与等待。
        const std::size_t retired_count = retired_changes_.size();
        const std::size_t retired_chunks = retired_count == 0 ? 0
            : std::max<std::size_t>(1, std::min(retired_count,
                                                workers * kChunksPerWorker));
        const std::size_t retired_chunk_size = retired_chunks == 0 ? 0
            : (retired_count + retired_chunks - 1) / retired_chunks;
        thread_pool_->run(chunk_count + retired_chunks,
                          [this, &world, tick, chunk_size, slot_count, chunk_count,
                           retired_chunk_size, retired_count](std::size_t chunk) {
            if (chunk < chunk_count) {
                const std::size_t first = chunk * chunk_size;
                const std::size_t last = std::min(slot_count, first + chunk_size);
                for (std::size_t index = first; index < last; ++index) {
                    ParallelSlot& slot = parallel_slots_[index];
                    parallel_frames_[index] = build_frame(*slot.connection, *slot.cache,
                                                          *slot.throttle_pending, world,
                                                          tick);
                }
                return;
            }
            const std::size_t first = (chunk - chunk_count) * retired_chunk_size;
            const std::size_t last = std::min(retired_count,
                                              first + retired_chunk_size);
            for (std::size_t index = first; index < last; ++index) {
                retired_changes_[index].clear();
            }
        });
        for (std::size_t index = 0; index < slot_count; ++index) {
            pending_frames_[parallel_slots_[index].id] =
                std::move(parallel_frames_[index]);
        }
        retired_changes_.clear();
        return;
    }

    for (auto& [id, connection] : connections_) {
        pending_frames_[id] = build_frame(connection, visible_caches_[id],
                                          throttle_pending_[id], world, tick);
    }
    // 未注入线程池：淘汰的共享投影由主线程随即销毁，与原随淘汰析构行为一致。
    retired_changes_.clear();
}

[[nodiscard]] std::optional<StateFrame> ProjectionEngine::next_frame(ConnectionId id) {
    const auto found = pending_frames_.find(id);
    if (found == pending_frames_.end()) {
        return std::nullopt;
    }
    std::optional<StateFrame> frame = std::move(found->second);
    pending_frames_.erase(found);
    return frame;
}

[[nodiscard]] AckResult ProjectionEngine::acknowledge(ConnectionId id,
                                                      std::uint64_t snapshot_id) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return AckResult::error_unknown_snapshot;
    }
    ConnectionProjection& connection = found->second;

    if (snapshot_id <= connection.epoch_snapshot_floor()) {
        return AckResult::error_epoch_mismatch;
    }
    if (snapshot_id <= connection.acked_baseline()) {
        return AckResult::duplicate_ignored;
    }

    const auto& records = connection.unacked_records();
    const auto record = std::find_if(records.begin(), records.end(),
        [snapshot_id](const SentFrameRecord& candidate) {
            return candidate.snapshot_id == snapshot_id;
        });
    if (record == records.end()) {
        return AckResult::error_unknown_snapshot;
    }

    // 按顺序重放已确认帧的操作，推进已确认 replica 视图。
    auto& acked = connection.acked_view();
    for (const SentFrameRecord& sent : records) {
        if (sent.snapshot_id > snapshot_id) {
            break;
        }
        for (const SentOp& op : sent.ops) {
            if (op.kind == SentOp::Kind::leave) {
                acked.erase(op.wid);
            } else {
                TrackedEntity& entry = acked[op.wid];
                entry.fid = op.fid;
                entry.entity_hash = op.entity_hash;
            }
            connection.mark_acked_dirty(op.wid);
        }
    }
    connection.set_acked_baseline(snapshot_id);
    connection.drop_records_through(snapshot_id);
    return AckResult::accepted;
}

[[nodiscard]] std::uint64_t ProjectionEngine::connection_view_hash(
    ConnectionId id) const {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return 0;
    }

    std::vector<std::pair<foundation::WorldId, const TrackedEntity*>> keyed;
    keyed.reserve(found->second.current_view().size());
    for (const auto& [wid, tracked] : found->second.current_view()) {
        keyed.emplace_back(wid, &tracked);
    }
    std::sort(keyed.begin(), keyed.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second->fid < rhs.second->fid;
        });

    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& [wid, tracked] : keyed) {
        hash_view_entry(hash, tracked->fid, wid, tracked->entity_hash);
    }
    return hash;
}

[[nodiscard]] std::uint64_t ProjectionEngine::latest_snapshot_id() const noexcept {
    return latest_snapshot_id_;
}

[[nodiscard]] bool ProjectionEngine::schedule_keyframe(ConnectionId id) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return false;
    }
    found->second.set_needs_keyframe(true);
    return true;
}

[[nodiscard]] std::size_t ProjectionEngine::history_size() const noexcept {
    return history_.size();
}

[[nodiscard]] std::size_t ProjectionEngine::connection_count() const noexcept {
    return connections_.size();
}

[[nodiscard]] const ConnectionProjection* ProjectionEngine::connection(
    ConnectionId id) const noexcept {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return nullptr;
    }
    return &found->second;
}

void ProjectionEngine::set_thread_pool(std::shared_ptr<foundation::ThreadPool> pool) {
    thread_pool_ = std::move(pool);
}

[[nodiscard]] std::uint64_t ProjectionEngine::interval_ticks(
    std::uint32_t frequency_hz) const noexcept {
    const double ticks_per_interval =
        static_cast<double>(kMicrosecondsPerSecond)
        / (static_cast<double>(config_.tick_dt_microseconds)
           * static_cast<double>(frequency_hz));
    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(ticks_per_interval)));
}

[[nodiscard]] std::optional<spatial::Aabb> ProjectionEngine::resolve_bounds(
    const ConnectionProjection& connection) const {
    const Subscription& subscription = connection.subscription();
    if (subscription.area.has_value()) {
        return subscription.area;
    }
    if (subscription.follow.has_value()) {
        const auto target = global_view_.find(*subscription.follow);
        if (target == global_view_.end()) {
            return std::nullopt;
        }
        const spatial::Enu& center = target->second.enu;
        const double radius = subscription.follow_radius_meters;
        return spatial::Aabb{
            spatial::Enu{center.east - radius, center.north - radius, center.up - radius},
            spatial::Enu{center.east + radius, center.north + radius, center.up + radius},
        };
    }
    return std::nullopt;
}

// 可见集缓存：空间索引修订号与解析后的 bounds 都未变时，AOI 结果与重算逐字节一致。
// 设置相关性过滤器时结果依赖实体内容，不走缓存。cache_hit 输出是否命中缓存。
[[nodiscard]] const std::vector<foundation::WorldId>& ProjectionEngine::resolve_visible(
    const ConnectionProjection& connection, VisibleCache& cache,
    const std::optional<spatial::Aabb>& bounds, const world::World& world,
    bool& cache_hit) {
    cache_hit = false;
    if (relevance_filter_) {
        scratch_visible_.clear();
        if (bounds.has_value()) {
            scratch_visible_ = spatial_.query(*bounds);
            std::erase_if(scratch_visible_, [this, &connection, &world](foundation::WorldId wid) {
                const world::WorldObject* object = world.find(wid);
                return object == nullptr || !relevance_filter_(connection.id(), *object);
            });
        }
        return scratch_visible_;
    }

    const bool bounds_same = cache.valid && bounds.has_value() == cache.has_bounds
        && (!bounds.has_value() || aabb_equal(*bounds, cache.bounds));
    if (bounds_same && cache.spatial_revision == spatial_revision_) {
        cache_hit = true;
        return cache.visible;
    }

    cache.visible.clear();
    if (bounds.has_value()) {
        cache.visible = spatial_.query(*bounds);
    }
    cache.has_bounds = bounds.has_value();
    if (bounds.has_value()) {
        cache.bounds = *bounds;
    }
    cache.spatial_revision = spatial_revision_;
    cache.valid = true;
    return cache.visible;
}

[[nodiscard]] std::uint64_t ProjectionEngine::keyframe_interval_ticks() const noexcept {
    return (static_cast<std::uint64_t>(config_.keyframe_interval_seconds)
            * static_cast<std::uint64_t>(kMicrosecondsPerSecond))
           / static_cast<std::uint64_t>(config_.tick_dt_microseconds);
}

[[nodiscard]] bool ProjectionEngine::keyframe_due(
    const ConnectionProjection& connection, std::uint64_t tick) const {
    const std::uint64_t interval_ticks_value = keyframe_interval_ticks();
    // 已安排周期 keyframe 后严格按 next 到期：间隔恒为 interval，相位偏移由
    // 上次安排时的错峰永久保持，不会像旧公式那样把周期缩成 interval - phase。
    if (connection.next_keyframe_tick() != 0) {
        return tick >= connection.next_keyframe_tick();
    }
    // 首轮（连接建立后尚未安排过周期 keyframe）：按连接 ID 相位错峰，
    // 到期条件为 tick >= last + interval - phase，使不同连接的首个周期
    // keyframe 均匀散开，避免整批连接同 tick 尖峰。
    const std::uint64_t phase =
        static_cast<std::uint64_t>(ConnectionIdHash{}(connection.id()))
        % interval_ticks_value;
    return tick + phase >= connection.last_keyframe_tick() + interval_ticks_value;
}

void ProjectionEngine::apply_epoch_reset(ConnectionProjection& connection,
                                         EpochResetReason reason) {
    connection.reset_epoch(reason, latest_snapshot_id_);
}

[[nodiscard]] std::optional<StateFrame> ProjectionEngine::build_frame(
    ConnectionProjection& connection, VisibleCache& cache,
    std::unordered_set<foundation::WorldId, foundation::WorldIdHash>& throttle_pending,
    const world::World& world, std::uint64_t tick) {
    const std::optional<spatial::Aabb> bounds = resolve_bounds(connection);

    // 候选 -> 读权限/相关性，顺序冻结，频率与优先级只作用于 update。
    bool visible_from_cache = false;
    const std::vector<foundation::WorldId>& visible =
        resolve_visible(connection, cache, bounds, world, visible_from_cache);

    const auto& acked = connection.acked_view();
    auto visible_contains = [&visible](foundation::WorldId wid) {
        return std::binary_search(visible.begin(), visible.end(), wid);
    };

    const bool periodic_due = keyframe_due(connection, tick);
    bool forced_keyframe = connection.needs_keyframe()
        || connection.policy_version() != policy_.version();

    // FID 空间不足时按规则重建 epoch，第一帧必须是 keyframe。
    // 缓存命中时可见集与映射相对上一帧未变：新可见实体只随缓存重建出现，
    // enter 构建时已建立映射，未映射计数必为 0，跳过全量扫描。
    std::size_t unmapped = 0;
    if (!visible_from_cache) {
        for (const foundation::WorldId wid : visible) {
            if (!connection.fid_of(wid).has_value()) {
                ++unmapped;
            }
        }
    }
    if (connection.fid_count() + unmapped > config_.max_feature_ids_per_epoch) {
        apply_epoch_reset(connection, EpochResetReason::fid_exhausted);
        forced_keyframe = true;
    }

    if (forced_keyframe || periodic_due) {
        Keyframe keyframe;
        keyframe.stream_epoch = connection.stream_epoch();
        keyframe.stream_sequence = connection.next_stream_sequence();
        keyframe.snapshot_id = latest_snapshot_id_;
        keyframe.baseline_snapshot_id = 0;
        keyframe.tick = tick;
        keyframe.simulation_time_us =
            tick * static_cast<std::uint64_t>(config_.tick_dt_microseconds);

        std::vector<SentOp> ops;
        std::unordered_map<foundation::WorldId, TrackedEntity,
                           foundation::WorldIdHash> next_view;
        for (const foundation::WorldId wid : visible) {
            const auto state = global_view_.find(wid);
            if (state == global_view_.end()) {
                continue;
            }
            // 新 epoch 按 WID 升序分配 FID；已有映射在 epoch 内保持不变。
            foundation::FeatureId fid;
            const auto mapped = connection.fid_of(wid);
            if (mapped.has_value()) {
                fid = *mapped;
            } else {
                fid = connection.allocate_fid();
                connection.map_fid(wid, fid);
            }
            EntityEnter enter;
            enter.wid = wid;
            enter.fid = fid;
            enter.entity = *state->second.projected;
            enter.entity.fid = fid;
            keyframe.entities.push_back(std::move(enter));

            TrackedEntity tracked;
            tracked.fid = fid;
            tracked.entity_hash = state->second.hash;
            tracked.frequency = state->second.projected->metadata.frequency;
            tracked.last_sent_tick = tick;
            next_view.emplace(wid, tracked);
            ops.push_back(SentOp{SentOp::Kind::enter, wid, fid, state->second.hash});
        }
        // keyframe 替换整个视图：已确认但不再可见的实体以 leave 操作记录推进 ack。
        for (const auto& [wid, tracked] : acked) {
            if (!visible_contains(wid)) {
                ops.push_back(SentOp{SentOp::Kind::leave, wid, tracked.fid, 0});
            }
        }
        std::sort(ops.begin(), ops.end(), [](const SentOp& lhs, const SentOp& rhs) {
            return lhs.wid < rhs.wid;
        });

        connection.current_view() = std::move(next_view);
        connection.clear_acked_dirty();
        throttle_pending.clear();
        connection.set_needs_keyframe(false);
        connection.set_last_keyframe_tick(tick);
        // 安排下一次周期 keyframe：周期到期保持 interval 间隔，连接的相位偏移
        // 由首次错峰永久保持；强制 keyframe（join/淘汰/策略/epoch）之后的首个
        // 周期 keyframe 重新按连接相位错峰，避免整批连接同 tick 尖峰。
        const std::uint64_t interval_ticks_value = keyframe_interval_ticks();
        const std::uint64_t phase = forced_keyframe
            ? static_cast<std::uint64_t>(ConnectionIdHash{}(connection.id()))
                  % interval_ticks_value
            : 0;
        connection.set_next_keyframe_tick(tick + interval_ticks_value - phase);
        connection.set_policy_version(policy_.version());
        connection.record_sent(latest_snapshot_id_, std::move(ops));
        connection.advance_stream_sequence();
        if (connection.evict_unacked(config_.max_unacked_frames,
                                     config_.max_unacked_bytes)) {
            connection.set_needs_keyframe(true);
        }
        return StateFrame{std::move(keyframe)};
    }

    // Delta 相对于已确认基线计算；update 携带完整最新投影。
    Delta delta;
    delta.stream_epoch = connection.stream_epoch();
    delta.stream_sequence = connection.next_stream_sequence();
    delta.snapshot_id = latest_snapshot_id_;
    delta.baseline_snapshot_id = connection.acked_baseline();
    delta.tick = tick;
    delta.simulation_time_us =
        tick * static_cast<std::uint64_t>(config_.tick_dt_microseconds);

    // 本帧操作的视图落点先记入小表，避免整表复制 acked 视图；
    // frame_ops/frame_leaves 与旧 next_view 的写入集合一一对应。
    std::vector<SentOp> ops;
    std::unordered_map<foundation::WorldId, TrackedEntity,
                       foundation::WorldIdHash> frame_ops;
    std::unordered_map<foundation::WorldId, foundation::FeatureId,
                       foundation::WorldIdHash> frame_leaves;
    const auto& previous_view = connection.current_view();

    // 频率暂缓集合每帧重建：旧集合作为候选来源，本帧再次被跳过的重新记入。
    std::unordered_set<foundation::WorldId, foundation::WorldIdHash> pending_candidates;
    pending_candidates.swap(throttle_pending);

    const auto push_leave = [this, &connection, &world, &bounds, &delta, &ops,
                             &frame_leaves](foundation::WorldId wid,
                                            const TrackedEntity& tracked) {
        frame_leaves.emplace(wid, tracked.fid);
        LeaveReason reason = LeaveReason::out_of_area;
        if (world.find(wid) == nullptr) {
            reason = LeaveReason::destroyed;
        } else if (bounds.has_value() && relevance_filter_) {
            const world::WorldObject* object = world.find(wid);
            if (object != nullptr && !relevance_filter_(connection.id(), *object)) {
                reason = LeaveReason::not_relevant;
            }
        }
        delta.leaves.push_back(EntityLeave{wid, tracked.fid, reason});
        ops.push_back(SentOp{SentOp::Kind::leave, wid, tracked.fid, 0});
    };

    const auto process_visible_wid = [this, &connection, &acked, &previous_view, &delta,
                                      &ops, &frame_ops, &throttle_pending, tick](
                                         foundation::WorldId wid) {
        const auto state = global_view_.find(wid);
        if (state == global_view_.end()) {
            return;
        }
        const std::uint64_t hash = state->second.hash;
        const auto acked_entry = acked.find(wid);
        if (acked_entry == acked.end()) {
            // 重新进入可见集的实体沿用 epoch 内既有 FID，不分配新值。
            foundation::FeatureId fid;
            const auto mapped = connection.fid_of(wid);
            if (mapped.has_value()) {
                fid = *mapped;
            } else {
                fid = connection.allocate_fid();
                connection.map_fid(wid, fid);
            }
            EntityEnter enter;
            enter.wid = wid;
            enter.fid = fid;
            enter.entity = *state->second.projected;
            enter.entity.fid = fid;
            delta.enters.push_back(std::move(enter));

            TrackedEntity tracked;
            tracked.fid = fid;
            tracked.entity_hash = hash;
            tracked.frequency = state->second.projected->metadata.frequency;
            tracked.last_sent_tick = tick;
            frame_ops.emplace(wid, tracked);
            ops.push_back(SentOp{SentOp::Kind::enter, wid, fid, hash});
            return;
        }
        if (acked_entry->second.entity_hash == hash) {
            return;
        }

        const FrequencyClass frequency = state->second.projected->metadata.frequency;
        std::uint64_t last_sent = 0;
        const auto previous = previous_view.find(wid);
        if (previous != previous_view.end()) {
            last_sent = previous->second.last_sent_tick;
        }
        const std::uint32_t hz = frequency == FrequencyClass::fast
            ? config_.data_frequency_hz
            : (frequency == FrequencyClass::slow ? config_.slow_frequency_hz
                                                 : config_.data_frequency_hz);
        if (frequency != FrequencyClass::on_change
            && tick - last_sent < interval_ticks(hz)) {
            throttle_pending.insert(wid);
            return;
        }

        EntityUpdate update;
        update.wid = wid;
        update.fid = acked_entry->second.fid;
        update.entity = *state->second.projected;
        update.entity.fid = acked_entry->second.fid;
        delta.updates.push_back(std::move(update));

        TrackedEntity tracked = acked_entry->second;
        tracked.entity_hash = hash;
        tracked.frequency = frequency;
        tracked.last_sent_tick = tick;
        frame_ops.emplace(wid, tracked);
        ops.push_back(SentOp{SentOp::Kind::update, wid, tracked.fid, hash});
    };

    if (visible_from_cache) {
        // 缓存命中意味着本 tick 无位置/创建/销毁变化，可见集与上一帧相同。
        // 可能产生操作的 WID 必属于：未确认操作覆盖集、本 tick 规范化变化集 ∩ 可见集、
        // 频率暂缓集；对其余可见实体全量循环必然逐项跳过，结果一致。
        std::vector<foundation::WorldId> candidates;
        candidates.reserve(connection.unacked_wid_counts().size()
                           + pending_candidates.size());
        for (const auto& [wid, count] : connection.unacked_wid_counts()) {
            static_cast<void>(count);
            candidates.push_back(wid);
        }
        for (const foundation::WorldId wid : pending_candidates) {
            candidates.push_back(wid);
        }
        auto changed = tick_changed_wids_.begin();
        auto seen = visible.begin();
        while (changed != tick_changed_wids_.end() && seen != visible.end()) {
            if (*changed < *seen) {
                ++changed;
            } else if (*seen < *changed) {
                ++seen;
            } else {
                candidates.push_back(*changed);
                ++changed;
                ++seen;
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());

        for (const foundation::WorldId wid : candidates) {
            if (visible_contains(wid)) {
                process_visible_wid(wid);
                continue;
            }
            const auto acked_entry = acked.find(wid);
            if (acked_entry != acked.end()) {
                push_leave(wid, acked_entry->second);
            }
        }
    } else {
        for (const foundation::WorldId wid : visible) {
            process_visible_wid(wid);
        }
        for (const auto& [wid, tracked] : acked) {
            if (!visible_contains(wid)) {
                push_leave(wid, tracked);
            }
        }
    }

    if (delta.enters.empty() && delta.updates.empty() && delta.leaves.empty()) {
        return std::nullopt;
    }

    // 带宽饱和时按 (优先级降序, 等待时间降序) 保留配额内 update，避免低优先级饥饿。
    if (delta.updates.size() > config_.max_updates_per_frame) {
        std::sort(delta.updates.begin(), delta.updates.end(),
            [tick](const EntityUpdate& lhs, const EntityUpdate& rhs) {
                if (lhs.entity.metadata.priority != rhs.entity.metadata.priority) {
                    return lhs.entity.metadata.priority > rhs.entity.metadata.priority;
                }
                return lhs.fid < rhs.fid;
            });
        delta.updates.resize(config_.max_updates_per_frame);
        // 被裁掉的 update 不能进入已发送记录，恢复 acked 视图中的旧 hash。
        std::sort(ops.begin(), ops.end(), [](const SentOp& lhs, const SentOp& rhs) {
            return lhs.wid < rhs.wid;
        });
        std::unordered_map<std::uint32_t, foundation::WorldId> kept;
        for (const EntityUpdate& update : delta.updates) {
            kept.emplace(update.fid.value, update.wid);
        }
        std::erase_if(ops, [&kept, &frame_ops, &acked](const SentOp& op) {
            if (op.kind != SentOp::Kind::update) {
                return false;
            }
            if (kept.contains(op.fid.value)) {
                return false;
            }
            frame_ops[op.wid] = acked.at(op.wid);
            return true;
        });
    }

    // 输出顺序冻结：leave(FID)、enter(WID)、update(FID) 各自升序。
    std::sort(delta.leaves.begin(), delta.leaves.end(),
        [](const EntityLeave& lhs, const EntityLeave& rhs) { return lhs.fid < rhs.fid; });
    std::sort(delta.enters.begin(), delta.enters.end(),
        [](const EntityEnter& lhs, const EntityEnter& rhs) { return lhs.wid < rhs.wid; });
    std::sort(delta.updates.begin(), delta.updates.end(),
        [](const EntityUpdate& lhs, const EntityUpdate& rhs) { return lhs.fid < rhs.fid; });

    // current_view 就地收敛到「acked ∪ 本帧操作」，与整表拷贝语义逐字节一致：
    // 1) ack 推进过的 WID 采用新的 acked 值（含 acked 侧的 last_sent_tick）；
    // 2) 有未确认操作且本帧未重发的 WID 回滚到 acked 值，acked 缺失则移除；
    // 3) 本帧操作覆盖落点。
    auto& view = connection.current_view();
    for (const foundation::WorldId wid : connection.acked_dirty()) {
        if (frame_ops.contains(wid) || frame_leaves.contains(wid)
            || connection.unacked_wid_counts().contains(wid)) {
            continue;
        }
        const auto acked_entry = acked.find(wid);
        if (acked_entry == acked.end()) {
            view.erase(wid);
        } else {
            view[wid] = acked_entry->second;
        }
    }
    connection.clear_acked_dirty();
    for (const auto& [wid, count] : connection.unacked_wid_counts()) {
        static_cast<void>(count);
        if (frame_ops.contains(wid) || frame_leaves.contains(wid)) {
            continue;
        }
        const auto acked_entry = acked.find(wid);
        if (acked_entry == acked.end()) {
            view.erase(wid);
        } else {
            view[wid] = acked_entry->second;
        }
    }
    for (const auto& [wid, tracked] : frame_ops) {
        view[wid] = tracked;
    }
    for (const auto& [wid, fid] : frame_leaves) {
        static_cast<void>(fid);
        view.erase(wid);
    }

    connection.record_sent(latest_snapshot_id_, std::move(ops));
    connection.advance_stream_sequence();
    if (connection.evict_unacked(config_.max_unacked_frames,
                                 config_.max_unacked_bytes)) {
        connection.set_needs_keyframe(true);
    }
    return StateFrame{std::move(delta)};
}

} // namespace geoworld::projection
