#pragma once

// persistence::DurableLog 到 gateway DurableAdmissionLog 的桥接。
// 仅在 GW_BUILD_M5 下编译进 geoworld_gateway（CMake 条件源文件）；
// 该头是 gateway 公共头中唯一暴露 persistence 类型的入口。

#include "geoworld/gateway/durable.hpp"
#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/persistence/durable_log.hpp"
#include "geoworld/persistence/wal.hpp"

#include <memory>

namespace geoworld::gateway {

// 桥接持久化实现：append 立即失败（队列满/故障态/关闭中）映射为 nullptr。
[[nodiscard]] std::shared_ptr<DurableAdmissionLog> make_persistence_admission_log(
    std::shared_ptr<persistence::DurableLog> log);

// 用 WAL scan 结果重建 GatewayCore 的持久幂等索引：只回放
// external_command/command_outcome 记录，其余记录类型跳过；
// 记录损坏返回 false，调用方必须 fail-closed。
[[nodiscard]] bool restore_durable_index(GatewayCore& core,
                                         const persistence::WalScanResult& scan);

} // namespace geoworld::gateway
