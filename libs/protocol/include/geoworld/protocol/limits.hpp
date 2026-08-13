#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace geoworld::protocol {

// 协议输入上限基线。max_frame_bytes 冻结于 docs/M4.md 配置基线（8 MiB）；
// 其余默认值来自安全基线的有界输入要求，需覆盖 R4 标准负载
//（单连接可见 2,000 实体、世界 100,000 实体），部署可收紧但不得改变协议语义。
struct ProtocolLimits {
    std::size_t max_frame_bytes{8U * 1024U * 1024U};
    std::size_t max_string_bytes{64U * 1024U};
    std::size_t max_properties_per_entity{256}; // properties 与 state 各自受限
    std::size_t max_relations_per_entity{256};
    std::size_t max_entities_per_frame{100'000};
    // flatbuffers::Verifier 的深度与表数量上限，与库默认值保持一致。
    std::uint32_t verifier_max_depth{64};
    std::uint32_t verifier_max_tables{1'000'000};
};

// 非法配置（任何 0 值）必须被拒绝并给出诊断，不允许静默回退固定值。
[[nodiscard]] inline bool validate(const ProtocolLimits& limits, std::string& diagnostic) {
    if (limits.max_frame_bytes == 0) {
        diagnostic = "max_frame_bytes 不能为 0";
        return false;
    }
    if (limits.max_string_bytes == 0) {
        diagnostic = "max_string_bytes 不能为 0";
        return false;
    }
    if (limits.max_properties_per_entity == 0) {
        diagnostic = "max_properties_per_entity 不能为 0";
        return false;
    }
    if (limits.max_relations_per_entity == 0) {
        diagnostic = "max_relations_per_entity 不能为 0";
        return false;
    }
    if (limits.max_entities_per_frame == 0) {
        diagnostic = "max_entities_per_frame 不能为 0";
        return false;
    }
    if (limits.verifier_max_depth == 0) {
        diagnostic = "verifier_max_depth 不能为 0";
        return false;
    }
    if (limits.verifier_max_tables == 0) {
        diagnostic = "verifier_max_tables 不能为 0";
        return false;
    }
    diagnostic.clear();
    return true;
}

} // namespace geoworld::protocol
