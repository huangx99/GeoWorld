#pragma once

#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/limits.hpp"
#include "geoworld/protocol/wire.hpp"
#include "geoworld/projection/frame.hpp"

#include <string_view>

namespace geoworld::gateway {

// projection StateFrame / gateway 回执与 protocol wire 结构、GWSF 字节之间的转换。
// 这是 protocol wire 类型进入 gateway 的唯一位置；核心只接受注入的 std::function。
// 编码逐字节确定；输入超过 limits 时 protocol 编码返回空 vector。

[[nodiscard]] protocol::WireEntity to_wire_entity(const projection::ProjectedEntity& entity);
[[nodiscard]] protocol::WireFrame to_wire_frame(const projection::StateFrame& frame);
[[nodiscard]] protocol::WireCommandReceipt to_wire_receipt(const CommandReceipt& receipt);

// GatewayCore 注入签名工厂：FrameBytes 出口。
[[nodiscard]] GatewayCore::FrameEncoder make_frame_encoder(
    protocol::ProtocolLimits limits = {});
[[nodiscard]] GatewayCore::ReceiptEncoder make_receipt_encoder(
    protocol::ProtocolLimits limits = {});
[[nodiscard]] GatewayCore::HeartbeatEncoder make_heartbeat_encoder(
    protocol::ProtocolLimits limits = {});

// 协议错误 ReliableEvent；传输层在畸形输入等情况下直接写给对端后关闭连接。
[[nodiscard]] FrameBytes encode_protocol_error(std::string_view code,
                                               std::string_view message,
                                               const protocol::ProtocolLimits& limits = {});

} // namespace geoworld::gateway
