#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace geoworld::persistence {

// LSN：单世界、单分支内从 1 开始的 64 位单调序列，0 无效，语义冻结于 docs/M5.md。
struct Lsn {
    std::uint64_t value{};

    constexpr auto operator<=>(const Lsn&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};

inline constexpr Lsn kFirstLsn{1};

// 溢出拒绝：到达 64 位上限后不允许继续分配，调用方必须停止写入。
[[nodiscard]] constexpr std::optional<Lsn> next_lsn(Lsn current) noexcept {
    if (!current.valid() || current.value == UINT64_MAX) {
        return std::nullopt;
    }
    return Lsn{current.value + 1};
}

// 分支标识：128 位，线格式固定为 16 字节原始字节序（RFC 4122 大端顺序）。
// 公共 API 只暴露字节数组与规范字符串，不暴露 Boost.UUID 类型。
struct BranchId {
    std::array<std::uint8_t, 16> bytes{};

    constexpr auto operator<=>(const BranchId&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept {
        for (const std::uint8_t byte : bytes) {
            if (byte != 0) {
                return true;
            }
        }
        return false;
    }
};

[[nodiscard]] BranchId generate_branch_id();
// 只接受规范 36 字符形式（8-4-4-4-12 十六进制）。
[[nodiscard]] std::optional<BranchId> parse_branch_id(std::string_view text) noexcept;
[[nodiscard]] std::string format_branch_id(BranchId id);

// 稳定错误码第一批，含义逐字冻结于 docs/M5.md WAL 契约；
// 后续只能追加，不能复用或改变语义。
inline constexpr std::string_view error_record_invalid = "GWP001";        // 记录字段、类型或大小无效
inline constexpr std::string_view error_lsn_discontinuity = "GWP002";     // LSN 不连续
inline constexpr std::string_view error_checksum_mismatch = "GWP003";     // CRC32C 校验失败
inline constexpr std::string_view error_segment_corrupted = "GWP004";     // segment 结构损坏（中部或已关闭段）
inline constexpr std::string_view error_io_failure = "GWP005";            // 底层读写失败
inline constexpr std::string_view error_sync_failed = "GWP006";           // fdatasync/fsync 失败
inline constexpr std::string_view error_no_space_or_permission = "GWP007";// 磁盘满或权限错误
inline constexpr std::string_view error_queue_full = "GWP008";            // durable 写入队列积圧达到配置上限
inline constexpr std::string_view error_fault_read_only = "GWP009";       // WAL 已进入只读故障状态
inline constexpr std::string_view error_config_invalid = "GWP010";        // 配置或目录身份无效
inline constexpr std::string_view error_lsn_overflow = "GWP011";          // LSN 溢出，拒绝继续写入
inline constexpr std::string_view error_manifest_invalid = "GWP012";      // manifest 缺失字段或校验失败
inline constexpr std::string_view error_not_found = "GWP013";             // 请求的 segment/manifest 不存在
inline constexpr std::string_view error_shutting_down = "GWP014";         // writer 正在或已经关闭
inline constexpr std::string_view error_relaxed_not_allowed = "GWP015";   // relaxed 耐久级别未在配置中显式允许

enum class PersistenceError {
    none,
    record_invalid,
    lsn_discontinuity,
    checksum_mismatch,
    segment_corrupted,
    io_failure,
    sync_failed,
    no_space_or_permission,
    queue_full,
    fault_read_only,
    config_invalid,
    lsn_overflow,
    manifest_invalid,
    not_found,
    shutting_down,
    relaxed_not_allowed,
};

[[nodiscard]] const char* error_code(PersistenceError error) noexcept;

template <typename T>
struct Result {
    T value{};
    PersistenceError error{PersistenceError::none};

    [[nodiscard]] bool ok() const noexcept { return error == PersistenceError::none; }
};

struct Status {
    PersistenceError error{PersistenceError::none};

    [[nodiscard]] bool ok() const noexcept { return error == PersistenceError::none; }
};

} // namespace geoworld::persistence
