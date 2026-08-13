#include "geoworld/projection/canonical.hpp"

#include <bit>
#include <cstdint>
#include <string_view>

namespace geoworld::projection {

namespace {

// FNV-1a 64，与 debug::world_state_hash 同一族哈希，保证规范化投影可稳定比较。
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

class HashWriter {
public:
    void bytes(const void* data, std::size_t size) noexcept {
        const auto* cursor = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash_ = (hash_ ^ cursor[index]) * kFnvPrime;
        }
    }

    void u8(std::uint8_t value) noexcept { bytes(&value, sizeof(value)); }

    void u32(std::uint32_t value) noexcept { bytes(&value, sizeof(value)); }

    void u64(std::uint64_t value) noexcept { bytes(&value, sizeof(value)); }

    void f64(double value) noexcept { u64(std::bit_cast<std::uint64_t>(value)); }

    void text(std::string_view value) noexcept {
        u64(value.size());
        bytes(value.data(), value.size());
    }

    [[nodiscard]] std::uint64_t finish() const noexcept { return hash_; }

private:
    std::uint64_t hash_{kFnvOffset};
};

void write_value(HashWriter& writer, const schema::Value& value) noexcept {
    writer.u8(static_cast<std::uint8_t>(value.index()));
    switch (value.index()) {
    case 0:
        writer.u64(std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value)));
        break;
    case 1:
        writer.f64(std::get<double>(value));
        break;
    case 2:
        writer.u8(std::get<bool>(value) ? 1U : 0U);
        break;
    case 3:
        writer.text(std::get<std::string>(value));
        break;
    default:
        break;
    }
}

void write_bag(HashWriter& writer, const world::PropertyBag& bag) noexcept {
    writer.u64(bag.size());
    for (const auto& [key, value] : bag) {
        writer.text(key);
        write_value(writer, value);
    }
}

} // namespace

[[nodiscard]] std::uint64_t projected_entity_hash(const ProjectedEntity& entity) noexcept {
    HashWriter writer;
    writer.u64(entity.wid.value);
    writer.u64(entity.version);
    writer.f64(entity.position.x);
    writer.f64(entity.position.y);
    writer.f64(entity.position.z);
    writer.text(entity.semantic_type);
    writer.text(entity.geometry_ref);
    writer.u8(static_cast<std::uint8_t>(entity.lifecycle));
    write_bag(writer, entity.properties);
    write_bag(writer, entity.state);

    writer.u64(entity.relations.size());
    for (const auto& relation : entity.relations) {
        writer.text(relation.type);
        writer.u64(relation.target.value);
        write_bag(writer, relation.attributes);
    }

    writer.u64(entity.capabilities.size());
    for (const auto& capability : entity.capabilities) {
        writer.text(capability);
    }

    writer.u8(static_cast<std::uint8_t>(entity.metadata.frequency));
    writer.u32(entity.metadata.priority);
    writer.u64(entity.metadata.visibility_tags.size());
    for (const auto& tag : entity.metadata.visibility_tags) {
        writer.text(tag);
    }
    return writer.finish();
}

} // namespace geoworld::projection
