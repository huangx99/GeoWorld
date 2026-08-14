#include "geoworld/persistence/durable_log.hpp"
#include "geoworld/persistence/manifest.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/types.hpp"
#include "geoworld/persistence/wal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using geoworld::foundation::WorldId;
using geoworld::persistence::AppendTicket;
using geoworld::persistence::BranchId;
using geoworld::persistence::Durability;
using geoworld::persistence::FileOps;
using geoworld::persistence::Lsn;
using geoworld::persistence::PersistenceError;
using geoworld::persistence::TailPolicy;
using geoworld::persistence::WalConfig;
using geoworld::persistence::WalRecord;
using geoworld::persistence::WalRecordKind;
using geoworld::persistence::WalWriter;
using geoworld::persistence::WritableFile;

std::atomic<int> g_dir_counter{0};

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(std::string_view name) {
        path = std::filesystem::temp_directory_path()
               / ("gw-m5a-" + std::string{name} + "-" + std::to_string(::getpid()) + "-"
                  + std::to_string(++g_dir_counter));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

[[nodiscard]] std::uint16_t read_le16(const std::byte* in) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0])
                                      | static_cast<std::uint16_t>(
                                            static_cast<std::uint16_t>(in[1]) << 8U));
}

[[nodiscard]] std::uint32_t read_le32(const std::byte* in) {
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(in[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_le64(const std::byte* in) {
    std::uint64_t value = 0;
    for (std::uint64_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(in[index]) << (index * 8U);
    }
    return value;
}

void append_le32(std::vector<std::byte>& out, std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4; ++index) {
        out.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

[[nodiscard]] std::vector<std::byte> payload_of(std::string_view text) {
    std::vector<std::byte> payload(text.size());
    std::memcpy(payload.data(), text.data(), text.size());
    return payload;
}

[[nodiscard]] WalRecord make_record(WalRecordKind kind, std::uint64_t tick,
                                    std::vector<std::byte> payload,
                                    Durability durability = Durability::durable) {
    WalRecord record;
    record.kind = kind;
    record.target_tick = tick;
    record.payload = std::move(payload);
    record.durability = durability;
    return record;
}

[[nodiscard]] WalConfig make_config(const std::filesystem::path& root) {
    WalConfig config;
    config.durable_root = root;
    config.world = WorldId{42};
    config.branch =
        geoworld::persistence::parse_branch_id("01234567-89ab-cdef-0123-456789abcdef")
            .value_or(BranchId{});
    return config;
}

[[nodiscard]] std::filesystem::path find_file_with_suffix(const std::filesystem::path& dir,
                                                          std::string_view suffix) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.size() >= suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return entry.path();
        }
    }
    return {};
}

// 计数与故障注入后端：包装真实 POSIX 实现，用于组提交计数、磁盘满/同步失败与延迟注入。
class CountingOps final : public FileOps {
public:
    explicit CountingOps(std::shared_ptr<FileOps> inner) : inner_(std::move(inner)) {}

    std::atomic<long long> file_syncs{0};
    std::atomic<long long> file_writes{0};
    std::atomic<bool> fail_writes{false};
    std::atomic<bool> fail_syncs{false};
    std::atomic<bool> fail_renames{false};
    std::atomic<long long> sync_delay_ms{0};

    [[nodiscard]] bool wait_sync_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(latch_mutex_);
        return latch_cv_.wait_for(lock, timeout, [&] { return sync_entered_; });
    }

    void reset_sync_latch() {
        std::lock_guard lock(latch_mutex_);
        sync_entered_ = false;
    }

    geoworld::persistence::Result<std::unique_ptr<WritableFile>> open_append(
        const std::filesystem::path& path) override {
        auto opened = inner_->open_append(path);
        if (opened.ok()) {
            opened.value = std::make_unique<CountingFile>(this, std::move(opened.value));
        }
        return opened;
    }

    geoworld::persistence::Result<std::unique_ptr<WritableFile>> create_exclusive(
        const std::filesystem::path& path) override {
        auto created = inner_->create_exclusive(path);
        if (created.ok()) {
            created.value = std::make_unique<CountingFile>(this, std::move(created.value));
        }
        return created;
    }

    [[nodiscard]] PersistenceError rename_file(const std::filesystem::path& from,
                                               const std::filesystem::path& to) override {
        if (fail_renames.load()) {
            return PersistenceError::io_failure;
        }
        return inner_->rename_file(from, to);
    }

    [[nodiscard]] PersistenceError sync_directory(const std::filesystem::path& dir) override {
        return inner_->sync_directory(dir);
    }

    [[nodiscard]] PersistenceError truncate_file(const std::filesystem::path& path,
                                                 std::uint64_t size) override {
        return inner_->truncate_file(path, size);
    }

    geoworld::persistence::Result<std::vector<std::byte>> read_file(
        const std::filesystem::path& path) override {
        return inner_->read_file(path);
    }

    geoworld::persistence::Result<std::vector<std::filesystem::path>> list_files(
        const std::filesystem::path& dir) override {
        return inner_->list_files(dir);
    }

    [[nodiscard]] PersistenceError remove_file(const std::filesystem::path& path) override {
        return inner_->remove_file(path);
    }

    [[nodiscard]] PersistenceError create_directories(const std::filesystem::path& dir) override {
        return inner_->create_directories(dir);
    }

private:
    class CountingFile final : public WritableFile {
    public:
        CountingFile(CountingOps* owner, std::unique_ptr<WritableFile> inner)
            : owner_(owner), inner_(std::move(inner)) {}

        [[nodiscard]] PersistenceError write(std::span<const std::byte> data) noexcept override {
            owner_->file_writes.fetch_add(1);
            if (owner_->fail_writes.load()) {
                return PersistenceError::no_space_or_permission;
            }
            return inner_->write(data);
        }

        [[nodiscard]] PersistenceError sync() noexcept override {
            owner_->file_syncs.fetch_add(1);
            {
                std::lock_guard lock(owner_->latch_mutex_);
                owner_->sync_entered_ = true;
            }
            owner_->latch_cv_.notify_all();
            const long long delay = owner_->sync_delay_ms.load();
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{delay});
            }
            if (owner_->fail_syncs.load()) {
                return PersistenceError::sync_failed;
            }
            return inner_->sync();
        }

        [[nodiscard]] std::uint64_t size() const noexcept override { return inner_->size(); }

    private:
        CountingOps* owner_;
        std::unique_ptr<WritableFile> inner_;
    };

    std::shared_ptr<FileOps> inner_;
    std::mutex latch_mutex_;
    std::condition_variable latch_cv_;
    bool sync_entered_{false};
};

