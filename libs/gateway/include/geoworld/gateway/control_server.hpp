#pragma once

#include "geoworld/gateway/gateway_core.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace geoworld::gateway {

// TLS 证书文件对；控制面与数据面共用。非 loopback 监听必须提供。
struct TlsCertificateFiles {
    std::string certificate_chain_file;
    std::string private_key_file;
};

// loopback 判定只识别字面回环地址与 localhost；空地址或通配地址不视为 loopback。
[[nodiscard]] bool is_loopback_address(std::string_view address);

struct ControlServerConfig {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t port{50051};
    // OpenSession 响应中告知客户端的数据面端点。
    std::string advertised_data_endpoint{"ws://127.0.0.1:50052/stream"};
    std::vector<std::string> capabilities{"observe", "command.set_property",
                                          "command.create_object", "command.destroy_object"};
    // 权威 tick 来源；RPC 在主线程执行时读取，不引入墙钟。
    std::function<std::uint64_t()> tick_source;
    // gRPC 处理器线程等待主线程执行结果的上限；超时向客户端返回 UNAVAILABLE。
    std::chrono::milliseconds dispatch_timeout{10'000};
    // 非 loopback 监听必须配置 TLS；loopback 允许明文用于测试。
    std::optional<TlsCertificateFiles> tls;
};

// gRPC 控制面：8 个 RPC 的实现封送到组合根主线程执行（poll），
// GatewayCore 不从 gRPC 工作线程访问，跨线程只传递请求/响应 DTO。
// 生成代码与 grpc:: 类型不出现在本头。
class ControlServer {
public:
    ControlServer(GatewayCore& core, ControlServerConfig config);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // TLS 策略不满足或端口占用时返回 false 并填充诊断。
    [[nodiscard]] bool start(std::string& diagnostic);
    // 主循环每个 tick 调用：在调用线程执行排队的 RPC 请求并回传结果。
    void poll();
    void shutdown();

    [[nodiscard]] std::uint16_t bound_port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::gateway
