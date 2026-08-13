#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace geoworld::protocol {

// 协议版本与标识，冻结于 docs/M4.md「线协议版本与错误码」；
// 破坏性变化必须使用新 identifier/subprotocol 和新的 Protobuf package。
inline constexpr std::uint32_t control_api_version = 1;
inline constexpr std::uint32_t data_schema_version = 1;
inline constexpr std::uint32_t projection_schema_version = 1;
inline constexpr std::string_view websocket_subprotocol = "geoworld.stream.v1";
inline constexpr std::string_view server_frame_identifier = "GWSF";
inline constexpr std::string_view client_frame_identifier = "GWCF";

// 闭区间版本支持范围；minimum > maximum 视为非法范围。
struct VersionRange {
    std::uint32_t minimum{};
    std::uint32_t maximum{};

    [[nodiscard]] constexpr bool valid() const noexcept { return minimum <= maximum; }
};

// 版本协商：客户端与服务端各自给出支持范围，选择双方共同的最高版本；
// 范围非法或无交集时返回空（拒绝会话，对应 GWG001 协议或 schema 版本不兼容）。
[[nodiscard]] constexpr std::optional<std::uint32_t> negotiate_version(
    VersionRange client, VersionRange server) noexcept {
    if (!client.valid() || !server.valid()) {
        return std::nullopt;
    }
    const std::uint32_t common_minimum = std::max(client.minimum, server.minimum);
    const std::uint32_t common_maximum = std::min(client.maximum, server.maximum);
    if (common_minimum > common_maximum) {
        return std::nullopt;
    }
    return common_maximum;
}

} // namespace geoworld::protocol
