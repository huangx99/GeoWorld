#pragma once

#include "geoworld/protocol/limits.hpp"
#include "geoworld/protocol/wire.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flatbuffers {
template <bool Is64Aware> class FlatBufferBuilderImpl;
using FlatBufferBuilder = FlatBufferBuilderImpl<false>;
}

namespace geoworld::protocol {

// 解码失败的稳定错误码（error.hpp）与诊断信息；诊断不进入协议响应。
struct DecodeFailure {
    std::string_view error_code{};
    std::string message;
};

// 编码逐字节确定：相同输入产生相同字节序列（有序 map 与 vector 均按既有顺序写出）。
// 输入超过 limits 时返回空 vector。
[[nodiscard]] std::vector<std::uint8_t> encode_server_frame(
    const WireFrame& frame, const ProtocolLimits& limits = {});
// 热路径复用变体：编码进调用方持有的 builder（先 Reset），产物留在 builder 内，
// 输出与 encode_server_frame 逐字节一致；失败返回 false 且 builder 内容无效。
[[nodiscard]] bool encode_server_frame_into(
    const WireFrame& frame, const ProtocolLimits& limits,
    flatbuffers::FlatBufferBuilder& builder);
[[nodiscard]] std::vector<std::uint8_t> encode_client_control(
    const WireClientControl& control, const ProtocolLimits& limits = {});

// 解码先校验：buffer 非空且 <= max_frame_bytes、file identifier 匹配、
// schema_version == data_schema_version、flatbuffers::Verifier（含深度/表数量上限）、
// 枚举范围与字符串/数量上限。任何校验失败返回空并填充 failure（含稳定错误码），不抛异常。
[[nodiscard]] std::optional<WireFrame> decode_server_frame(
    std::span<const std::uint8_t> bytes, DecodeFailure& failure, const ProtocolLimits& limits = {});
[[nodiscard]] std::optional<WireClientControl> decode_client_control(
    std::span<const std::uint8_t> bytes, DecodeFailure& failure, const ProtocolLimits& limits = {});

} // namespace geoworld::protocol
