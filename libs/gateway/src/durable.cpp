#include "geoworld/gateway/durable.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace geoworld::gateway {
namespace {

// 命令操作编码：与 proto CommandOperation 数值一致（SET_PROPERTY=1 等）。
inline constexpr std::uint8_t kOperationSetProperty = 1;
inline constexpr std::uint8_t kOperationCreateObject = 2;
inline constexpr std::uint8_t kOperationDestroyObject = 3;

// PropertyValue 变体标签：规范化编码自有约定，不依赖 proto 序号。
inline constexpr std::uint8_t kValueTagInt64 = 0;
inline constexpr std::uint8_t kValueTagDouble = 1;
inline constexpr std::uint8_t kValueTagBool = 2;
inline constexpr std::uint8_t kValueTagString = 3;

inline constexpr std::uint8_t kOutcomeRejected = 0;
inline constexpr std::uint8_t kOutcomeApplied = 1;

class Encoder {
public:
    void put_u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

    void put_u16(std::uint16_t value) {
        for (std::uint32_t shift = 0; shift < 16; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u64(std::uint64_t value) {
        for (std::uint32_t shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_i64(std::int64_t value) { put_u64(std::bit_cast<std::uint64_t>(value)); }
    void put_double(double value) { put_u64(std::bit_cast<std::uint64_t>(value)); }

    void put_text(std::string_view text) {
        put_u16(static_cast<std::uint16_t>(text.size()));
        for (const char character : text) {
            bytes_.push_back(static_cast<std::byte>(character));
        }
    }

    void put_bytes(std::span<const std::byte> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] bool get_u8(std::uint8_t& out) {
        if (offset_ + 1 > data_.size()) {
            return false;
        }
        out = static_cast<std::uint8_t>(data_[offset_++]);
        return true;
    }

    [[nodiscard]] bool get_u16(std::uint16_t& out) {
        std::uint8_t low = 0;
        std::uint8_t high = 0;
        if (!get_u8(low) || !get_u8(high)) {
            return false;
        }
        out = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8U));
        return true;
    }

    [[nodiscard]] bool get_u64(std::uint64_t& out) {
        std::uint64_t value = 0;
        for (std::uint32_t shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!get_u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        out = value;
        return true;
    }

    [[nodiscard]] bool get_text(std::string& out) {
        std::uint16_t length = 0;
        if (!get_u16(length) || offset_ + length > data_.size()) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(data_.data() + offset_), length);
        offset_ += length;
        return true;
    }

    [[nodiscard]] bool get_fixed(std::span<std::byte> out) {
        if (offset_ + out.size() > data_.size()) {
            return false;
        }
        std::memcpy(out.data(), data_.data() + offset_, out.size());
        offset_ += out.size();
        return true;
    }

    [[nodiscard]] std::span<const std::byte> rest() const { return data_.subspan(offset_); }

private:
    std::span<const std::byte> data_;
    std::size_t offset_{};
};

void encode_value(Encoder& encoder, const world::PropertyValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        encoder.put_u8(kValueTagInt64);
        encoder.put_i64(*integer);
    } else if (const auto* number = std::get_if<double>(&value)) {
        encoder.put_u8(kValueTagDouble);
        encoder.put_double(*number);
    } else if (const auto* flag = std::get_if<bool>(&value)) {
        encoder.put_u8(kValueTagBool);
        encoder.put_u8(*flag ? 1 : 0);
    } else {
        encoder.put_u8(kValueTagString);
        encoder.put_text(std::get<std::string>(value));
    }
}

void encode_params(Encoder& encoder, const CommandParams& params) {
    if (const auto* set_property = std::get_if<SetPropertyParams>(&params)) {
        encoder.put_u8(kOperationSetProperty);
        encoder.put_text(set_property->key);
        encode_value(encoder, set_property->value);
    } else if (const auto* create = std::get_if<CreateObjectParams>(&params)) {
        encoder.put_u8(kOperationCreateObject);
        encoder.put_text(create->semantic_type);
        encoder.put_text(create->geometry_ref);
        encoder.put_double(create->position.x);
        encoder.put_double(create->position.y);
        encoder.put_double(create->position.z);
        // PropertyBag 为 std::map，键序遍历保证规范化字节确定。
        encoder.put_u64(create->properties.size());
        for (const auto& [key, value] : create->properties) {
            encoder.put_text(key);
            encode_value(encoder, value);
        }
    } else {
        encoder.put_u8(kOperationDestroyObject);
    }
}

void encode_identity(Encoder& encoder, std::string_view principal_id,
                     const DurableRequestId& request_id) {
    encoder.put_u16(kDurableRecordFormatVersion);
    encoder.put_text(principal_id);
    encoder.put_bytes(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(request_id.data()), request_id.size()});
}

[[nodiscard]] bool decode_identity(Decoder& decoder, DurableIdempotencyIndex::Key& key) {
    std::uint16_t version = 0;
    if (!decoder.get_u16(version) || version != kDurableRecordFormatVersion) {
        return false;
    }
    if (!decoder.get_text(key.principal_id)) {
        return false;
    }
    return decoder.get_fixed(std::span<std::byte>{
        reinterpret_cast<std::byte*>(key.request_id.data()), key.request_id.size()});
}

} // namespace

