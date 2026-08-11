#include "geoworld/debug/state_hash.hpp"

#include <bit>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace geoworld::debug {
namespace {

class HashBuilder {
public:
    void add(std::uint64_t value) noexcept {
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            hash_ ^= static_cast<std::uint8_t>(value >> shift);
            hash_ *= 1099511628211ULL;
        }
    }

    void add(std::string_view value) noexcept {
        add(value.size());
        for (const auto character : value) {
            hash_ ^= static_cast<std::uint8_t>(character);
            hash_ *= 1099511628211ULL;
        }
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_{14695981039346656037ULL};
};

void add_property(HashBuilder& hash, const world::PropertyValue& value) noexcept {
    hash.add(value.index());
    std::visit([&hash](const auto& item) {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::string>) {
            hash.add(item);
        } else if constexpr (std::is_same_v<Value, double>) {
            hash.add(std::bit_cast<std::uint64_t>(item));
        } else {
            hash.add(static_cast<std::uint64_t>(item));
        }
    }, value);
}

void add_properties(HashBuilder& hash, const world::PropertyBag& properties) noexcept {
    hash.add(properties.size());
    for (const auto& [name, value] : properties) {
        hash.add(name);
        add_property(hash, value);
    }
}

} // namespace

std::uint64_t world_state_hash(const world::World& world) {
    HashBuilder hash;
    const auto objects = world.snapshot();
    hash.add(objects.size());

    for (const auto& object : objects) {
        hash.add(object.id.value);
        hash.add(object.geometry_ref);
        hash.add(std::bit_cast<std::uint64_t>(object.position.x));
        hash.add(std::bit_cast<std::uint64_t>(object.position.y));
        hash.add(std::bit_cast<std::uint64_t>(object.position.z));
        hash.add(object.semantic_type);
        add_properties(hash, object.properties);
        add_properties(hash, object.state);
        hash.add(static_cast<std::uint64_t>(object.lifecycle));
        hash.add(object.version);

        hash.add(object.capabilities.size());
        for (const auto& capability : object.capabilities) {
            hash.add(capability);
        }

        hash.add(object.relations.size());
        for (const auto& relation : object.relations) {
            hash.add(relation.target.value);
            hash.add(relation.type);
            add_properties(hash, relation.attributes);
        }
    }
    return hash.value();
}

} // namespace geoworld::debug
