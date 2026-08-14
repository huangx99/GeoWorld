#pragma once

// 内部 little-endian 编解码：WAL 记录、segment 头与目录 manifest 的
// 多字节整数固定为 little-endian，与宿主机字节序无关。

#include <cstddef>
#include <cstdint>
#include <span>

namespace geoworld::persistence::detail {

inline void write_le16(std::byte* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::byte>(value & 0xFFU);
    out[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

inline void write_le32(std::byte* out, std::uint32_t value) noexcept {
    for (std::uint32_t index = 0; index < 4; ++index) {
        out[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

inline void write_le64(std::byte* out, std::uint64_t value) noexcept {
    for (std::uint64_t index = 0; index < 8; ++index) {
        out[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] inline std::uint16_t read_le16(const std::byte* in) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0])
                                      | static_cast<std::uint16_t>(
                                            static_cast<std::uint16_t>(in[1]) << 8U));
}

[[nodiscard]] inline std::uint32_t read_le32(const std::byte* in) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(in[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] inline std::uint64_t read_le64(const std::byte* in) noexcept {
    std::uint64_t value = 0;
    for (std::uint64_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(in[index]) << (index * 8U);
    }
    return value;
}

} // namespace geoworld::persistence::detail
