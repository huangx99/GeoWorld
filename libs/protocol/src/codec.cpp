#include "geoworld/protocol/codec.hpp"

#include "geoworld/protocol/error.hpp"
#include "geoworld/protocol/version.hpp"

#include "client_stream_generated.h"
#include "world_stream_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <utility>

namespace geoworld::protocol {

namespace {

namespace fb = geoworld::stream::v1;

// 最小合法帧：root uoffset + 4 字节 file identifier。
constexpr std::size_t kMinFrameBytes = 8;

bool fail(DecodeFailure& failure, std::string_view code, std::string message) {
    failure.error_code = code;
    failure.message = std::move(message);
    return false;
}

[[nodiscard]] bool text_within_limits(std::size_t size, const ProtocolLimits& limits) {
    return size <= limits.max_string_bytes;
}

// ---- 编码 ----

[[nodiscard]] bool encode_bag(flatbuffers::FlatBufferBuilder& builder, const WireAttributes& bag,
                              const ProtocolLimits& limits,
                              std::vector<flatbuffers::Offset<fb::PropertyEntry>>& out) {
    if (bag.size() > limits.max_properties_per_entity) {
        return false;
    }
    out.reserve(bag.size());
    for (const auto& [key, value] : bag) {
        if (!text_within_limits(key.size(), limits)) {
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
            if (!text_within_limits(text.size(), limits)) {
                return false;
            }
            payload_type = fb::PropertyValue::StringValue;
            payload = fb::CreateStringValue(builder, builder.CreateSharedString(text)).Union();
            break;
        }
        default:
            return false;
        }
        // 帧内重复 key 用共享字符串：字节仍逐帧确定，解码语义不变，体积更小。
        out.push_back(fb::CreatePropertyEntry(builder, builder.CreateSharedString(key),
                                              payload_type, payload));
    }
    return true;
}

[[nodiscard]] bool encode_strings(flatbuffers::FlatBufferBuilder& builder,
                                  const std::vector<std::string>& source,
                                  const ProtocolLimits& limits,
                                  std::vector<flatbuffers::Offset<flatbuffers::String>>& out) {
    out.reserve(source.size());
    for (const auto& text : source) {
        if (!text_within_limits(text.size(), limits)) {
            return false;
        }
        out.push_back(builder.CreateSharedString(text));
    }
    return true;
}

[[nodiscard]] bool encode_entity(flatbuffers::FlatBufferBuilder& builder, const WireEntity& entity,
                                 const ProtocolLimits& limits,
                                 flatbuffers::Offset<fb::ProjectedEntity>& out) {
    if (!text_within_limits(entity.semantic_type.size(), limits)
        || !text_within_limits(entity.geometry_ref.size(), limits)
        || entity.relations.size() > limits.max_relations_per_entity) {
        return false;
    }
    std::vector<flatbuffers::Offset<fb::PropertyEntry>> properties;
    std::vector<flatbuffers::Offset<fb::PropertyEntry>> state;
    if (!encode_bag(builder, entity.properties, limits, properties)
        || !encode_bag(builder, entity.state, limits, state)) {
        return false;
    }
    std::vector<flatbuffers::Offset<fb::RelationEntry>> relations;
    relations.reserve(entity.relations.size());
    for (const auto& relation : entity.relations) {
        if (!text_within_limits(relation.type.size(), limits)) {
            return false;
        }
        relations.push_back(fb::CreateRelationEntry(
            builder, builder.CreateSharedString(relation.type), relation.target_wid));
    }
    std::vector<flatbuffers::Offset<flatbuffers::String>> capabilities;
    std::vector<flatbuffers::Offset<flatbuffers::String>> visibility_tags;
    if (!encode_strings(builder, entity.capabilities, limits, capabilities)
        || !encode_strings(builder, entity.visibility_tags, limits, visibility_tags)) {
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
        static_cast<fb::FrequencyClass>(static_cast<std::uint8_t>(entity.frequency)),
        entity.priority, builder.CreateVector(visibility_tags));
    return true;
}

[[nodiscard]] bool encode_enter(flatbuffers::FlatBufferBuilder& builder, const WireEnter& enter,
                                const ProtocolLimits& limits,
                                flatbuffers::Offset<fb::EntityEnter>& out) {
    flatbuffers::Offset<fb::ProjectedEntity> entity;
    if (!encode_entity(builder, enter.entity, limits, entity)) {
        return false;
    }
    out = fb::CreateEntityEnter(builder, enter.wid.value, enter.fid.value, entity);
    return true;
}

[[nodiscard]] bool encode_update(flatbuffers::FlatBufferBuilder& builder, const WireUpdate& update,
                                 const ProtocolLimits& limits,
                                 flatbuffers::Offset<fb::EntityUpdate>& out) {
    flatbuffers::Offset<fb::ProjectedEntity> entity;
    if (!encode_entity(builder, update.entity, limits, entity)) {
        return false;
    }
    out = fb::CreateEntityUpdate(builder, update.wid.value, update.fid.value, entity);
    return true;
}

[[nodiscard]] bool encode_enters(flatbuffers::FlatBufferBuilder& builder,
                                 const std::vector<WireEnter>& enters,
                                 const ProtocolLimits& limits,
                                 std::vector<flatbuffers::Offset<fb::EntityEnter>>& out) {
    out.reserve(enters.size());
    for (const auto& enter : enters) {
        flatbuffers::Offset<fb::EntityEnter> offset;
        if (!encode_enter(builder, enter, limits, offset)) {
            return false;
        }
        out.push_back(offset);
    }
    return true;
}

[[nodiscard]] flatbuffers::Offset<fb::WorldStreamFrame> encode_keyframe(
    flatbuffers::FlatBufferBuilder& builder, const WireKeyframe& keyframe,
    const ProtocolLimits& limits, bool& ok) {
    std::vector<flatbuffers::Offset<fb::EntityEnter>> entities;
    ok = keyframe.entities.size() <= limits.max_entities_per_frame
        && encode_enters(builder, keyframe.entities, limits, entities);
    if (!ok) {
        return {0};
    }
    const auto frame = fb::CreateKeyframeFrame(
        builder, keyframe.stream_epoch, keyframe.stream_sequence, keyframe.snapshot_id,
        keyframe.baseline_snapshot_id, keyframe.tick, keyframe.simulation_time_us,
        builder.CreateVector(entities));
    return fb::CreateWorldStreamFrame(builder, data_schema_version,
                                      fb::ServerFrame::KeyframeFrame, frame.Union());
}

[[nodiscard]] flatbuffers::Offset<fb::WorldStreamFrame> encode_delta(
    flatbuffers::FlatBufferBuilder& builder, const WireDelta& delta,
    const ProtocolLimits& limits, bool& ok) {
    const std::size_t total = delta.enters.size() + delta.updates.size() + delta.leaves.size();
    std::vector<flatbuffers::Offset<fb::EntityEnter>> enters;
    std::vector<flatbuffers::Offset<fb::EntityUpdate>> updates;
    std::vector<flatbuffers::Offset<fb::EntityLeave>> leaves;
    ok = total <= limits.max_entities_per_frame
        && encode_enters(builder, delta.enters, limits, enters);
    if (ok) {
        updates.reserve(delta.updates.size());
        for (const auto& update : delta.updates) {
            flatbuffers::Offset<fb::EntityUpdate> offset;
            if (!encode_update(builder, update, limits, offset)) {
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
        delta.baseline_snapshot_id, delta.tick, delta.simulation_time_us,
        builder.CreateVector(enters), builder.CreateVector(updates), builder.CreateVector(leaves));
    return fb::CreateWorldStreamFrame(builder, data_schema_version, fb::ServerFrame::DeltaFrame,
                                      frame.Union());
}

[[nodiscard]] flatbuffers::Offset<fb::WorldStreamFrame> encode_reliable(
    flatbuffers::FlatBufferBuilder& builder, const WireReliable& reliable,
    const ProtocolLimits& limits, bool& ok) {
    flatbuffers::Offset<fb::CommandReceipt> receipt;
    flatbuffers::Offset<fb::ProtocolError> error;
    ok = true;
    if (reliable.kind == ReliableKind::command_receipt) {
        if (!text_within_limits(reliable.receipt.error_code.size(), limits)) {
            ok = false;
            return {0};
        }
        receipt = fb::CreateCommandReceipt(
            builder, reliable.receipt.client_sequence,
            static_cast<fb::ReceiptStatus>(static_cast<std::uint8_t>(reliable.receipt.status)),
            builder.CreateString(reliable.receipt.error_code));
    }
    if (reliable.kind == ReliableKind::protocol_error) {
        if (!text_within_limits(reliable.error.code.size(), limits)
            || !text_within_limits(reliable.error.message.size(), limits)) {
            ok = false;
            return {0};
        }
        error = fb::CreateProtocolError(builder, builder.CreateString(reliable.error.code),
                                        builder.CreateString(reliable.error.message));
    }
    const auto frame = fb::CreateReliableEvent(
        builder, static_cast<fb::ReliableKind>(static_cast<std::uint8_t>(reliable.kind)), receipt,
        error);
    return fb::CreateWorldStreamFrame(builder, data_schema_version, fb::ServerFrame::ReliableEvent,
                                      frame.Union());
}

[[nodiscard]] flatbuffers::Offset<fb::WorldStreamFrame> encode_heartbeat(
    flatbuffers::FlatBufferBuilder& builder, const WireHeartbeat& heartbeat) {
    const auto frame =
        fb::CreateHeartbeatFrame(builder, heartbeat.server_tick, heartbeat.latest_snapshot_id);
    return fb::CreateWorldStreamFrame(builder, data_schema_version, fb::ServerFrame::HeartbeatFrame,
                                      frame.Union());
}

[[nodiscard]] std::vector<std::uint8_t> finish_server_frame(
    flatbuffers::FlatBufferBuilder& builder, flatbuffers::Offset<fb::WorldStreamFrame> root) {
    fb::FinishWorldStreamFrameBuffer(builder, root);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

[[nodiscard]] std::vector<std::uint8_t> finish_client_frame(
    flatbuffers::FlatBufferBuilder& builder, flatbuffers::Offset<fb::ClientStreamFrame> root) {
    fb::FinishClientStreamFrameBuffer(builder, root);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

// ---- 解码 ----

[[nodiscard]] bool decode_bag(const flatbuffers::Vector<flatbuffers::Offset<fb::PropertyEntry>>*
                                  source,
                              const ProtocolLimits& limits, WireAttributes& out,
                              DecodeFailure& failure) {
    if (source == nullptr) {
        return true;
    }
    if (source->size() > limits.max_properties_per_entity) {
        return fail(failure, error_limit_exceeded, "属性数量超过上限");
    }
    for (const auto* entry : *source) {
        if (entry == nullptr || entry->key() == nullptr) {
            return fail(failure, error_invalid_request, "属性条目缺少 key");
        }
        if (!text_within_limits(entry->key()->size(), limits)) {
            return fail(failure, error_limit_exceeded, "属性 key 超过长度上限");
        }
        WireValue value;
        switch (entry->value_type()) {
        case fb::PropertyValue::IntValue: {
            const auto* payload = static_cast<const fb::IntValue*>(entry->value());
            value = payload->value();
            break;
        }
        case fb::PropertyValue::DoubleValue: {
            const auto* payload = static_cast<const fb::DoubleValue*>(entry->value());
            value = payload->value();
            break;
        }
        case fb::PropertyValue::BoolValue: {
            const auto* payload = static_cast<const fb::BoolValue*>(entry->value());
            value = payload->value();
            break;
        }
        case fb::PropertyValue::StringValue: {
            const auto* payload = static_cast<const fb::StringValue*>(entry->value());
            if (payload->value() == nullptr) {
                return fail(failure, error_invalid_request, "字符串属性缺少值");
            }
            if (!text_within_limits(payload->value()->size(), limits)) {
                return fail(failure, error_limit_exceeded, "字符串属性超过长度上限");
            }
            value = payload->value()->str();
            break;
        }
        case fb::PropertyValue::NONE:
        default:
            return fail(failure, error_invalid_request, "未知属性值类型");
        }
        out.emplace(entry->key()->str(), std::move(value));
    }
    return true;
}

[[nodiscard]] bool decode_entity(const fb::ProjectedEntity* source, const ProtocolLimits& limits,
                                 WireEntity& out, DecodeFailure& failure) {
    if (source == nullptr || source->position() == nullptr || source->semantic_type() == nullptr
        || source->geometry_ref() == nullptr) {
        return fail(failure, error_invalid_request, "实体缺少必要字段");
    }
    if (!text_within_limits(source->semantic_type()->size(), limits)
        || !text_within_limits(source->geometry_ref()->size(), limits)) {
        return fail(failure, error_limit_exceeded, "实体字符串超过长度上限");
    }
    const auto lifecycle = static_cast<std::uint8_t>(source->lifecycle());
    if (lifecycle > static_cast<std::uint8_t>(Lifecycle::retired)) {
        return fail(failure, error_invalid_request, "lifecycle 枚举越界");
    }
    const auto frequency = static_cast<std::uint8_t>(source->frequency());
    if (frequency > static_cast<std::uint8_t>(FrequencyClass::fast)) {
        return fail(failure, error_invalid_request, "frequency 枚举越界");
    }

    out.wid = foundation::WorldId{source->wid()};
    out.fid = foundation::FeatureId{source->fid()};
    out.version = source->version();
    out.position = WireVec3{source->position()->x(), source->position()->y(),
                            source->position()->z()};
    out.semantic_type = source->semantic_type()->str();
    out.geometry_ref = source->geometry_ref()->str();
    out.lifecycle = static_cast<Lifecycle>(lifecycle);
    out.frequency = static_cast<FrequencyClass>(frequency);
    out.priority = source->priority();

    if (!decode_bag(source->properties(), limits, out.properties, failure)
        || !decode_bag(source->state(), limits, out.state, failure)) {
        return false;
    }
    if (const auto* relations = source->relations(); relations != nullptr) {
        if (relations->size() > limits.max_relations_per_entity) {
            return fail(failure, error_limit_exceeded, "关系数量超过上限");
        }
        out.relations.reserve(relations->size());
        for (const auto* relation : *relations) {
            if (relation == nullptr || relation->type() == nullptr) {
                return fail(failure, error_invalid_request, "关系缺少 type");
            }
            if (!text_within_limits(relation->type()->size(), limits)) {
                return fail(failure, error_limit_exceeded, "关系 type 超过长度上限");
            }
            out.relations.push_back(WireRelation{relation->type()->str(), relation->target_wid()});
        }
    }
    if (const auto* capabilities = source->capabilities(); capabilities != nullptr) {
        out.capabilities.reserve(capabilities->size());
        for (const auto* capability : *capabilities) {
            if (capability == nullptr) {
                return fail(failure, error_invalid_request, "capability 为空");
            }
            if (!text_within_limits(capability->size(), limits)) {
                return fail(failure, error_limit_exceeded, "capability 超过长度上限");
            }
            out.capabilities.push_back(capability->str());
        }
    }
    if (const auto* tags = source->visibility_tags(); tags != nullptr) {
        out.visibility_tags.reserve(tags->size());
        for (const auto* tag : *tags) {
            if (tag == nullptr) {
                return fail(failure, error_invalid_request, "visibility tag 为空");
            }
            if (!text_within_limits(tag->size(), limits)) {
                return fail(failure, error_limit_exceeded, "visibility tag 超过长度上限");
            }
            out.visibility_tags.push_back(tag->str());
        }
    }
    return true;
}

[[nodiscard]] bool decode_enter(const fb::EntityEnter* source, const ProtocolLimits& limits,
                                WireEnter& out, DecodeFailure& failure) {
    if (source == nullptr) {
        return fail(failure, error_invalid_request, "enter 为空");
    }
    if (!decode_entity(source->entity(), limits, out.entity, failure)) {
        return false;
    }
    out.wid = foundation::WorldId{source->wid()};
    out.fid = foundation::FeatureId{source->fid()};
    return true;
}

[[nodiscard]] bool decode_keyframe(const fb::KeyframeFrame* source, const ProtocolLimits& limits,
                                   WireFrame& out, DecodeFailure& failure) {
    WireKeyframe keyframe;
    keyframe.stream_epoch = source->stream_epoch();
    keyframe.stream_sequence = source->stream_sequence();
    keyframe.snapshot_id = source->snapshot_id();
    keyframe.baseline_snapshot_id = source->baseline_snapshot_id();
    keyframe.tick = source->tick();
    keyframe.simulation_time_us = source->simulation_time_us();
    if (const auto* entities = source->entities(); entities != nullptr) {
        if (entities->size() > limits.max_entities_per_frame) {
            return fail(failure, error_limit_exceeded, "单帧实体数量超过上限");
        }
        keyframe.entities.reserve(entities->size());
        for (const auto* entity : *entities) {
            WireEnter enter;
            if (!decode_enter(entity, limits, enter, failure)) {
                return false;
            }
            keyframe.entities.push_back(std::move(enter));
        }
    }
    out = std::move(keyframe);
    return true;
}

[[nodiscard]] bool decode_delta(const fb::DeltaFrame* source, const ProtocolLimits& limits,
                                WireFrame& out, DecodeFailure& failure) {
    const auto* enters = source->enters();
    const auto* updates = source->updates();
    const auto* leaves = source->leaves();
    const std::size_t total = (enters != nullptr ? enters->size() : 0)
        + (updates != nullptr ? updates->size() : 0) + (leaves != nullptr ? leaves->size() : 0);
    if (total > limits.max_entities_per_frame) {
        return fail(failure, error_limit_exceeded, "单帧实体数量超过上限");
    }
    WireDelta delta;
    delta.stream_epoch = source->stream_epoch();
    delta.stream_sequence = source->stream_sequence();
    delta.snapshot_id = source->snapshot_id();
    delta.baseline_snapshot_id = source->baseline_snapshot_id();
    delta.tick = source->tick();
    delta.simulation_time_us = source->simulation_time_us();
    if (enters != nullptr) {
        delta.enters.reserve(enters->size());
        for (const auto* entity : *enters) {
            WireEnter enter;
            if (!decode_enter(entity, limits, enter, failure)) {
                return false;
            }
            delta.enters.push_back(std::move(enter));
        }
    }
    if (updates != nullptr) {
        delta.updates.reserve(updates->size());
        for (const auto* entity : *updates) {
            if (entity == nullptr) {
                return fail(failure, error_invalid_request, "update 为空");
            }
            WireUpdate update;
            if (!decode_entity(entity->entity(), limits, update.entity, failure)) {
                return false;
            }
            update.wid = foundation::WorldId{entity->wid()};
            update.fid = foundation::FeatureId{entity->fid()};
            delta.updates.push_back(std::move(update));
        }
    }
    if (leaves != nullptr) {
        delta.leaves.reserve(leaves->size());
        for (const auto* entity : *leaves) {
            if (entity == nullptr) {
                return fail(failure, error_invalid_request, "leave 为空");
            }
            const auto reason = static_cast<std::uint8_t>(entity->reason());
            if (reason > static_cast<std::uint8_t>(LeaveReason::policy_changed)) {
                return fail(failure, error_invalid_request, "leave reason 枚举越界");
            }
            delta.leaves.push_back(WireLeave{foundation::WorldId{entity->wid()},
                                             foundation::FeatureId{entity->fid()},
                                             static_cast<LeaveReason>(reason)});
        }
    }
    out = std::move(delta);
    return true;
}

[[nodiscard]] bool decode_reliable(const fb::ReliableEvent* source, WireFrame& out,
                                   DecodeFailure& failure) {
    const auto kind = static_cast<std::uint8_t>(source->kind());
    if (kind > static_cast<std::uint8_t>(ReliableKind::event)) {
        return fail(failure, error_invalid_request, "reliable kind 枚举越界");
    }
    WireReliable reliable;
    reliable.kind = static_cast<ReliableKind>(kind);
    if (const auto* receipt = source->receipt(); receipt != nullptr) {
        const auto status = static_cast<std::uint8_t>(receipt->status());
        if (status > static_cast<std::uint8_t>(ReceiptStatus::duplicate)) {
            return fail(failure, error_invalid_request, "receipt status 枚举越界");
        }
        reliable.receipt.client_sequence = receipt->client_sequence();
        reliable.receipt.status = static_cast<ReceiptStatus>(status);
        if (receipt->error_code() != nullptr) {
            reliable.receipt.error_code = receipt->error_code()->str();
        }
    }
    if (const auto* error = source->error(); error != nullptr) {
        if (error->code() != nullptr) {
            reliable.error.code = error->code()->str();
        }
        if (error->message() != nullptr) {
            reliable.error.message = error->message()->str();
        }
    }
    out = std::move(reliable);
    return true;
}

} // namespace

bool encode_server_frame_into(const WireFrame& frame, const ProtocolLimits& limits,
                              flatbuffers::FlatBufferBuilder& builder) {
    builder.Reset();
    bool ok = true;
    flatbuffers::Offset<fb::WorldStreamFrame> root;
    switch (frame.index()) {
    case 0:
        root = encode_keyframe(builder, std::get<WireKeyframe>(frame), limits, ok);
        break;
    case 1:
        root = encode_delta(builder, std::get<WireDelta>(frame), limits, ok);
        break;
    case 2:
        root = encode_reliable(builder, std::get<WireReliable>(frame), limits, ok);
        break;
    case 3:
        root = encode_heartbeat(builder, std::get<WireHeartbeat>(frame));
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) {
        return false;
    }
    fb::FinishWorldStreamFrameBuffer(builder, root);
    return true;
}

std::vector<std::uint8_t> encode_server_frame(const WireFrame& frame,
                                              const ProtocolLimits& limits) {
    flatbuffers::FlatBufferBuilder builder;
    if (!encode_server_frame_into(frame, limits, builder)) {
        return {};
    }
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

std::vector<std::uint8_t> encode_client_control(const WireClientControl& control,
                                                const ProtocolLimits& limits) {
    flatbuffers::FlatBufferBuilder builder;
    flatbuffers::Offset<fb::ClientStreamFrame> root;
    if (const auto* ack = std::get_if<WireAck>(&control)) {
        const auto frame = fb::CreateAckFrame(builder, ack->stream_epoch, ack->snapshot_id);
        root = fb::CreateClientStreamFrame(builder, data_schema_version,
                                           fb::ClientControl::AckFrame, frame.Union());
    } else if (const auto* request = std::get_if<WireKeyframeRequest>(&control)) {
        if (!text_within_limits(request->reason.size(), limits)) {
            return {};
        }
        const auto frame = fb::CreateKeyframeRequest(builder, builder.CreateString(request->reason));
        root = fb::CreateClientStreamFrame(builder, data_schema_version,
                                           fb::ClientControl::KeyframeRequest, frame.Union());
    } else {
        return {};
    }
    return finish_client_frame(builder, root);
}

std::optional<WireFrame> decode_server_frame(std::span<const std::uint8_t> bytes,
                                             DecodeFailure& failure,
                                             const ProtocolLimits& limits) {
    failure = DecodeFailure{};
    if (bytes.empty()) {
        fail(failure, error_invalid_request, "空帧");
        return std::nullopt;
    }
    if (bytes.size() > limits.max_frame_bytes) {
        fail(failure, error_limit_exceeded, "帧超过 max_frame_bytes");
        return std::nullopt;
    }
    if (bytes.size() < kMinFrameBytes) {
        fail(failure, error_invalid_request, "帧长度过短");
        return std::nullopt;
    }
    if (!fb::WorldStreamFrameBufferHasIdentifier(bytes.data())) {
        fail(failure, error_version_incompatible, "file identifier 不匹配");
        return std::nullopt;
    }
    flatbuffers::Verifier verifier{bytes.data(), bytes.size(), limits.verifier_max_depth,
                                   limits.verifier_max_tables};
    if (!fb::VerifyWorldStreamFrameBuffer(verifier)) {
        fail(failure, error_invalid_request, "FlatBuffers verifier 拒绝");
        return std::nullopt;
    }
    const auto* root = fb::GetWorldStreamFrame(bytes.data());
    if (root->schema_version() != data_schema_version) {
        fail(failure, error_version_incompatible, "schema_version 不兼容");
        return std::nullopt;
    }

    WireFrame frame;
    switch (root->frame_type()) {
    case fb::ServerFrame::KeyframeFrame:
        if (!decode_keyframe(static_cast<const fb::KeyframeFrame*>(root->frame()), limits, frame,
                             failure)) {
            return std::nullopt;
        }
        return frame;
    case fb::ServerFrame::DeltaFrame:
        if (!decode_delta(static_cast<const fb::DeltaFrame*>(root->frame()), limits, frame,
                          failure)) {
            return std::nullopt;
        }
        return frame;
    case fb::ServerFrame::ReliableEvent:
        if (!decode_reliable(static_cast<const fb::ReliableEvent*>(root->frame()), frame,
                             failure)) {
            return std::nullopt;
        }
        return frame;
    case fb::ServerFrame::HeartbeatFrame: {
        const auto* heartbeat = static_cast<const fb::HeartbeatFrame*>(root->frame());
        frame = WireHeartbeat{heartbeat->server_tick(), heartbeat->latest_snapshot_id()};
        return frame;
    }
    case fb::ServerFrame::NONE:
    default:
        fail(failure, error_invalid_request, "未知服务端帧类型");
        return std::nullopt;
    }
}

std::optional<WireClientControl> decode_client_control(std::span<const std::uint8_t> bytes,
                                                       DecodeFailure& failure,
                                                       const ProtocolLimits& limits) {
    failure = DecodeFailure{};
    if (bytes.empty()) {
        fail(failure, error_invalid_request, "空帧");
        return std::nullopt;
    }
    if (bytes.size() > limits.max_frame_bytes) {
        fail(failure, error_limit_exceeded, "帧超过 max_frame_bytes");
        return std::nullopt;
    }
    if (bytes.size() < kMinFrameBytes) {
        fail(failure, error_invalid_request, "帧长度过短");
        return std::nullopt;
    }
    if (!fb::ClientStreamFrameBufferHasIdentifier(bytes.data())) {
        fail(failure, error_version_incompatible, "file identifier 不匹配");
        return std::nullopt;
    }
    flatbuffers::Verifier verifier{bytes.data(), bytes.size(), limits.verifier_max_depth,
                                   limits.verifier_max_tables};
    if (!fb::VerifyClientStreamFrameBuffer(verifier)) {
        fail(failure, error_invalid_request, "FlatBuffers verifier 拒绝");
        return std::nullopt;
    }
    const auto* root = fb::GetClientStreamFrame(bytes.data());
    if (root->schema_version() != data_schema_version) {
        fail(failure, error_version_incompatible, "schema_version 不兼容");
        return std::nullopt;
    }

    switch (root->control_type()) {
    case fb::ClientControl::AckFrame: {
        const auto* ack = static_cast<const fb::AckFrame*>(root->control());
        return WireClientControl{WireAck{ack->stream_epoch(), ack->snapshot_id()}};
    }
    case fb::ClientControl::KeyframeRequest: {
        const auto* request = static_cast<const fb::KeyframeRequest*>(root->control());
        WireKeyframeRequest result;
        if (request->reason() != nullptr) {
            if (!text_within_limits(request->reason()->size(), limits)) {
                fail(failure, error_limit_exceeded, "reason 超过长度上限");
                return std::nullopt;
            }
            result.reason = request->reason()->str();
        }
        return WireClientControl{std::move(result)};
    }
    case fb::ClientControl::NONE:
    default:
        fail(failure, error_invalid_request, "未知客户端控制帧类型");
        return std::nullopt;
    }
}

} // namespace geoworld::protocol