[[nodiscard]] bool lsn_and_id_rules() {
    if (geoworld::persistence::kFirstLsn.value != 1 || Lsn{0}.valid()) {
        return false;
    }
    if (geoworld::persistence::next_lsn(Lsn{0}).has_value()
        || geoworld::persistence::next_lsn(Lsn{UINT64_MAX}).has_value()) {
        return false;
    }
    const auto next = geoworld::persistence::next_lsn(Lsn{7});
    if (!next.has_value() || next->value != 8) {
        return false;
    }
    const std::string name = geoworld::persistence::segment_file_name(Lsn{42});
    if (name != "seg-00000000000000000042") {
        return false;
    }
    const auto parsed = geoworld::persistence::parse_segment_file_name(name);
    if (!parsed.has_value() || *parsed != Lsn{42}) {
        return false;
    }
    if (geoworld::persistence::parse_segment_file_name("seg-42").has_value()
        || geoworld::persistence::parse_segment_file_name("seg-00000000000000000000")
               .has_value()
        || geoworld::persistence::parse_segment_file_name("other-file").has_value()) {
        return false;
    }
    const BranchId generated = geoworld::persistence::generate_branch_id();
    if (!generated.valid()) {
        return false;
    }
    const std::string text = geoworld::persistence::format_branch_id(generated);
    const auto roundtrip = geoworld::persistence::parse_branch_id(text);
    if (!roundtrip.has_value() || *roundtrip != generated) {
        return false;
    }
    if (geoworld::persistence::parse_branch_id("not-a-uuid").has_value()) {
        return false;
    }
    using namespace std::string_view_literals;
    if (geoworld::persistence::error_code(PersistenceError::queue_full)
            != geoworld::persistence::error_queue_full
        || geoworld::persistence::error_code(PersistenceError::fault_read_only)
               != geoworld::persistence::error_fault_read_only
        || geoworld::persistence::error_code(PersistenceError::none)[0] != '\0') {
        return false;
    }
    return true;
}

