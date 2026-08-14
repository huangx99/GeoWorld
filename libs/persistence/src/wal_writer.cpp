#include "geoworld/persistence/wal.hpp"

#include "geoworld/persistence/manifest.hpp"

#include "wal_codec.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace geoworld::persistence {

struct AppendTicket::State {
    std::mutex mutex;
    std::condition_variable ready;
    bool done{false};
    AppendOutcome outcome{};
};

AppendOutcome AppendTicket::wait() const {
    if (!state_) {
        return {};
    }
    std::unique_lock lock(state_->mutex);
    state_->ready.wait(lock, [&] { return state_->done; });
    return state_->outcome;
}

std::optional<AppendOutcome> AppendTicket::wait_for(std::chrono::milliseconds timeout) const {
    if (!state_) {
        return AppendOutcome{};
    }
    std::unique_lock lock(state_->mutex);
    if (!state_->ready.wait_for(lock, timeout, [&] { return state_->done; })) {
        return std::nullopt;
    }
    return state_->outcome;
}

bool AppendTicket::pending() const {
    if (!state_) {
        return false;
    }
    std::lock_guard lock(state_->mutex);
    return !state_->done;
}

namespace {

void fulfill_ticket(const std::shared_ptr<AppendTicket::State>& state, AppendOutcome outcome) {
    {
        std::lock_guard lock(state->mutex);
        state->outcome = outcome;
        state->done = true;
    }
    state->ready.notify_all();
}

struct PendingAppend {
    WalRecord record;
    std::shared_ptr<AppendTicket::State> ticket;
};

} // namespace

struct WalWriter::Impl {
    Impl(WalConfig config, std::shared_ptr<FileOps> file_ops)
        : config(std::move(config)),
          ops(file_ops ? std::move(file_ops) : make_posix_file_ops()) {}

    WalConfig config;
    std::shared_ptr<FileOps> ops;
    DurableLayout layout{};

    std::mutex mutex;
    std::condition_variable queue_ready;
    std::deque<PendingAppend> queue;
    std::size_t queue_payload_bytes{0};
    bool stopping{false};
    bool started{false};
    std::atomic<bool> fault{false};
    std::atomic<bool> lsn_exhausted{false};
    std::atomic<std::uint64_t> last_durable{0};
    std::thread worker;

    // 以下状态仅 writer 线程访问：LSN 唯一分配与 segment 写入都序列化在该线程。
    std::uint64_t next_lsn_value{kFirstLsn.value};
    std::unique_ptr<WritableFile> active_segment;
    std::filesystem::path active_path;
    Lsn active_first_lsn{};
    std::uint64_t active_bytes{0};
    std::chrono::steady_clock::time_point active_opened_at{};
    bool unsynced_writes{false};

    [[nodiscard]] Status start() {
        if (!config.world.valid() || !config.branch.valid() || config.durable_root.empty()
            || config.group_commit_max_records == 0 || config.group_commit_max_bytes == 0
            || config.queue_max_records == 0 || config.queue_max_bytes == 0
            || config.max_record_bytes == 0
            || config.max_record_bytes > config.segment_max_bytes) {
            return {PersistenceError::config_invalid};
        }
        layout = make_durable_layout(config.durable_root, config.world, config.branch);
        PersistenceError error = ops->create_directories(layout.wal_dir());
        if (error == PersistenceError::none) {
            error = ops->create_directories(layout.manifest_dir());
        }
        if (error == PersistenceError::none) {
            error = ops->create_directories(layout.checkpoint_dir());
        }
        if (error != PersistenceError::none) {
            return {error};
        }
        // 目录 manifest 冻结身份：缺失则原子发布，存在则必须匹配世界与分支。
        const std::filesystem::path manifest_path = layout.directory_manifest_path();
        auto manifest = load_directory_manifest(*ops, manifest_path);
        if (!manifest.ok()) {
            if (manifest.error != PersistenceError::not_found) {
                return {manifest.error};
            }
            DirectoryManifest identity;
            identity.world = config.world;
            identity.branch = config.branch;
            error = publish_directory_manifest(*ops, manifest_path, identity);
            if (error != PersistenceError::none) {
                return {error};
            }
        } else if (manifest.value.world != config.world || manifest.value.branch != config.branch) {
            return {PersistenceError::config_invalid};
        }
        // 启动恢复：修剪最后一个活跃 segment 的断电尾部，恢复 LSN 分配位置。
        WalScanResult scan =
            scan_wal_directory(layout.wal_dir(), *ops, TailPolicy::trim_active_tail,
                               kFirstLsn, config.max_record_bytes);
        if (!scan.ok()) {
            return {scan.error};
        }
        if (scan.last_lsn.valid()) {
            const std::optional<Lsn> next = next_lsn(scan.last_lsn);
            if (!next.has_value()) {
                lsn_exhausted.store(true);
            } else {
                next_lsn_value = next->value;
            }
            last_durable.store(scan.last_lsn.value);
        }
        if (!scan.active_segment.empty()) {
            auto opened = ops->open_append(scan.active_segment);
            if (!opened.ok()) {
                return {opened.error};
            }
            active_segment = std::move(opened.value);
            active_path = scan.active_segment;
            const std::string file_name = active_path.filename().string();
            active_first_lsn =
                parse_segment_file_name(file_name.substr(
                                            0, file_name.size() - kActiveSegmentSuffix.size()))
                    .value_or(Lsn{next_lsn_value});
            active_bytes = active_segment->size();
            active_opened_at = std::chrono::steady_clock::now();
            if (active_bytes == 0) {
                // 断电留下的空活跃段：重写头部，首 LSN 与文件名保持一致。
                const std::vector<std::byte> header =
                    detail::encode_segment_header(Lsn{next_lsn_value});
                error = active_segment->write(header);
                if (error != PersistenceError::none) {
                    return {error};
                }
                active_bytes = header.size();
            }
            unsynced_writes = false;
        }
        started = true;
        worker = std::thread([this] { run(); });
        return {};
    }

