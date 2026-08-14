#include "wal_codec.hpp"

#include "le_codec.hpp"

#include <crc32c/crc32c.h>

#include <cstdio>
#include <cstring>

namespace geoworld::persistence::detail {

namespace {

// 固定头布局（little-endian）：格式版本 u16、记录类型 u16、LSN u64、目标 tick u64、
// payload 长度 u32。长度前缀与 CRC 不在固定头内。
constexpr std::size_t offset_format_version = 0;
constexpr std::size_t offset_kind = 2;
constexpr std::size_t offset_lsn = 4;
constexpr std::size_t offset_target_tick = 12;
constexpr std::size_t offset_payload_length = 20;

[[nodiscard]] std::uint32_t record_crc(std::span<const std::byte> fixed_header,
                                       std::span<const std::byte> payload) noexcept {
    std::uint32_t crc = crc32c::Crc32c(
        reinterpret_cast<const std::uint8_t*>(fixed_header.data()), fixed_header.size());
    crc = crc32c::Extend(crc, reinterpret_cast<const std::uint8_t*>(payload.data()),
                         payload.size());
    return crc;
}

} // namespace

std::vector<std::byte> encode_record(Lsn lsn, WalRecordKind kind, std::uint64_t target_tick,
                                     std::span<const std::byte> payload) {
    const std::uint32_t body_bytes = static_cast<std::uint32_t>(
        kRecordFixedHeaderBytes + payload.size() + kRecordCrcBytes);
    std::vector<std::byte> out(kRecordLengthPrefixBytes + body_bytes);
    write_le32(out.data(), body_bytes);
    std::byte* header = out.data() + kRecordLengthPrefixBytes;
    write_le16(header + offset_format_version, kRecordFormatVersion);
    write_le16(header + offset_kind, static_cast<std::uint16_t>(kind));
    write_le64(header + offset_lsn, lsn.value);
    write_le64(header + offset_target_tick, target_tick);
    write_le32(header + offset_payload_length, static_cast<std::uint32_t>(payload.size()));
    std::memcpy(out.data() + kRecordLengthPrefixBytes + kRecordFixedHeaderBytes, payload.data(),
                payload.size());
    const std::span<const std::byte> fixed_header{header, kRecordFixedHeaderBytes};
    write_le32(out.data() + kRecordLengthPrefixBytes + kRecordFixedHeaderBytes + payload.size(),
               record_crc(fixed_header, payload));
    return out;
}

std::vector<std::byte> encode_segment_header(Lsn first_lsn) {
    std::vector<std::byte> out(kSegmentHeaderBytes);
    std::memcpy(out.data(), kSegmentMagic.data(), kSegmentMagic.size());
    write_le32(out.data() + kSegmentMagic.size(), kSegmentFormatVersion);
    write_le64(out.data() + kSegmentMagic.size() + sizeof(std::uint32_t), first_lsn.value);
    return out;
}

RecordDecodeResult decode_record(std::span<const std::byte> buffer,
                                 std::size_t max_record_bytes) noexcept {
    RecordDecodeResult result;
    if (buffer.size() < kRecordLengthPrefixBytes) {
        return result;
    }
    const std::uint32_t body_bytes = read_le32(buffer.data());
    const std::uint64_t total = kRecordLengthPrefixBytes + static_cast<std::uint64_t>(body_bytes);
    if (body_bytes < kRecordFixedHeaderBytes + kRecordCrcBytes || body_bytes > max_record_bytes) {
        result.status = RecordDecodeStatus::malformed;
        return result;
    }
    if (buffer.size() < total) {
        return result;
    }
    const std::byte* header = buffer.data() + kRecordLengthPrefixBytes;
    if (read_le16(header + offset_format_version) != kRecordFormatVersion) {
        result.status = RecordDecodeStatus::malformed;
        return result;
    }
    const std::uint32_t payload_length = read_le32(header + offset_payload_length);
    if (static_cast<std::uint64_t>(payload_length)
        != body_bytes - kRecordFixedHeaderBytes - kRecordCrcBytes) {
        result.status = RecordDecodeStatus::malformed;
        return result;
    }
    const std::span<const std::byte> payload{
        header + kRecordFixedHeaderBytes, payload_length};
    const std::span<const std::byte> fixed_header{header, kRecordFixedHeaderBytes};
    const std::uint32_t expected_crc = read_le32(header + kRecordFixedHeaderBytes + payload_length);
    if (record_crc(fixed_header, payload) != expected_crc) {
        result.status = RecordDecodeStatus::crc_mismatch;
        return result;
    }
    result.status = RecordDecodeStatus::ok;
    result.record.lsn = Lsn{read_le64(header + offset_lsn)};
    result.record.kind = static_cast<WalRecordKind>(read_le16(header + offset_kind));
    result.record.target_tick = read_le64(header + offset_target_tick);
    result.record.payload = payload;
    result.record.total_bytes = total;
    return result;
}

bool parse_segment_header(std::span<const std::byte> buffer, Lsn& first_lsn) noexcept {
    if (buffer.size() < kSegmentHeaderBytes) {
        return false;
    }
    if (std::memcmp(buffer.data(), kSegmentMagic.data(), kSegmentMagic.size()) != 0) {
        return false;
    }
    if (read_le32(buffer.data() + kSegmentMagic.size()) != kSegmentFormatVersion) {
        return false;
    }
    first_lsn = Lsn{read_le64(buffer.data() + kSegmentMagic.size() + sizeof(std::uint32_t))};
    return first_lsn.valid();
}

} // namespace geoworld::persistence::detail

namespace geoworld::persistence {

std::string segment_file_name(Lsn first_lsn) {
    // 20 位零填充保证文件名排序与 LSN 顺序一致（uint64 最大 20 位十进制）。
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "seg-%020llu",
                  static_cast<unsigned long long>(first_lsn.value));
    return buffer;
}

std::optional<Lsn> parse_segment_file_name(std::string_view name) noexcept {
    constexpr std::string_view prefix = "seg-";
    constexpr std::size_t digits = 20;
    if (name.size() != prefix.size() + digits || name.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char ch : name.substr(prefix.size())) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    const Lsn lsn{value};
    if (!lsn.valid()) {
        return std::nullopt;
    }
    return lsn;
}

} // namespace geoworld::persistence
