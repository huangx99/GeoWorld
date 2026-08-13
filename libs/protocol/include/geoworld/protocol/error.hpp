#pragma once

#include <string_view>

namespace geoworld::protocol {

// 稳定错误码第一批，含义逐字冻结于 docs/M4.md 错误码表；
// 后续只能追加，不能复用或改变语义。
inline constexpr std::string_view error_version_incompatible = "GWG001";  // 协议或 schema 版本不兼容
inline constexpr std::string_view error_authentication_failed = "GWG002"; // 认证失败
inline constexpr std::string_view error_permission_denied = "GWG003";     // 权限或属性所有权不足
inline constexpr std::string_view error_rate_limited = "GWG004";          // 请求或命令超过速率限制
inline constexpr std::string_view error_invalid_request = "GWG005";       // 请求字段、枚举或状态转换无效
inline constexpr std::string_view error_limit_exceeded = "GWG006";        // 消息、字段或队列超过配置上限
inline constexpr std::string_view error_ticket_invalid = "GWG101";        // stream ticket 无效、过期或已使用
inline constexpr std::string_view error_epoch_mismatch = "GWG102";        // stream epoch 不匹配
inline constexpr std::string_view error_unknown_snapshot_ack = "GWG103";  // ack 指向未来、未知或未发送的 snapshot
inline constexpr std::string_view error_baseline_unavailable = "GWG104";  // delta 基线不可用，需要 keyframe
inline constexpr std::string_view error_slow_client = "GWG105";           // 慢客户端超过可靠队列或超时上限
inline constexpr std::string_view error_version_conflict = "GWG201";      // 对象版本冲突
inline constexpr std::string_view error_target_missing = "GWG202";        // 目标对象或属性不存在
inline constexpr std::string_view error_tick_out_of_window = "GWG203";    // 目标 tick 超出允许窗口
inline constexpr std::string_view error_operation_unsupported = "GWG204"; // 命令 operation 不受支持

} // namespace geoworld::protocol
