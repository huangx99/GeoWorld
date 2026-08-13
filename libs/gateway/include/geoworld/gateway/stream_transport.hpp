#pragma once

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/gateway/control_server.hpp"
#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/protocol/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geoworld::gateway {

struct TransportConfig {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t port{50052};
    std::string stream_path{std::string(default_stream_path)};
    // 每连接传输写缓冲上限；满时字节留在核心队列内，由核心按合并/背压规则处理。
    std::size_t max_write_buffer_bytes{4U * 1024U * 1024U};
    // 非 loopback 监听必须配置 TLS；loopback 允许明文用于测试。
    std::optional<TlsCertificateFiles> tls;
};

// Beast WebSocket 数据面：单 io_context，不起线程；组合根每个 tick 调用 poll()
// 驱动 accept/读写回调、从 GatewayCore 取出站字节、关闭 must_disconnect 连接。
// Boost/Beast/OpenSSL 类型不出现在公共接口。
class StreamTransport {
public:
    StreamTransport(GatewayCore& core, protocol::ProtocolLimits limits,
                    TransportConfig config);
    ~StreamTransport();

    StreamTransport(const StreamTransport&) = delete;
    StreamTransport& operator=(const StreamTransport&) = delete;

    // TLS 策略不满足、地址非法或端口占用时返回 false 并填充诊断。
    [[nodiscard]] bool start(std::string& diagnostic);
    void poll();
    void shutdown();

    [[nodiscard]] std::vector<projection::ConnectionId> active_connections() const;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::gateway
