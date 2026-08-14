#include "geoworld/persistence/wal.hpp"

#include "wal_codec.hpp"

#include <mio/mmap.hpp>

#include <algorithm>
#include <system_error>

namespace geoworld::persistence {

namespace {

struct SegmentEntry {
    std::filesystem::path path;
    Lsn first_lsn{};
    bool active{false};
};

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size()
           && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

struct MappedFile {
    mio::mmap_source map;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {reinterpret_cast<const std::byte*>(map.data()), map.size()};
    }
};

[[nodiscard]] Result<MappedFile> map_file(const std::filesystem::path& path) {
    // mio 拒绝映射空文件；空活跃段是合法的断电残留，按空视图处理。
    std::error_code size_ec;
    const auto file_size = std::filesystem::file_size(path, size_ec);
    Result<MappedFile> result;
    if (size_ec) {
        result.error = size_ec == std::errc::no_such_file_or_directory
                           ? PersistenceError::not_found
                           : PersistenceError::io_failure;
        return result;
    }
    if (file_size == 0) {
        return result;
    }
    std::error_code ec;
    result.value.map = mio::make_mmap_source(path.string(), ec);
    if (ec) {
        result.error = ec == std::errc::no_such_file_or_directory
                           ? PersistenceError::not_found
                           : PersistenceError::io_failure;
    }
    return result;
}

void report_corruption(WalScanResult& result, const SegmentEntry& segment,
                       std::uint64_t offset, Lsn expected, PersistenceError error) {
    result.error = error;
    result.corruption =
        CorruptionReport{segment.path, offset, expected, error};
}

// 修剪活跃 segment 尾部：最后一个活跃段的不完整记录允许按策略截断，其余一律 fail-closed。
void trim_active_tail(WalScanResult& result, const SegmentEntry& segment, FileOps& ops,
                      std::uint64_t good_offset) {
    auto mapped_size = std::uint64_t{};
    {
        auto mapped = map_file(segment.path);
        if (mapped.ok()) {
            mapped_size = mapped.value.bytes().size();
        }
    }
    if (mapped_size > good_offset) {
        result.trimmed_bytes += mapped_size - good_offset;
    }
    ops.truncate_file(segment.path, good_offset);
    result.active_segment = segment.path;
}

// 扫描单个 segment，返回 false 表示 result 已携带错误或被修剪后终止。
[[nodiscard]] bool scan_segment(WalScanResult& result, const SegmentEntry& segment,
                                FileOps& ops, TailPolicy policy,
                                std::size_t max_record_bytes, Lsn& expected) {
    auto mapped = map_file(segment.path);
    if (!mapped.ok()) {
        report_corruption(result, segment, 0, expected, mapped.error);
        return false;
    }
    const std::span<const std::byte> bytes = mapped.value.bytes();
    const bool trimmable = segment.active && policy == TailPolicy::trim_active_tail;
    if (bytes.size() < kSegmentHeaderBytes) {
        // 空活跃段是合法的（新建未写入）；非空但不足头部按尾部修剪或损坏处理。
        if (bytes.empty()) {
            if (segment.active) {
                result.active_segment = segment.path;
            } else {
                report_corruption(result, segment, 0, expected,
                                  PersistenceError::segment_corrupted);
                return false;
            }
            return true;
        }
        if (trimmable) {
            trim_active_tail(result, segment, ops, 0);
            return true;
        }
        report_corruption(result, segment, 0, expected, PersistenceError::segment_corrupted);
        return false;
    }
    Lsn header_first{};
    if (!detail::parse_segment_header(bytes, header_first)) {
        if (trimmable) {
            trim_active_tail(result, segment, ops, 0);
            return true;
        }
        report_corruption(result, segment, 0, expected, PersistenceError::segment_corrupted);
        return false;
    }
    if (header_first != expected) {
        report_corruption(result, segment, 0, expected,
                          PersistenceError::lsn_discontinuity);
        return false;
    }
    if (segment.active) {
        result.active_segment = segment.path;
    }
    std::uint64_t offset = kSegmentHeaderBytes;
    while (offset < bytes.size()) {
        const detail::RecordDecodeResult decoded =
            detail::decode_record(bytes.subspan(offset), max_record_bytes);
        if (decoded.status != detail::RecordDecodeStatus::ok) {
            if (trimmable) {
                trim_active_tail(result, segment, ops, offset);
                return true;
            }
            const PersistenceError error =
                decoded.status == detail::RecordDecodeStatus::crc_mismatch
                    ? PersistenceError::checksum_mismatch
                    : PersistenceError::segment_corrupted;
            report_corruption(result, segment, offset, expected, error);
            return false;
        }
        if (decoded.record.lsn != expected) {
            report_corruption(result, segment, offset, expected,
                              PersistenceError::lsn_discontinuity);
            return false;
        }
        ScannedRecord record;
        record.lsn = decoded.record.lsn;
        record.kind = decoded.record.kind;
        record.target_tick = decoded.record.target_tick;
        record.payload.assign(decoded.record.payload.begin(), decoded.record.payload.end());
        result.records.push_back(std::move(record));
        const std::optional<Lsn> next = next_lsn(expected);
        if (!next.has_value()) {
            report_corruption(result, segment, offset, expected,
                              PersistenceError::lsn_overflow);
            return false;
        }
        expected = *next;
        offset += decoded.record.total_bytes;
    }
    return true;
}

} // namespace

WalScanResult scan_wal_directory(const std::filesystem::path& wal_dir, FileOps& ops,
                                 TailPolicy policy, Lsn first_expected,
                                 std::size_t max_record_bytes) {
    WalScanResult result;
    auto listed = ops.list_files(wal_dir);
    if (!listed.ok()) {
        if (listed.error == PersistenceError::not_found) {
            return result;
        }
        result.error = listed.error;
        result.corruption = CorruptionReport{wal_dir, 0, first_expected, listed.error};
        return result;
    }
    std::vector<SegmentEntry> segments;
    for (const std::filesystem::path& file : listed.value) {
        const std::string name = file.filename().string();
        // tmp- 前缀是原子发布的临时文件，永远不是恢复候选。
        if (starts_with(name, kTempFilePrefix)) {
            continue;
        }
        if (ends_with(name, kActiveSegmentSuffix)) {
            const std::optional<Lsn> first = parse_segment_file_name(
                std::string_view{name}.substr(0, name.size() - kActiveSegmentSuffix.size()));
            if (first.has_value()) {
                segments.push_back(SegmentEntry{file, *first, true});
            }
            continue;
        }
        if (ends_with(name, kSegmentExtension)) {
            const std::optional<Lsn> first = parse_segment_file_name(
                std::string_view{name}.substr(0, name.size() - kSegmentExtension.size()));
            if (first.has_value()) {
                segments.push_back(SegmentEntry{file, *first, false});
            }
        }
    }
    std::sort(segments.begin(), segments.end(), [](const SegmentEntry& left,
                                                   const SegmentEntry& right) {
        if (left.active != right.active) {
            return !left.active;
        }
        return left.first_lsn < right.first_lsn;
    });
    Lsn expected = first_expected;
    for (const SegmentEntry& segment : segments) {
        if (!scan_segment(result, segment, ops, policy, max_record_bytes, expected)) {
            return result;
        }
    }
    const std::optional<Lsn> previous =
        expected.valid() && expected.value > first_expected.value
            ? std::optional<Lsn>{Lsn{expected.value - 1}}
            : std::nullopt;
    if (previous.has_value()) {
        result.last_lsn = *previous;
    }
    return result;
}

} // namespace geoworld::persistence