[[nodiscard]] bool record_bytes_deterministic_and_layout() {
    TempDir first_dir("codec-a");
    TempDir second_dir("codec-b");
    const std::vector<std::byte> payload = payload_of("abc");
    for (const std::filesystem::path* dir : {&first_dir.path, &second_dir.path}) {
        WalWriter writer(make_config(*dir));
        if (!writer.start().ok()) {
            return false;
        }
        auto ticket = writer.append(
            make_record(WalRecordKind::external_command, 7, payload));
        if (!ticket.ok() || !ticket.value.wait().ok()) {
            return false;
        }
        writer.shutdown();
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    const auto first_layout = geoworld::persistence::make_durable_layout(
        first_dir.path, WorldId{42}, make_config(first_dir.path).branch);
    const auto second_layout = geoworld::persistence::make_durable_layout(
        second_dir.path, WorldId{42}, make_config(first_dir.path).branch);
    const auto first_bytes =
        ops->read_file(find_file_with_suffix(first_layout.wal_dir(), ".active"));
    const auto second_bytes =
        ops->read_file(find_file_with_suffix(second_layout.wal_dir(), ".active"));
    if (!first_bytes.ok() || !second_bytes.ok()) {
        return false;
    }
    // 相同记录与版本必须产生逐字节一致的分帧结果。
    if (first_bytes.value != second_bytes.value) {
        return false;
    }
    const std::vector<std::byte>& bytes = first_bytes.value;
    constexpr std::size_t header = 16;
    constexpr std::size_t record_total = 4 + 24 + 3 + 4;
    if (bytes.size() != header + record_total) {
        return false;
    }
    if (std::memcmp(bytes.data(), "GWLS", 4) != 0 || read_le32(bytes.data() + 4) != 1
        || read_le64(bytes.data() + 8) != 1) {
        return false;
    }
    const std::byte* record = bytes.data() + header;
    if (read_le32(record) != 24 + 3 + 4 || read_le16(record + 4) != 1
        || read_le16(record + 6) != 0 || read_le64(record + 8) != 1
        || read_le64(record + 16) != 7 || read_le32(record + 24) != 3
        || std::memcmp(record + 28, "abc", 3) != 0) {
        return false;
    }
    return true;
}

[[nodiscard]] bool durable_append_scan_and_restart() {
    TempDir dir("append");
    const WalConfig config = make_config(dir.path);
    {
        WalWriter writer(config);
        if (!writer.start().ok()) {
            return false;
        }
        for (std::uint64_t index = 0; index < 3; ++index) {
            auto ticket = writer.append(make_record(WalRecordKind::normalized_input,
                                                    100 + index, payload_of("rec")));
            if (!ticket.ok()) {
                return false;
            }
            const auto outcome = ticket.value.wait();
            if (!outcome.ok() || outcome.lsn != Lsn{index + 1}
                || outcome.status
                       != geoworld::persistence::AdmissionStatus::durable_accepted) {
                return false;
            }
        }
        if (writer.last_durable_lsn() != Lsn{3}) {
            return false;
        }
        writer.shutdown();
    }
    // 重启：扫描恢复 LSN 分配位置，新记录接续 4。
    {
        WalWriter writer(config);
        if (!writer.start().ok()) {
            return false;
        }
        auto ticket = writer.append(
            make_record(WalRecordKind::state_hash, 200, payload_of("hash")));
        if (!ticket.ok() || ticket.value.wait().lsn != Lsn{4}) {
            return false;
        }
        writer.shutdown();
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    const auto layout =
        geoworld::persistence::make_durable_layout(dir.path, config.world, config.branch);
    const auto scan = geoworld::persistence::scan_wal_directory(layout.wal_dir(), *ops,
                                                                TailPolicy::strict);
    if (!scan.ok() || scan.records.size() != 4 || scan.last_lsn != Lsn{4}) {
        return false;
    }
    for (std::size_t index = 0; index < scan.records.size(); ++index) {
        if (scan.records[index].lsn != Lsn{index + 1}) {
            return false;
        }
    }
    return scan.records[0].kind == WalRecordKind::normalized_input
           && scan.records[0].target_tick == 100
           && scan.records[0].payload == payload_of("rec")
           && scan.records[3].kind == WalRecordKind::state_hash;
}

[[nodiscard]] bool group_commit_max_records_trigger() {
    TempDir dir("gc-records");
    WalConfig config = make_config(dir.path);
    config.group_commit_max_records = 4;
    config.group_commit_max_wait = std::chrono::milliseconds{10000};
    auto ops = std::make_shared<CountingOps>(geoworld::persistence::make_posix_file_ops());
    WalWriter writer(config, ops);
    if (!writer.start().ok()) {
        return false;
    }
    const long long syncs_at_start = ops->file_syncs.load();
    std::vector<AppendTicket> tickets;
    for (int index = 0; index < 4; ++index) {
        auto ticket = writer.append(
            make_record(WalRecordKind::external_command, 1, payload_of("x")));
        if (!ticket.ok()) {
            return false;
        }
        tickets.push_back(ticket.value);
    }
    for (const AppendTicket& ticket : tickets) {
        if (!ticket.wait().ok()) {
            return false;
        }
    }
    // 4 条记录合并为一次组提交：只有一次数据 sync。
    const long long syncs = ops->file_syncs.load() - syncs_at_start;
    writer.shutdown();
    return syncs == 1;
}

[[nodiscard]] bool group_commit_max_bytes_trigger() {
    TempDir dir("gc-bytes");
    WalConfig config = make_config(dir.path);
    config.group_commit_max_records = 100;
    config.group_commit_max_bytes = 10;
    config.group_commit_max_wait = std::chrono::milliseconds{10000};
    auto ops = std::make_shared<CountingOps>(geoworld::persistence::make_posix_file_ops());
    WalWriter writer(config, ops);
    if (!writer.start().ok()) {
        return false;
    }
    const long long syncs_at_start = ops->file_syncs.load();
    std::vector<AppendTicket> tickets;
    for (int index = 0; index < 3; ++index) {
        auto ticket = writer.append(
            make_record(WalRecordKind::external_command, 1, payload_of("01234567")));
        if (!ticket.ok()) {
            return false;
        }
        tickets.push_back(ticket.value);
    }
    for (const AppendTicket& ticket : tickets) {
        if (!ticket.wait().ok()) {
            return false;
        }
    }
    // 单条 payload 8 字节、上限 10 字节：每条记录一个批次，各 sync 一次。
    const long long syncs = ops->file_syncs.load() - syncs_at_start;
    writer.shutdown();
    return syncs == 3;
}

[[nodiscard]] bool group_commit_max_wait_trigger() {
    TempDir dir("gc-wait");
    WalConfig config = make_config(dir.path);
    config.group_commit_max_records = 100;
    config.group_commit_max_wait = std::chrono::milliseconds{300};
    WalWriter writer(config);
    if (!writer.start().ok()) {
        return false;
    }
    const auto begin = std::chrono::steady_clock::now();
    auto ticket = writer.append(
        make_record(WalRecordKind::external_command, 1, payload_of("x")));
    if (!ticket.ok()) {
        return false;
    }
    const auto outcome = ticket.value.wait();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    writer.shutdown();
    // 单条记录必须等到最大等待时间窗口结束才提交。
    return outcome.ok()
           && elapsed >= std::chrono::milliseconds{250}
           && elapsed < std::chrono::milliseconds{10000};
}

[[nodiscard]] bool rotation_by_bytes_and_time() {
    TempDir dir("rotation");
    constexpr std::size_t payload_bytes = 100;
    constexpr std::uint64_t record_total = 4 + 24 + payload_bytes + 4;
    constexpr std::uint64_t header_bytes = 16;
    WalConfig config = make_config(dir.path);
    config.segment_max_bytes = header_bytes + 2 * record_total;
    config.max_record_bytes = record_total;
    WalWriter writer(config);
    if (!writer.start().ok()) {
        return false;
    }
    for (std::uint64_t index = 0; index < 5; ++index) {
        auto ticket = writer.append(make_record(
            WalRecordKind::external_command, index, payload_of(std::string(payload_bytes, 'r'))));
        if (!ticket.ok() || !ticket.value.wait().ok()) {
            return false;
        }
    }
    writer.shutdown();
    const auto layout =
        geoworld::persistence::make_durable_layout(dir.path, config.world, config.branch);
    int closed = 0;
    int active = 0;
    for (const auto& entry : std::filesystem::directory_iterator(layout.wal_dir())) {
        const std::string name = entry.path().filename().string();
        if (name.find(".active") != std::string::npos) {
            ++active;
        } else if (name.find(".gwal") != std::string::npos) {
            ++closed;
        }
    }
    // 每段容纳 2 条记录：5 条记录 -> seg-1、seg-3 关闭，seg-5 活跃。
    if (closed != 2 || active != 1) {
        return false;
    }
    if (find_file_with_suffix(layout.wal_dir(), "seg-00000000000000000001.gwal").empty()
        || find_file_with_suffix(layout.wal_dir(), "seg-00000000000000000003.gwal").empty()) {
        return false;
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    const auto scan = geoworld::persistence::scan_wal_directory(layout.wal_dir(), *ops,
                                                                TailPolicy::strict);
    if (!scan.ok() || scan.records.size() != 5 || scan.last_lsn != Lsn{5}) {
        return false;
    }

    // 时间维度 rotation：活跃段超过最大存续时间后下一条记录触发换段。
    TempDir age_dir("rotation-age");
    WalConfig age_config = make_config(age_dir.path);
    age_config.segment_max_age = std::chrono::milliseconds{200};
    WalWriter age_writer(age_config);
    if (!age_writer.start().ok()) {
        return false;
    }
    auto first = age_writer.append(
        make_record(WalRecordKind::external_command, 1, payload_of("a")));
    if (!first.ok() || !first.value.wait().ok()) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    auto second = age_writer.append(
        make_record(WalRecordKind::external_command, 2, payload_of("b")));
    if (!second.ok() || !second.value.wait().ok()) {
        return false;
    }
    age_writer.shutdown();
    const auto age_layout = geoworld::persistence::make_durable_layout(
        age_dir.path, age_config.world, age_config.branch);
    int age_segments = 0;
    for (const auto& entry : std::filesystem::directory_iterator(age_layout.wal_dir())) {
        if (entry.path().filename().string().find(".gwal") != std::string::npos) {
            ++age_segments;
        }
    }
    return age_segments == 2;
}

[[nodiscard]] bool queue_full_rejected_explicitly() {
    TempDir dir("queue-full");
    WalConfig config = make_config(dir.path);
    config.queue_max_records = 2;
    config.group_commit_max_records = 1;
    config.group_commit_max_wait = std::chrono::milliseconds{1};
    auto ops = std::make_shared<CountingOps>(geoworld::persistence::make_posix_file_ops());
    WalWriter writer(config, ops);
    if (!writer.start().ok()) {
        return false;
    }
    ops->reset_sync_latch();
    ops->sync_delay_ms.store(400);
    auto first = writer.append(make_record(WalRecordKind::external_command, 1, payload_of("a")));
    if (!first.ok() || !ops->wait_sync_entered(std::chrono::milliseconds{10000})) {
        return false;
    }
    // writer 正在慢 sync：1 条在途 + 2 条排队达到上限，第 4 条必须显式拒绝。
    auto second = writer.append(make_record(WalRecordKind::external_command, 2, payload_of("b")));
    auto third = writer.append(make_record(WalRecordKind::external_command, 3, payload_of("c")));
    if (!second.ok() || !third.ok()) {
        return false;
    }
    auto rejected = writer.append(make_record(WalRecordKind::external_command, 4, payload_of("d")));
    if (rejected.ok() || rejected.error != PersistenceError::queue_full
        || std::string_view{geoworld::persistence::error_code(rejected.error)}
               != geoworld::persistence::error_queue_full) {
        return false;
    }
    ops->sync_delay_ms.store(0);
    const bool drained = first.value.wait().ok() && second.value.wait().ok()
                         && third.value.wait().ok();
    writer.shutdown();
    if (!drained) {
        return false;
    }

    // 字节维度上限：第二条记录即超过队列字节上限。
    TempDir bytes_dir("queue-full-bytes");
    WalConfig bytes_config = make_config(bytes_dir.path);
    bytes_config.queue_max_bytes = 4;
    WalWriter bytes_writer(bytes_config);
    if (!bytes_writer.start().ok()) {
        return false;
    }
    auto accepted = bytes_writer.append(
        make_record(WalRecordKind::external_command, 1, payload_of("abc")));
    auto over = bytes_writer.append(
        make_record(WalRecordKind::external_command, 2, payload_of("de")));
    if (!accepted.ok() || over.ok() || over.error != PersistenceError::queue_full) {
        return false;
    }
    const bool accepted_ok = accepted.value.wait().ok();
    bytes_writer.shutdown();
    return accepted_ok;
}

[[nodiscard]] bool relaxed_and_durable_statuses_differ() {
    TempDir dir("relaxed");
    WalConfig config = make_config(dir.path);
    config.allow_relaxed = true;
    WalWriter writer(config);
    if (!writer.start().ok()) {
        return false;
    }
    auto relaxed = writer.append(make_record(WalRecordKind::external_command, 1,
                                             payload_of("r"), Durability::relaxed));
    auto durable = writer.append(make_record(WalRecordKind::external_command, 2,
                                             payload_of("d"), Durability::durable));
    if (!relaxed.ok() || !durable.ok()) {
        return false;
    }
    const auto relaxed_outcome = relaxed.value.wait();
    const auto durable_outcome = durable.value.wait();
    writer.shutdown();
    using geoworld::persistence::AdmissionStatus;
    if (!relaxed_outcome.ok() || relaxed_outcome.status != AdmissionStatus::relaxed_accepted
        || !durable_outcome.ok() || durable_outcome.status != AdmissionStatus::durable_accepted) {
        return false;
    }

    // relaxed 未在配置中显式允许时必须拒绝。
    TempDir strict_dir("relaxed-off");
    WalWriter strict_writer(make_config(strict_dir.path));
    if (!strict_writer.start().ok()) {
        return false;
    }
    auto rejected = strict_writer.append(make_record(WalRecordKind::external_command, 1,
                                                     payload_of("r"), Durability::relaxed));
    strict_writer.shutdown();
    return !rejected.ok() && rejected.error == PersistenceError::relaxed_not_allowed
           && std::string_view{geoworld::persistence::error_code(rejected.error)}
                  == geoworld::persistence::error_relaxed_not_allowed;
}

[[nodiscard]] bool power_cut_tail_is_trimmed() {
    TempDir dir("tail");
    const WalConfig config = make_config(dir.path);
    {
        WalWriter writer(config);
        if (!writer.start().ok()) {
            return false;
        }
        for (std::uint64_t index = 0; index < 3; ++index) {
            auto ticket = writer.append(
                make_record(WalRecordKind::external_command, index, payload_of("ok")));
            if (!ticket.ok() || !ticket.value.wait().ok()) {
                return false;
            }
        }
        writer.shutdown();
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    const auto layout =
        geoworld::persistence::make_durable_layout(dir.path, config.world, config.branch);
    const std::filesystem::path active =
        find_file_with_suffix(layout.wal_dir(), ".active");
    const auto before = ops->read_file(active);
    if (active.empty() || !before.ok()) {
        return false;
    }
    // 模拟断电：长度前缀声称 100 字节，实际只落地 10 字节的撕裂记录。
    {
        std::ofstream stream(active, std::ios::binary | std::ios::app);
        std::vector<std::byte> torn;
        append_le32(torn, 100);
        for (int index = 0; index < 10; ++index) {
            torn.push_back(std::byte{0xAB});
        }
        stream.write(reinterpret_cast<const char*>(torn.data()),
                     static_cast<std::streamsize>(torn.size()));
    }
    const auto trimmed = geoworld::persistence::scan_wal_directory(
        layout.wal_dir(), *ops, TailPolicy::trim_active_tail);
    if (!trimmed.ok() || trimmed.records.size() != 3 || trimmed.trimmed_bytes != 14
        || trimmed.active_segment != active) {
        return false;
    }
    const auto after = ops->read_file(active);
    if (!after.ok() || after.value.size() != before.value.size()) {
        return false;
    }
    // strict 策略下同一撕裂尾部必须 fail-closed 并报告位置。
    const auto strict = geoworld::persistence::scan_wal_directory(
        layout.wal_dir(), *ops, TailPolicy::strict);
    // 注意：上一扫描已修剪文件，此处重新构造撕裂尾。
    {
        std::ofstream stream(active, std::ios::binary | std::ios::app);
        std::vector<std::byte> torn;
        append_le32(torn, 100);
        stream.write(reinterpret_cast<const char*>(torn.data()),
                     static_cast<std::streamsize>(torn.size()));
    }
    const auto strict_result = geoworld::persistence::scan_wal_directory(
        layout.wal_dir(), *ops, TailPolicy::strict);
    if (strict_result.ok() || strict_result.error != PersistenceError::segment_corrupted
        || strict_result.corruption.segment != active
        || strict_result.corruption.offset != before.value.size()
        || strict_result.corruption.expected_lsn != Lsn{4}) {
        return false;
    }
    return strict.ok();
}

[[nodiscard]] bool closed_segment_midfile_corruption_rejected() {
    TempDir dir("corrupt");
    constexpr std::size_t payload_bytes = 100;
    constexpr std::uint64_t record_total = 4 + 24 + payload_bytes + 4;
    WalConfig config = make_config(dir.path);
    config.segment_max_bytes = 16 + 2 * record_total;
    config.max_record_bytes = record_total;
    {
        WalWriter writer(config);
        if (!writer.start().ok()) {
            return false;
        }
        for (std::uint64_t index = 0; index < 3; ++index) {
            auto ticket = writer.append(make_record(
                WalRecordKind::external_command, index,
                payload_of(std::string(payload_bytes, 'c'))));
            if (!ticket.ok() || !ticket.value.wait().ok()) {
                return false;
            }
        }
        writer.shutdown();
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    const auto layout =
        geoworld::persistence::make_durable_layout(dir.path, config.world, config.branch);
    const std::filesystem::path closed =
        layout.wal_dir() / "seg-00000000000000000001.gwal";
    auto bytes = ops->read_file(closed);
    if (!bytes.ok()) {
        return false;
    }
    // 翻转已关闭 segment 第二条记录（文件中部）的 payload 首字节。
    const std::uint64_t second_record_offset = 16 + record_total;
    const std::uint64_t payload_offset = second_record_offset + 4 + 24;
    bytes.value[payload_offset] = bytes.value[payload_offset] ^ std::byte{0xFF};
    {
        std::ofstream stream(closed, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.value.data()),
                     static_cast<std::streamsize>(bytes.value.size()));
    }
    const auto scan = geoworld::persistence::scan_wal_directory(layout.wal_dir(), *ops,
                                                                TailPolicy::trim_active_tail);
    if (scan.ok() || scan.error != PersistenceError::checksum_mismatch) {
        return false;
    }
    const auto& report = scan.corruption;
    return report.segment == closed && report.offset == second_record_offset
           && report.expected_lsn == Lsn{2};
}

[[nodiscard]] bool lsn_discontinuity_rejected() {
    TempDir dir("gap");
    WalConfig config = make_config(dir.path);
    config.segment_max_bytes = 16 + 2 * (4 + 24 + 4 + 4);
    config.max_record_bytes = 4 + 24 + 4 + 4;
    {
        WalWriter writer(config);
        if (!writer.start().ok()) {
            return false;
        }
        for (std::uint64_t index = 0; index < 3; ++index) {
            auto ticket = writer.append(
                make_record(WalRecordKind::external_command, index, payload_of("gap!")));
            if (!ticket.ok() || !ticket.value.wait().ok()) {
                return false;
            }
        }
        writer.shutdown();
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    const auto layout =
        geoworld::persistence::make_durable_layout(dir.path, config.world, config.branch);
    if (ops->remove_file(layout.wal_dir() / "seg-00000000000000000001.gwal")
        != PersistenceError::none) {
        return false;
    }
    const auto scan = geoworld::persistence::scan_wal_directory(layout.wal_dir(), *ops,
                                                                TailPolicy::strict);
    return !scan.ok() && scan.error == PersistenceError::lsn_discontinuity
           && scan.corruption.expected_lsn == geoworld::persistence::kFirstLsn;
}

[[nodiscard]] bool io_and_sync_failures_enter_readonly_fault() {
    // 同步失败：durable promise 以错误完成，writer 进入只读故障态。
    TempDir dir("fault-sync");
    auto ops = std::make_shared<CountingOps>(geoworld::persistence::make_posix_file_ops());
    WalWriter writer(make_config(dir.path), ops);
    if (!writer.start().ok()) {
        return false;
    }
    ops->fail_syncs.store(true);
    auto failed = writer.append(make_record(WalRecordKind::external_command, 1, payload_of("x")));
    if (!failed.ok()) {
        return false;
    }
    const auto outcome = failed.value.wait();
    if (outcome.ok() || outcome.error != PersistenceError::sync_failed
        || outcome.lsn.valid()) {
        return false;
    }
    if (!writer.faulted()) {
        return false;
    }
    auto rejected = writer.append(make_record(WalRecordKind::external_command, 2, payload_of("y")));
    if (rejected.ok() || rejected.error != PersistenceError::fault_read_only
        || std::string_view{geoworld::persistence::error_code(rejected.error)}
               != geoworld::persistence::error_fault_read_only) {
        return false;
    }
    writer.shutdown();

    // 磁盘满/权限错误：写入路径失败同样进入只读故障态。
    TempDir full_dir("fault-write");
    auto full_ops =
        std::make_shared<CountingOps>(geoworld::persistence::make_posix_file_ops());
    WalWriter full_writer(make_config(full_dir.path), full_ops);
    if (!full_writer.start().ok()) {
        return false;
    }
    full_ops->fail_writes.store(true);
    auto failed_write =
        full_writer.append(make_record(WalRecordKind::external_command, 1, payload_of("x")));
    if (!failed_write.ok()) {
        return false;
    }
    const auto write_outcome = failed_write.value.wait();
    const bool write_fault = !write_outcome.ok()
                             && write_outcome.error
                                    == PersistenceError::no_space_or_permission
                             && full_writer.faulted();
    full_writer.shutdown();
    return write_fault;
}

[[nodiscard]] bool atomic_publish_interruption_and_manifest() {
    TempDir dir("publish");
    auto ops = std::make_shared<CountingOps>(geoworld::persistence::make_posix_file_ops());
    const std::filesystem::path manifest_path = dir.path / "directory.gwmanifest";
    geoworld::persistence::DirectoryManifest manifest;
    manifest.world = WorldId{42};
    manifest.branch = make_config(dir.path).branch;

    // rename 中断：发布失败，临时文件不得成为恢复候选，加载必须 not_found。
    ops->fail_renames.store(true);
    if (geoworld::persistence::publish_directory_manifest(*ops, manifest_path, manifest)
        == PersistenceError::none) {
        return false;
    }
    const auto missing = geoworld::persistence::load_directory_manifest(*ops, manifest_path);
    if (missing.ok() || missing.error != PersistenceError::not_found) {
        return false;
    }
    ops->fail_renames.store(false);
    if (geoworld::persistence::publish_directory_manifest(*ops, manifest_path, manifest)
        != PersistenceError::none) {
        return false;
    }
    const auto loaded = geoworld::persistence::load_directory_manifest(*ops, manifest_path);
    if (!loaded.ok() || loaded.value != manifest) {
        return false;
    }
    // manifest 损坏：校验失败而不是静默接受。
    auto bytes = ops->read_file(manifest_path);
    if (!bytes.ok() || bytes.value.size() < 8) {
        return false;
    }
    bytes.value[6] = bytes.value[6] ^ std::byte{0xFF};
    {
        std::ofstream stream(manifest_path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.value.data()),
                     static_cast<std::streamsize>(bytes.value.size()));
    }
    const auto corrupted = geoworld::persistence::load_directory_manifest(*ops, manifest_path);
    if (corrupted.ok() || corrupted.error != PersistenceError::checksum_mismatch) {
        return false;
    }

    // WAL 目录中的 tmp- 前缀文件（发布中断残留）必须被扫描忽略。
    TempDir wal_dir("publish-tmp");
    const WalConfig config = make_config(wal_dir.path);
    {
        WalWriter writer(config);
        if (!writer.start().ok()) {
            return false;
        }
        auto ticket = writer.append(
            make_record(WalRecordKind::external_command, 1, payload_of("x")));
        if (!ticket.ok() || !ticket.value.wait().ok()) {
            return false;
        }
        writer.shutdown();
    }
    const auto layout =
        geoworld::persistence::make_durable_layout(wal_dir.path, config.world, config.branch);
    {
        std::ofstream leftover(layout.wal_dir() / "tmp-seg-crash.gwal", std::ios::binary);
        leftover << "garbage";
    }
    const auto scan = geoworld::persistence::scan_wal_directory(
        layout.wal_dir(), *geoworld::persistence::make_posix_file_ops(), TailPolicy::strict);
    return scan.ok() && scan.records.size() == 1;
}

[[nodiscard]] bool shutdown_resolves_all_tickets() {
    TempDir dir("shutdown");
    WalConfig config = make_config(dir.path);
    config.group_commit_max_wait = std::chrono::milliseconds{10000};
    WalWriter writer(config);
    if (!writer.start().ok()) {
        return false;
    }
    std::vector<AppendTicket> tickets;
    for (int index = 0; index < 5; ++index) {
        auto ticket = writer.append(
            make_record(WalRecordKind::external_command, 1, payload_of("s")));
        if (!ticket.ok()) {
            return false;
        }
        tickets.push_back(ticket.value);
    }
    // 不等待直接 shutdown：flush 当前承诺边界后返回，所有 ticket 必须已有终态。
    writer.shutdown();
    for (const AppendTicket& ticket : tickets) {
        if (ticket.pending()) {
            return false;
        }
        const auto outcome = ticket.wait();
        if (!outcome.ok()) {
            return false;
        }
    }
    auto late = writer.append(make_record(WalRecordKind::external_command, 2, payload_of("s")));
    if (late.ok() || late.error != PersistenceError::shutting_down) {
        return false;
    }
    writer.shutdown();
    return true;
}

[[nodiscard]] bool directory_identity_mismatch_rejected() {
    TempDir dir("identity");
    WalConfig config = make_config(dir.path);
    const auto layout =
        geoworld::persistence::make_durable_layout(dir.path, config.world, config.branch);
    std::filesystem::create_directories(layout.manifest_dir());
    geoworld::persistence::DirectoryManifest foreign;
    foreign.world = WorldId{42};
    foreign.branch =
        geoworld::persistence::parse_branch_id("ffffffff-ffff-ffff-ffff-ffffffffffff")
            .value_or(BranchId{});
    const auto ops = geoworld::persistence::make_posix_file_ops();
    if (geoworld::persistence::publish_directory_manifest(
            *ops, layout.directory_manifest_path(), foreign)
        != PersistenceError::none) {
        return false;
    }
    WalWriter writer(config);
    const auto status = writer.start();
    writer.shutdown();
    return !status.ok() && status.error == PersistenceError::config_invalid;
}

} // namespace

int main() {
    if (!lsn_and_id_rules()) {
        return 1;
    }
    if (!record_bytes_deterministic_and_layout()) {
        return 2;
    }
    if (!durable_append_scan_and_restart()) {
        return 3;
    }
    if (!group_commit_max_records_trigger()) {
        return 4;
    }
    if (!group_commit_max_bytes_trigger()) {
        return 5;
    }
    if (!group_commit_max_wait_trigger()) {
        return 6;
    }
    if (!rotation_by_bytes_and_time()) {
        return 7;
    }
    if (!queue_full_rejected_explicitly()) {
        return 8;
    }
    if (!relaxed_and_durable_statuses_differ()) {
        return 9;
    }
    if (!power_cut_tail_is_trimmed()) {
        return 10;
    }
    if (!closed_segment_midfile_corruption_rejected()) {
        return 11;
    }
    if (!lsn_discontinuity_rejected()) {
        return 12;
    }
    if (!io_and_sync_failures_enter_readonly_fault()) {
        return 13;
    }
    if (!atomic_publish_interruption_and_manifest()) {
        return 14;
    }
    if (!shutdown_resolves_all_tickets()) {
        return 15;
    }
    if (!directory_identity_mismatch_rejected()) {
        return 16;
    }
    return 0;
}
