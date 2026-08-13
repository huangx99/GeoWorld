# gateway

实现会话管理、鉴权/授权、属性级所有权、虚拟连接、命令接入与终态回执、可合并状态队列与有界可靠队列背压，以及控制面/数据面传输适配。

M4 已实现：`GatewayCore` 传输无关核心（编码器与命令提交器以 `std::function` 注入，核心不含协议与网络类型）、`frame_codec`（projection ↔ protocol wire ↔ GWSF 字节，wire 类型进入本模块的唯一位置）、gRPC 控制面（8 个 RPC，`control_server`）、Boost.Beast WebSocket 数据面（二进制帧、一次性 stream ticket、OpenSSL TLS，非 loopback 强制，`stream_transport`）。WebTransport/QUIC 暂不实施。完整范围和验收标准见 [`docs/M4.md`](../../docs/M4.md)。
