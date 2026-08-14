#pragma once

#include "geoworld/gateway/errors.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace geoworld::gateway {

// M4 具名配置基线，默认值冻结于 docs/M4.md 配置基线表。
// 非法配置必须使启动失败并输出明确诊断，不允许静默回退。
struct GatewayConfig {
    std::size_t max_state_queue_bytes{4U * 1024U * 1024U};
    std::size_t max_reliable_queue_bytes{1U * 1024U * 1024U};
    std::uint32_t ack_timeout_seconds{10};
    std::uint32_t heartbeat_interval_seconds{1};
    std::uint32_t stream_ticket_ttl_seconds{30};
    std::uint32_t command_rate_per_session{100};
    std::uint64_t command_lead_ticks{1};
    std::uint64_t max_command_lead_ticks{5};
    std::uint32_t gateway_io_threads{2};
    std::uint32_t encoding_workers{4};
    std::size_t max_sessions{1024};
    std::size_t max_command_parameters{64};
    double max_aoi_extent_meters{100'000.0};
    // ingress 序列起始值：仅测试注入回绕边界使用，生产保持 0（首条命令分配 1）。
    std::uint64_t initial_ingress_sequence{0};
};

[[nodiscard]] bool validate(const GatewayConfig& config, std::string& diagnostic);

} // namespace geoworld::gateway
