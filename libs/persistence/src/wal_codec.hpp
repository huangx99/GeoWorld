#pragma once

// 内部 WAL 分帧编解码：长度前缀 + 固定头（格式版本/记录类型/LSN/目标 tick/
// payload 长度）+ payload + CRC32C。FlatBuffers 只负责 payload 内容，不参与分帧。

#include "geoworld/persistence/durable_log.hpp"
#include "geoworld/persistence/types.hpp"
#include "geoworld/persistence/wal.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace geoworld::persistence::detail {

// 完整记录字节：长度前缀 + 固定头 + payload + CRC32C。
[[nodiscard]] std::vector<std::byte> encode_record(Lsn lsn, WalRecordKind kind,
                                                   std::uint64_t target_tick,
                                                   std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_segment_header(Lsn first_lsn);

enum class RecordDecodeStatus : std::uint8_t {
    ok,
    incomplete,      // 尾部不完整（长度前缀不足或记录被截断）
    crc_mismatch,    // CRC32C 校验失败
    malformed,       // 格式版本或长度字段非法
};

struct DecodedRecord {
    Lsn lsn{};
    WalRecordKind kind{WalRecordKind::external_command};
    std::uint64_t target_tick{};
    std::span<const std::byte> payload;
    std::uint64_t total_bytes{};
};

struct RecordDecodeResult {
    RecordDecodeStatus status{RecordDecodeStatus::incomplete};
    DecodedRecord record{};
};

[[nodiscard]] RecordDecodeResult decode_record(std::span<const std::byte> buffer,
                                               std::size_t max_record_bytes) noexcept;

[[nodiscard]] bool parse_segment_header(std::span<const std::byte> buffer,
                                        Lsn& first_lsn) noexcept;

} // namespace geoworld::persistence::detail
