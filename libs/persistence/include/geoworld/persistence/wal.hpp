#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/persistence/durable_log.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geoworld::persistence {

// 协议固定标识与版本（具名常量，语义冻结于 docs/M5.md WAL 契约）。
inline constexpr std::array<char, 4> kSegmentMagic{'G', 'W', 'L', 'S'};
inline constexpr std::uint32_t kSegmentFormatVersion = 1;
inline constexpr std::uint16_t kRecordFormatVersion = 1;
inline constexpr std::size_t kSegmentHeaderBytes = 16;
inline constexpr std::size_t kRecordLengthPrefixBytes = 4;
inline constexpr std::size_t kRecordFixedHeaderBytes = 24;
inline constexpr std::size_t kRecordCrcBytes = 4;

// segment 文件命名冻结：关闭段 seg-<首LSN,20位零填充>.gwal，活跃段加 .active 后缀，
// 原子发布与写入中的临时文件统一 tmp- 前缀，永不成恢复候选。
inline constexpr std::string_view kSegmentExtension = ".gwal";
inline constexpr std::string_view kActiveSegmentSuffix = ".gwal.active";
inline constexpr std::string_view kTempFilePrefix = "tmp-";

[[nodiscard]] std::string segment_file_name(Lsn first_lsn);
[[nodiscard]] std::optional<Lsn> parse_segment_file_name(std::string_view name) noexcept;

// 配置默认值：业务阈值集中于命名常量，WalConfig 字段可覆盖。
inline constexpr std::size_t kDefaultQueueMaxRecords = 4096;
inline constexpr std::size_t kDefaultQueueMaxBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kDefaultGroupCommitMaxRecords = 256;
inline constexpr std::size_t kDefaultGroupCommitMaxBytes = 4U * 1024U * 1024U;
inline constexpr std::chrono::milliseconds kDefaultGroupCommitMaxWait{5};
inline constexpr std::uint64_t kDefaultSegmentMaxBytes = 64U * 1024U * 1024U;
inline constexpr std::chrono::seconds kDefaultSegmentMaxAge{300};
inline constexpr std::size_t kDefaultMaxRecordBytes = 4U * 1024U * 1024U;

struct WalConfig {
    std::filesystem::path durable_root;
    geoworld::foundation::WorldId world{};
    BranchId branch{};
    std::size_t queue_max_records{kDefaultQueueMaxRecords};
    std::size_t queue_max_bytes{kDefaultQueueMaxBytes};
    std::size_t group_commit_max_records{kDefaultGroupCommitMaxRecords};
    std::size_t group_commit_max_bytes{kDefaultGroupCommitMaxBytes};
    std::chrono::milliseconds group_commit_max_wait{kDefaultGroupCommitMaxWait};
    std::uint64_t segment_max_bytes{kDefaultSegmentMaxBytes};
    std::chrono::milliseconds segment_max_age{kDefaultSegmentMaxAge};
    std::size_t max_record_bytes{kDefaultMaxRecordBytes};
    bool allow_relaxed{false};
    // 仅在已验证检查点覆盖旧 WAL 时设置；允许从该 LSN 之后的首个保留段恢复。
    Lsn recovery_floor_lsn{};
};

struct WalWriterMetrics {
    std::uint64_t group_commit_batches{};
    std::uint64_t records_written{};
    std::vector<std::uint64_t> group_commit_nanoseconds;
    std::vector<std::uint64_t> sync_nanoseconds;
    std::vector<std::uint64_t> rotation_nanoseconds;
};

// 单 writer WAL：LSN 唯一分配、有界队列、组提交、segment rotation、只读故障态。
// 后台线程只处理自有不可变记录；file_ops 为空时使用 POSIX 本地文件系统实现。
class WalWriter final : public DurableLog {
public:
    explicit WalWriter(WalConfig config, std::shared_ptr<FileOps> file_ops = {});
    ~WalWriter() override;

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // 创建目录、校验目录 manifest 身份、修剪断电尾部并恢复 LSN 分配位置。
    [[nodiscard]] Status start();

    [[nodiscard]] Result<AppendTicket> append(WalRecord record) override;
    [[nodiscard]] Lsn last_durable_lsn() const noexcept override;
    [[nodiscard]] bool faulted() const noexcept override;
    [[nodiscard]] WalWriterMetrics metrics() const;
    void shutdown() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 启动恢复只允许 trim_active_tail：修剪最后一个活跃 segment 尾部的不完整记录。
enum class TailPolicy : std::uint8_t {
    trim_active_tail,
    strict,
};

struct CorruptionReport {
    std::filesystem::path segment;
    std::uint64_t offset{};
    Lsn expected_lsn{};
    PersistenceError error{PersistenceError::none};
};

struct ScannedRecord {
    Lsn lsn{};
    WalRecordKind kind{WalRecordKind::external_command};
    std::uint64_t target_tick{};
    std::vector<std::byte> payload;
};

struct WalScanResult {
    std::vector<ScannedRecord> records;
    PersistenceError error{PersistenceError::none};
    CorruptionReport corruption{};
    std::uint64_t trimmed_bytes{};
    // first_lsn 是目录中首个保留段的起点；next_lsn 在空活跃段时仍有效。
    Lsn first_lsn{};
    Lsn last_lsn{};
    Lsn next_lsn{};
    // 扫描到的活跃 segment（已按策略修剪）；为空表示当前没有活跃段。
    std::filesystem::path active_segment;

    [[nodiscard]] bool ok() const noexcept { return error == PersistenceError::none; }
};

// 连续扫描并校验 LSN 连续性与 CRC；已关闭 segment 或文件中部损坏 fail-closed，
// 在 corruption 中报告 segment、offset 与期望 LSN。first_expected 用于检查点后的
// 局部扫描（默认从 kFirstLsn 起）；first_expected=0 表示从首个保留段自动发现。
[[nodiscard]] WalScanResult scan_wal_directory(
    const std::filesystem::path& wal_dir, FileOps& ops,
    TailPolicy policy = TailPolicy::trim_active_tail, Lsn first_expected = kFirstLsn,
    std::size_t max_record_bytes = kDefaultMaxRecordBytes);

} // namespace geoworld::persistence
