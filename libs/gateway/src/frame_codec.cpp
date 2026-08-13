#include "geoworld/gateway/frame_codec.hpp"

#include "geoworld/gateway/errors.hpp"
#include "geoworld/protocol/version.hpp"

#include "world_stream_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <variant>
#include <vector>

namespace geoworld::gateway {
namespace {

namespace fb = geoworld::stream::v1;

[[nodiscard]] protocol::WireValue to_wire_value(const world::PropertyValue& value) {
    // schema::Value 与 WireValue 的 variant 备选完全一致（int64/double/bool/string）。
    return std::visit([](const auto& inner) -> protocol::WireValue { return inner; }, value);
}

[[nodiscard]] protocol::WireAttributes to_wire_attributes(const world::PropertyBag& bag) {
    protocol::WireAttributes attributes;
    for (const auto& [key, value] : bag) {
        attributes.emplace(key, to_wire_value(value));
    }
    return attributes;
}

[[nodiscard]] protocol::Lifecycle to_wire_lifecycle(world::LifecycleState lifecycle) {
    // 两侧枚举序数一致，冻结于 docs/M4.md 投影实体契约。
    return static_cast<protocol::Lifecycle>(static_cast<std::uint8_t>(lifecycle));
}

[[nodiscard]] protocol::FrequencyClass to_wire_frequency(
    projection::FrequencyClass frequency) {
    return static_cast<protocol::FrequencyClass>(static_cast<std::uint8_t>(frequency));
}

[[nodiscard]] protocol::LeaveReason to_wire_leave_reason(projection::LeaveReason reason) {
    return static_cast<protocol::LeaveReason>(static_cast<std::uint8_t>(reason));
}

[[nodiscard]] protocol::WireEnter to_wire_enter(const projection::EntityEnter& enter) {
    return protocol::WireEnter{enter.wid, enter.fid, to_wire_entity(enter.entity)};
}

[[nodiscard]] FrameBytes to_frame_bytes(std::vector<std::uint8_t> encoded) {
    FrameBytes bytes(encoded.size());
    if (!encoded.empty()) {
        std::memcpy(bytes.data(), encoded.data(), encoded.size());
    }
    return bytes;
}

} // namespace

[[nodiscard]] protocol::WireEntity to_wire_entity(const projection::ProjectedEntity& entity) {
    protocol::WireEntity wire;
    wire.wid = entity.wid;
    wire.fid = entity.fid;
    wire.version = entity.version;
    wire.position = protocol::WireVec3{entity.position.x, entity.position.y,
                                       entity.position.z};
    wire.semantic_type = entity.semantic_type;
    wire.geometry_ref = entity.geometry_ref;
    wire.lifecycle = to_wire_lifecycle(entity.lifecycle);
    wire.properties = to_wire_attributes(entity.properties);
    wire.state = to_wire_attributes(entity.state);
    wire.relations.reserve(entity.relations.size());
    for (const world::Relation& relation : entity.relations) {
        // fbs RelationEntry 只携带 type 与 target_wid，不携带 relation attributes。
        wire.relations.push_back(
            protocol::WireRelation{relation.type, relation.target.value});
    }
    wire.capabilities = entity.capabilities;
    wire.frequency = to_wire_frequency(entity.metadata.frequency);
    wire.priority = entity.metadata.priority;
    wire.visibility_tags = entity.metadata.visibility_tags;
    return wire;
}

[[nodiscard]] protocol::WireFrame to_wire_frame(const projection::StateFrame& frame) {
    if (const auto* keyframe = std::get_if<projection::Keyframe>(&frame)) {
        protocol::WireKeyframe wire;
        wire.stream_epoch = keyframe->stream_epoch;
        wire.stream_sequence = keyframe->stream_sequence;
        wire.snapshot_id = keyframe->snapshot_id;
        wire.baseline_snapshot_id = keyframe->baseline_snapshot_id;
        wire.tick = keyframe->tick;
        wire.simulation_time_us = static_cast<std::int64_t>(keyframe->simulation_time_us);
        wire.entities.reserve(keyframe->entities.size());
        for (const projection::EntityEnter& enter : keyframe->entities) {
            wire.entities.push_back(to_wire_enter(enter));
        }
        return protocol::WireFrame{std::move(wire)};
    }

    const auto& delta = std::get<projection::Delta>(frame);
    protocol::WireDelta wire;
    wire.stream_epoch = delta.stream_epoch;
    wire.stream_sequence = delta.stream_sequence;
    wire.snapshot_id = delta.snapshot_id;
    wire.baseline_snapshot_id = delta.baseline_snapshot_id;
    wire.tick = delta.tick;
    wire.simulation_time_us = static_cast<std::int64_t>(delta.simulation_time_us);
    wire.enters.reserve(delta.enters.size());
    for (const projection::EntityEnter& enter : delta.enters) {
        wire.enters.push_back(to_wire_enter(enter));
    }
    wire.updates.reserve(delta.updates.size());
    for (const projection::EntityUpdate& update : delta.updates) {
        wire.updates.push_back(protocol::WireUpdate{update.wid, update.fid,
                                                    to_wire_entity(update.entity)});
    }
    wire.leaves.reserve(delta.leaves.size());
    for (const projection::EntityLeave& leave : delta.leaves) {
        wire.leaves.push_back(protocol::WireLeave{leave.wid, leave.fid,
                                                  to_wire_leave_reason(leave.reason)});
    }
    return protocol::WireFrame{std::move(wire)};
}

// ---- 热路径直接编码：由 projection 类型直接构建 FlatBuffers ----
// 下列函数与 protocol codec.cpp 的 encode_* 保持逐语句同构（相同 builder 调用
// 序列、相同共享字符串策略、相同 limits 判定），输出字节与
// encode_server_frame_into(to_wire_frame(frame)) 完全一致；该等价性由
// gateway 单元测试逐字节对拍锁定。镜像只读 projection 类型，省去 wire 中间
// 结构的全部深拷贝与分配。

[[nodiscard]] bool direct_text_within_limits(std::size_t size,
                                             const protocol::ProtocolLimits& limits) {
    return size <= limits.max_string_bytes;
}

[[nodiscard]] bool direct_encode_bag(
    flatbuffers::FlatBufferBuilder& builder, const world::PropertyBag& bag,
    const protocol::ProtocolLimits& limits,
    std::vector<flatbuffers::Offset<fb::PropertyEntry>>& out) {
    if (bag.size() > limits.max_properties_per_entity) {
        return false;
    }
    out.reserve(bag.size());
    for (const auto& [key, value] : bag) {
        if (!direct_text_within_limits(key.size(), limits)) {
            return false;
        }
        fb::PropertyValue payload_type = fb::PropertyValue::NONE;
        flatbuffers::Offset<void> payload;
        switch (value.index()) {
        case 0: {
            payload_type = fb::PropertyValue::IntValue;
            payload = fb::CreateIntValue(builder, std::get<std::int64_t>(value)).Union();
            break;
        }
        case 1: {
            payload_type = fb::PropertyValue::DoubleValue;
            payload = fb::CreateDoubleValue(builder, std::get<double>(value)).Union();
            break;
        }
        case 2: {
            payload_type = fb::PropertyValue::BoolValue;
            payload = fb::CreateBoolValue(builder, std::get<bool>(value)).Union();
            break;
        }
        case 3: {
            const auto& text = std::get<std::string>(value);
            if (!direct_text_within_limits(text.size(), limits)) {
                return false;
            }
            payload_type = fb::PropertyValue::StringValue;
            payload = fb::CreateStringValue(builder, builder.CreateSharedString(text)).Union();
            break;
        }
        default:
            return false;
        }
        out.push_back(fb::CreatePropertyEntry(builder, builder.CreateSharedString(key),
                                              payload_type, payload));
    }
    return true;
}

[[nodiscard]] bool direct_encode_strings(
    flatbuffers::FlatBufferBuilder& builder, const std::vector<std::string>& source,
    const protocol::ProtocolLimits& limits,
    std::vector<flatbuffers::Offset<flatbuffers::String>>& out) {
    out.reserve(source.size());
    for (const auto& text : source) {
        if (!direct_text_within_limits(text.size(), limits)) {
            return false;
        }
        out.push_back(builder.CreateSharedString(text));
    }
    return true;
}

[[nodiscard]] bool direct_encode_entity(
    flatbuffers::FlatBufferBuilder& builder, const projection::ProjectedEntity& entity,
    const protocol::ProtocolLimits& limits,
    flatbuffers::Offset<fb::ProjectedEntity>& out) {
    // 偏移暂存线程内复用：仅作为 CreateVector 的输入，内容在写入 builder 后失效，
    // 不影响输出字节；省去逐实体堆分配（热路径每 tick 数十万级）。
    thread_local std::vector<flatbuffers::Offset<fb::PropertyEntry>> properties;
    thread_local std::vector<flatbuffers::Offset<fb::PropertyEntry>> state;
    thread_local std::vector<flatbuffers::Offset<fb::RelationEntry>> relations;
    thread_local std::vector<flatbuffers::Offset<flatbuffers::String>> capabilities;
    thread_local std::vector<flatbuffers::Offset<flatbuffers::String>> visibility_tags;
    properties.clear();
    state.clear();
    relations.clear();
    capabilities.clear();
    visibility_tags.clear();
    if (!direct_text_within_limits(entity.semantic_type.size(), limits)
        || !direct_text_within_limits(entity.geometry_ref.size(), limits)
        || entity.relations.size() > limits.max_relations_per_entity) {
        return false;
    }
    if (!direct_encode_bag(builder, entity.properties, limits, properties)
        || !direct_encode_bag(builder, entity.state, limits, state)) {
        return false;
    }
    relations.reserve(entity.relations.size());
    for (const auto& relation : entity.relations) {
        if (!direct_text_within_limits(relation.type.size(), limits)) {
            return false;
        }
        relations.push_back(fb::CreateRelationEntry(
            builder, builder.CreateSharedString(relation.type), relation.target.value));
    }
    if (!direct_encode_strings(builder, entity.capabilities, limits, capabilities)
        || !direct_encode_strings(builder, entity.metadata.visibility_tags, limits,
                                  visibility_tags)) {
        return false;
    }
    const auto position = fb::CreateVec3Ecef(builder, entity.position.x, entity.position.y,
                                             entity.position.z);
    out = fb::CreateProjectedEntity(
        builder, entity.wid.value, entity.fid.value, entity.version, position,
        builder.CreateSharedString(entity.semantic_type),
        // geometry_ref 是逐实体唯一的资产路径，入共享池只有哈希开销没有去重收益。
        builder.CreateString(entity.geometry_ref),
        static_cast<fb::Lifecycle>(static_cast<std::uint8_t>(entity.lifecycle)),
        builder.CreateVector(properties), builder.CreateVector(state),
        builder.CreateVector(relations), builder.CreateVector(capabilities),
        static_cast<fb::FrequencyClass>(
            static_cast<std::uint8_t>(entity.metadata.frequency)),
        entity.metadata.priority, builder.CreateVector(visibility_tags));
    return true;
}

[[nodiscard]] bool direct_encode_enter(
    flatbuffers::FlatBufferBuilder& builder, const projection::EntityEnter& enter,
    const protocol::ProtocolLimits& limits,
    flatbuffers::Offset<fb::EntityEnter>& out) {
    flatbuffers::Offset<fb::ProjectedEntity> entity;
    if (!direct_encode_entity(builder, enter.entity, limits, entity)) {
        return false;
    }
    out = fb::CreateEntityEnter(builder, enter.wid.value, enter.fid.value, entity);
    return true;
}

[[nodiscard]] bool direct_encode_update(
    flatbuffers::FlatBufferBuilder& builder, const projection::EntityUpdate& update,
    const protocol::ProtocolLimits& limits,
    flatbuffers::Offset<fb::EntityUpdate>& out) {
    flatbuffers::Offset<fb::ProjectedEntity> entity;
    if (!direct_encode_entity(builder, update.entity, limits, entity)) {
        return false;
    }
    out = fb::CreateEntityUpdate(builder, update.wid.value, update.fid.value, entity);
    return true;
}

[[nodiscard]] bool direct_encode_enters(
    flatbuffers::FlatBufferBuilder& builder,
    const std::vector<projection::EntityEnter>& enters,
    const protocol::ProtocolLimits& limits,
    std::vector<flatbuffers::Offset<fb::EntityEnter>>& out) {
    out.reserve(enters.size());
    for (const auto& enter : enters) {
        flatbuffers::Offset<fb::EntityEnter> offset;
        if (!direct_encode_enter(builder, enter, limits, offset)) {
            return false;
        }
        out.push_back(offset);
    }
    return true;
}

[[nodiscard]] flatbuffers::Offset<fb::WorldStreamFrame> direct_encode_keyframe(
    flatbuffers::FlatBufferBuilder& builder, const projection::Keyframe& keyframe,
    const protocol::ProtocolLimits& limits, bool& ok) {
    std::vector<flatbuffers::Offset<fb::EntityEnter>> entities;
    ok = keyframe.entities.size() <= limits.max_entities_per_frame
        && direct_encode_enters(builder, keyframe.entities, limits, entities);
    if (!ok) {
        return {0};
    }
    const auto frame = fb::CreateKeyframeFrame(
        builder, keyframe.stream_epoch, keyframe.stream_sequence, keyframe.snapshot_id,
        keyframe.baseline_snapshot_id, keyframe.tick,
        static_cast<std::int64_t>(keyframe.simulation_time_us),
        builder.CreateVector(entities));
    return fb::CreateWorldStreamFrame(builder, protocol::data_schema_version,
                                      fb::ServerFrame::KeyframeFrame, frame.Union());
}

[[nodiscard]] flatbuffers::Offset<fb::WorldStreamFrame> direct_encode_delta(
    flatbuffers::FlatBufferBuilder& builder, const projection::Delta& delta,
    const protocol::ProtocolLimits& limits, bool& ok) {
    const std::size_t total = delta.enters.size() + delta.updates.size()
        + delta.leaves.size();
    std::vector<flatbuffers::Offset<fb::EntityEnter>> enters;
    std::vector<flatbuffers::Offset<fb::EntityUpdate>> updates;
    std::vector<flatbuffers::Offset<fb::EntityLeave>> leaves;
    ok = total <= limits.max_entities_per_frame
        && direct_encode_enters(builder, delta.enters, limits, enters);
    if (ok) {
        updates.reserve(delta.updates.size());
        for (const auto& update : delta.updates) {
            flatbuffers::Offset<fb::EntityUpdate> offset;
            if (!direct_encode_update(builder, update, limits, offset)) {
                ok = false;
                break;
            }
            updates.push_back(offset);
        }
    }
    if (ok) {
        leaves.reserve(delta.leaves.size());
        for (const auto& leave : delta.leaves) {
            leaves.push_back(fb::CreateEntityLeave(
                builder, leave.wid.value, leave.fid.value,
                static_cast<fb::LeaveReason>(static_cast<std::uint8_t>(leave.reason))));
        }
    }
    if (!ok) {
        return {0};
    }
    const auto frame = fb::CreateDeltaFrame(
        builder, delta.stream_epoch, delta.stream_sequence, delta.snapshot_id,
        delta.baseline_snapshot_id, delta.tick,
        static_cast<std::int64_t>(delta.simulation_time_us),
        builder.CreateVector(enters), builder.CreateVector(updates),
        builder.CreateVector(leaves));
    return fb::CreateWorldStreamFrame(builder, protocol::data_schema_version,
                                      fb::ServerFrame::DeltaFrame, frame.Union());
}

[[nodiscard]] bool encode_state_frame_into(const projection::StateFrame& frame,
                                           const protocol::ProtocolLimits& limits,
                                           flatbuffers::FlatBufferBuilder& builder) {
    builder.Reset();
    bool ok = true;
    flatbuffers::Offset<fb::WorldStreamFrame> root;
    if (const auto* keyframe = std::get_if<projection::Keyframe>(&frame)) {
        root = direct_encode_keyframe(builder, *keyframe, limits, ok);
    } else {
        root = direct_encode_delta(builder, std::get<projection::Delta>(frame), limits,
                                   ok);
    }
    if (!ok) {
        return false;
    }
    fb::FinishWorldStreamFrameBuffer(builder, root);
    return true;
}

[[nodiscard]] protocol::WireCommandReceipt to_wire_receipt(const CommandReceipt& receipt) {
    protocol::WireCommandReceipt wire;
    wire.client_sequence = receipt.client_sequence;
    switch (receipt.status) {
    case ReceiptStatus::accepted:
        wire.status = protocol::ReceiptStatus::accepted;
        break;
    case ReceiptStatus::applied:
        wire.status = protocol::ReceiptStatus::applied;
        break;
    case ReceiptStatus::rejected:
        wire.status = protocol::ReceiptStatus::rejected;
        break;
    case ReceiptStatus::duplicate:
        wire.status = protocol::ReceiptStatus::duplicate;
        break;
    }
    wire.error_code = error_code(receipt.error);
    return wire;
}

[[nodiscard]] GatewayCore::FrameEncoder make_frame_encoder(protocol::ProtocolLimits limits) {
    return [limits = std::move(limits)](const projection::StateFrame& frame) {
        // 热路径：线程内复用 builder 避免逐帧重新扩容；Reset 后编码输出与新建
        // builder 逐字节一致。pump 并行编码时每个工作线程持有独立实例。
        thread_local flatbuffers::FlatBufferBuilder builder;
        if (!encode_state_frame_into(frame, limits, builder)) {
            return FrameBytes{};
        }
        FrameBytes bytes(builder.GetSize());
        if (builder.GetSize() > 0) {
            std::memcpy(bytes.data(), builder.GetBufferPointer(), builder.GetSize());
        }
        return bytes;
    };
}

[[nodiscard]] GatewayCore::ReceiptEncoder make_receipt_encoder(
    protocol::ProtocolLimits limits) {
    return [limits = std::move(limits)](const CommandReceipt& receipt) {
        protocol::WireReliable reliable;
        reliable.kind = protocol::ReliableKind::command_receipt;
        reliable.receipt = to_wire_receipt(receipt);
        return to_frame_bytes(
            protocol::encode_server_frame(protocol::WireFrame{std::move(reliable)}, limits));
    };
}

[[nodiscard]] GatewayCore::HeartbeatEncoder make_heartbeat_encoder(
    protocol::ProtocolLimits limits) {
    return [limits = std::move(limits)](std::uint64_t tick, std::uint64_t snapshot_id) {
        return to_frame_bytes(protocol::encode_server_frame(
            protocol::WireFrame{protocol::WireHeartbeat{tick, snapshot_id}}, limits));
    };
}

[[nodiscard]] FrameBytes encode_protocol_error(std::string_view code,
                                               std::string_view message,
                                               const protocol::ProtocolLimits& limits) {
    protocol::WireReliable reliable;
    reliable.kind = protocol::ReliableKind::protocol_error;
    reliable.error = protocol::WireProtocolError{std::string(code), std::string(message)};
    return to_frame_bytes(
        protocol::encode_server_frame(protocol::WireFrame{std::move(reliable)}, limits));
}

} // namespace geoworld::gateway