std::size_t DurableIdempotencyIndex::KeyHash::operator()(const Key& key) const noexcept {
    std::size_t hash = std::hash<std::string>{}(key.principal_id);
    for (const std::uint8_t byte : key.request_id) {
        // FNV-1a 混合：进程内索引哈希，不进入持久格式。
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] DurableIdempotencyIndex::LookupResult DurableIdempotencyIndex::lookup(
    const Key& key, const std::vector<std::byte>& fingerprint) const {
    const auto found = slots_.find(key);
    if (found == slots_.end()) {
        return LookupResult{};
    }
    return LookupResult{&found->second.entry, found->second.fingerprint == fingerprint};
}

void DurableIdempotencyIndex::register_pending(Key key, std::vector<std::byte> fingerprint,
                                               std::uint64_t client_sequence) {
    Slot slot;
    slot.fingerprint = std::move(fingerprint);
    slot.entry.state = DurableEntryState::pending;
    slot.entry.client_sequence = client_sequence;
    slots_.insert_or_assign(std::move(key), std::move(slot));
}

void DurableIdempotencyIndex::mark_durable_accepted(const Key& key, std::uint64_t lsn,
                                                    std::uint64_t ingress_sequence) {
    const auto found = slots_.find(key);
    if (found == slots_.end()) {
        return;
    }
    found->second.entry.state = DurableEntryState::durable_accepted;
    found->second.entry.lsn = lsn;
    found->second.entry.ingress_sequence = ingress_sequence;
}

void DurableIdempotencyIndex::mark_final(const Key& key, bool applied, GatewayError error) {
    const auto found = slots_.find(key);
    if (found == slots_.end()) {
        return;
    }
    found->second.entry.state =
        applied ? DurableEntryState::applied : DurableEntryState::rejected;
    found->second.entry.error = applied ? GatewayError::none : error;
}

void DurableIdempotencyIndex::erase(const Key& key) { slots_.erase(key); }

[[nodiscard]] std::size_t DurableIdempotencyIndex::size() const noexcept {
    return slots_.size();
}

[[nodiscard]] bool DurableIdempotencyIndex::restore(DurableRecordKind kind,
                                                    std::uint64_t lsn,
                                                    std::span<const std::byte> payload) {
    Decoder decoder{payload};
    Key key;
    if (!decode_identity(decoder, key)) {
        return false;
    }

    if (kind == DurableRecordKind::external_command) {
        std::uint64_t target_tick = 0;
        if (!decoder.get_u64(target_tick)) {
            return false;
        }
        const std::span<const std::byte> fingerprint = decoder.rest();
        std::uint64_t client_sequence = 0;
        {
            Decoder fingerprint_decoder{fingerprint};
            if (!fingerprint_decoder.get_u64(client_sequence)) {
                return false;
            }
        }
        const auto found = slots_.find(key);
        if (found != slots_.end()) {
            // 重复登记的命令记录：内容不一致即损坏，fail-closed。
            const std::vector<std::byte>& existing = found->second.fingerprint;
            if (existing.size() != fingerprint.size()
                || !std::equal(existing.begin(), existing.end(), fingerprint.begin())) {
                return false;
            }
            return true;
        }
        Slot slot;
        slot.fingerprint.assign(fingerprint.begin(), fingerprint.end());
        slot.entry.state = DurableEntryState::durable_accepted;
        slot.entry.lsn = lsn;
        slot.entry.client_sequence = client_sequence;
        slots_.emplace(std::move(key), std::move(slot));
        return true;
    }

    if (kind == DurableRecordKind::command_outcome) {
        std::uint64_t client_sequence = 0;
        std::uint64_t ingress_sequence = 0;
        std::uint8_t outcome = 0;
        std::uint16_t error_value = 0;
        if (!decoder.get_u64(client_sequence) || !decoder.get_u64(ingress_sequence)
            || !decoder.get_u8(outcome) || !decoder.get_u16(error_value)) {
            return false;
        }
        const auto found = slots_.find(key);
        if (found == slots_.end()) {
            // 未知键的终态：容许（未来保留期裁剪后可能只剩 outcome）。
            return true;
        }
        DurableIdempotencyEntry& entry = found->second.entry;
        entry.client_sequence = client_sequence;
        entry.ingress_sequence = ingress_sequence;
        const bool applied = outcome == kOutcomeApplied;
        entry.state = applied ? DurableEntryState::applied : DurableEntryState::rejected;
        entry.error = applied ? GatewayError::none
                              : static_cast<GatewayError>(error_value);
        return true;
    }

    return false;
}

[[nodiscard]] std::vector<std::byte> make_command_fingerprint(
    const ExternalCommand& command) {
    Encoder encoder;
    encoder.put_u64(command.client_sequence);
    encoder.put_u64(command.target_wid.value);
    encoder.put_u64(command.expected_object_version);
    encoder.put_u64(command.target_tick_hint);
    encode_params(encoder, command.params);
    return encoder.take();
}

[[nodiscard]] std::vector<std::byte> encode_external_command_record(
    std::string_view principal_id, const DurableRequestId& request_id,
    std::uint64_t target_tick, const ExternalCommand& command) {
    Encoder encoder;
    encode_identity(encoder, principal_id, request_id);
    encoder.put_u64(target_tick);
    encoder.put_bytes(make_command_fingerprint(command));
    return encoder.take();
}

[[nodiscard]] std::vector<std::byte> encode_command_outcome_record(
    std::string_view principal_id, const DurableRequestId& request_id,
    std::uint64_t client_sequence, std::uint64_t ingress_sequence,
    bool applied, GatewayError error) {
    Encoder encoder;
    encode_identity(encoder, principal_id, request_id);
    encoder.put_u64(client_sequence);
    encoder.put_u64(ingress_sequence);
    encoder.put_u8(applied ? kOutcomeApplied : kOutcomeRejected);
    encoder.put_u16(static_cast<std::uint16_t>(error));
    return encoder.take();
}

} // namespace geoworld::gateway
