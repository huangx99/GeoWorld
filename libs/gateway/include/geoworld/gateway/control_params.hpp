#pragma once

#include <string_view>

namespace geoworld::gateway {

// 控制面 SubmitCommand 参数映射约定（geoworld_control.proto 冻结后不能加字段，
// 复合参数经 parameters map 的具名键表达）。服务端与参考客户端共享这些常量。
// SET_PROPERTY：parameters 恰好一个条目，键即属性键，值即属性值。
// CREATE_OBJECT：下列具名键表达语义类型、几何引用与 ECEF 位置，其余条目为初始属性。
inline constexpr std::string_view create_param_semantic_type = "semantic_type";
inline constexpr std::string_view create_param_geometry_ref = "geometry_ref";
inline constexpr std::string_view create_param_position_x = "ecef_x";
inline constexpr std::string_view create_param_position_y = "ecef_y";
inline constexpr std::string_view create_param_position_z = "ecef_z";

// WebSocket 升级请求目标中携带一次性 stream ticket 的查询键。
inline constexpr std::string_view stream_ticket_query_key = "ticket";
// 数据面默认监听路径，可在 TransportConfig 中覆盖。
inline constexpr std::string_view default_stream_path = "/stream";

} // namespace geoworld::gateway