    [[nodiscard]] Result<AppendTicket> append(WalRecord record) {
        if (record.payload.size() > config.max_record_bytes) {
            return {{}, PersistenceError::record_invalid};
        }
        if (record.durability == Durability::relaxed && !config.allow_relaxed) {
            return {{}, PersistenceError::relaxed_not_allowed};
        }
        if (lsn_exhausted.load()) {
            return {{}, PersistenceError::lsn_overflow};
        }
        auto state = std::make_shared<AppendTicket::State>();
        {
            std::lock_guard lock(mutex);
            if (!started || stopping) {
                return {{}, PersistenceError::shutting_down};
            }
            if (fault.load()) {
                return {{}, PersistenceError::fault_read_only};
            }
            // 队列健康但积圧达到上限：以稳定错误码显式拒绝，客户端可重试。
            if (queue.size() >= config.queue_max_records
                || queue_payload_bytes + record.payload.size() > config.queue_max_bytes) {
                return {{}, PersistenceError::queue_full};
            }
            queue_payload_bytes += record.payload.size();
            queue.push_back(PendingAppend{std::move(record), state});
        }
        queue_ready.notify_one();
        return {AppendTicket{std::move(state)}, PersistenceError::none};
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            started = false;
        }
        queue_ready.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void run() {
        std::vector<PendingAppend> batch;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                queue_ready.wait(lock, [&] { return !queue.empty() || stopping; });
                if (queue.empty() && stopping) {
                    break;
                }
                // 组提交三约束：命名配置的最大记录数、最大字节数、最大等待时间。
                const auto deadline =
                    std::chrono::steady_clock::now() + config.group_commit_max_wait;
                std::size_t batch_bytes = 0;
                while (!queue.empty()) {
                    if (batch.size() >= config.group_commit_max_records) {
                        break;
                    }
                    if (!batch.empty()
                        && batch_bytes + queue.front().record.payload.size()
                               > config.group_commit_max_bytes) {
                        break;
                    }
                    batch_bytes += queue.front().record.payload.size();
                    queue_payload_bytes -= queue.front().record.payload.size();
                    batch.push_back(std::move(queue.front()));
                    queue.pop_front();
                    if (batch.size() >= config.group_commit_max_records || stopping) {
                        break;
                    }
                    if (queue.empty()) {
                        queue_ready.wait_until(lock, deadline,
                                               [&] { return !queue.empty() || stopping; });
                        if (queue.empty() || std::chrono::steady_clock::now() >= deadline) {
                            break;
                        }
                    }
                }
            }
            process_batch(batch);
            batch.clear();
        }
        // shutdown 边界：队列已排空，尽力把 relaxed 残留写入同步落盘。
        if (active_segment && unsynced_writes && !fault.load()) {
            active_segment->sync();
        }
        active_segment.reset();
    }

    // 以下函数只在 writer 线程执行。

    [[nodiscard]] PersistenceError open_new_segment(Lsn first_lsn) {
        active_path = layout.wal_dir()
                      / (segment_file_name(first_lsn) + std::string{kActiveSegmentSuffix});
        auto created = ops->create_exclusive(active_path);
        if (!created.ok()) {
            return created.error;
        }
        active_segment = std::move(created.value);
        active_first_lsn = first_lsn;
        const std::vector<std::byte> header = detail::encode_segment_header(first_lsn);
        const PersistenceError error = active_segment->write(header);
        if (error != PersistenceError::none) {
            active_segment.reset();
            return error;
        }
        active_bytes = header.size();
        active_opened_at = std::chrono::steady_clock::now();
        unsynced_writes = true;
        return PersistenceError::none;
    }

    [[nodiscard]] PersistenceError rotate_segment(Lsn next_first_lsn) {
        // 关闭顺序冻结：sync -> rename 去掉 active 后缀 -> 父目录 sync。
        PersistenceError error = active_segment->sync();
        if (error == PersistenceError::none) {
            const std::filesystem::path closed_path =
                layout.wal_dir()
                / (segment_file_name(active_first_lsn) + std::string{kSegmentExtension});
            error = ops->rename_file(active_path, closed_path);
        }
        if (error == PersistenceError::none) {
            error = ops->sync_directory(layout.wal_dir());
        }
        if (error != PersistenceError::none) {
            return error;
        }
        active_segment.reset();
        return open_new_segment(next_first_lsn);
    }

    [[nodiscard]] PersistenceError ensure_segment_for(Lsn lsn, std::uint64_t record_bytes) {
        if (!active_segment) {
            return open_new_segment(lsn);
        }
        const bool has_records = active_bytes > kSegmentHeaderBytes;
        const bool size_exceeded = active_bytes + record_bytes > config.segment_max_bytes;
        const bool age_exceeded =
            std::chrono::steady_clock::now() - active_opened_at > config.segment_max_age;
        if (has_records && (size_exceeded || age_exceeded)) {
            return rotate_segment(lsn);
        }
        return PersistenceError::none;
    }

    void enter_fault() {
        fault.store(true);
        std::deque<PendingAppend> rejected;
        {
            std::lock_guard lock(mutex);
            rejected.swap(queue);
            queue_payload_bytes = 0;
        }
        // 已进入只读故障状态：拒绝全部在途 durable 输入，不降级为内存接纳。
        for (auto& pending : rejected) {
            fulfill_ticket(pending.ticket,
                           {PersistenceError::fault_read_only, {}, AdmissionStatus::none});
        }
        active_segment.reset();
    }

    void process_batch(std::vector<PendingAppend>& batch) {
        if (batch.empty()) {
            return;
        }
        if (fault.load()) {
            for (auto& pending : batch) {
                fulfill_ticket(pending.ticket,
                               {PersistenceError::fault_read_only, {}, AdmissionStatus::none});
            }
            return;
        }
        std::vector<std::pair<std::shared_ptr<AppendTicket::State>, Lsn>> written;
        written.reserve(batch.size());
        bool any_durable = false;
        PersistenceError error = PersistenceError::none;
        std::size_t failed_index = batch.size();
        for (std::size_t index = 0; index < batch.size(); ++index) {
            PendingAppend& pending = batch[index];
            const Lsn lsn{next_lsn_value};
            const std::vector<std::byte> encoded =
                detail::encode_record(lsn, pending.record.kind, pending.record.target_tick,
                                      pending.record.payload);
            error = ensure_segment_for(lsn, encoded.size());
            if (error == PersistenceError::none) {
                error = active_segment->write(encoded);
            }
            if (error != PersistenceError::none) {
                failed_index = index;
                break;
            }
            active_bytes += encoded.size();
            unsynced_writes = true;
            written.emplace_back(pending.ticket, lsn);
            if (pending.record.durability == Durability::durable) {
                any_durable = true;
            }
            const std::optional<Lsn> next = next_lsn(lsn);
            if (!next.has_value()) {
                lsn_exhausted.store(true);
            } else {
                next_lsn_value = next->value;
            }
        }
        if (error == PersistenceError::none && any_durable) {
            // fdatasync 成功才完成 durable promise；relaxed 批次不强制 sync。
            error = active_segment->sync();
            if (error == PersistenceError::none) {
                unsynced_writes = false;
            }
        }
        if (error != PersistenceError::none) {
            // 磁盘满/权限/同步失败：进入只读故障状态。已写未 sync 的记录不得报告 durable。
            for (auto& [ticket, lsn] : written) {
                fulfill_ticket(ticket, {error, {}, AdmissionStatus::none});
            }
            for (std::size_t index = failed_index; index < batch.size(); ++index) {
                fulfill_ticket(batch[index].ticket,
                               {error, {}, AdmissionStatus::none});
            }
            enter_fault();
            return;
        }
        for (std::size_t index = 0; index < batch.size(); ++index) {
            const bool relaxed = batch[index].record.durability == Durability::relaxed;
            fulfill_ticket(batch[index].ticket,
                           {PersistenceError::none, written[index].second,
                            relaxed ? AdmissionStatus::relaxed_accepted
                                    : AdmissionStatus::durable_accepted});
        }
        if (any_durable) {
            last_durable.store(written.back().second.value);
        }
    }
};

WalWriter::WalWriter(WalConfig config, std::shared_ptr<FileOps> file_ops)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(file_ops))) {}

WalWriter::~WalWriter() {
    shutdown();
}

Status WalWriter::start() {
    return impl_->start();
}

Result<AppendTicket> WalWriter::append(WalRecord record) {
    return impl_->append(std::move(record));
}

Lsn WalWriter::last_durable_lsn() const noexcept {
    return Lsn{impl_->last_durable.load()};
}

bool WalWriter::faulted() const noexcept {
    return impl_->fault.load();
}

void WalWriter::shutdown() {
    impl_->shutdown();
}

} // namespace geoworld::persistence
