#include "geoworld/persistence/checkpoint.hpp"

#include "geoworld/world/snapshot.hpp"

#include "le_codec.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>
#include <variant>
#include <vector>

namespace geoworld::persistence {

namespace {

inline constexpr std::uint64_t kMaxWorldObjects = 2'000'000;
inline constexpr std::uint32_t kMaxWorldCollectionItems = 2'000'000;
inline constexpr std::uint32_t kMaxWorldStringBytes = 16U * 1024U * 1024U;

// world provider payload 线格式（schema_version 1，全部 little-endian）：
// u64 对象数；逐对象 u64 id/version/revision、u8 lifecycle、3×u64 位置位、
// 字符串 geometry_ref/semantic_type、属性包、状态包、关系数组、能力数组；
// 末尾 u64 next_revision/erase_revision。排序依据：对象按 id、属性包为 std::map，
// 关系/能力保持插入序——同一历史下确定。
class PayloadWriter {
public:
    void u8(std::uint8_t value) { out_.push_back(static_cast<std::byte>(value)); }

    void u64(std::uint64_t value) {
        std::byte buffer[8];
        detail::write_le64(buffer, value);
        out_.insert(out_.end(), buffer, buffer + 8);
    }

    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

    void string(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
        out_.insert(out_.end(), bytes, bytes + value.size());
    }

    void u32(std::uint32_t value) {
        std::byte buffer[4];
        detail::write_le32(buffer, value);
        out_.insert(out_.end(), buffer, buffer + 4);
    }

    [[nodiscard]] std::vector<std::byte> take() { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

constexpr std::uint8_t kTagInt = 0;
constexpr std::uint8_t kTagDouble = 1;
constexpr std::uint8_t kTagBool = 2;
constexpr std::uint8_t kTagString = 3;

void write_value(PayloadWriter& writer, const world::PropertyValue& value) {
    std::visit(
        [&writer](const auto& item) {
            using Value = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Value, std::int64_t>) {
                writer.u8(kTagInt);
                writer.i64(item);
            } else if constexpr (std::is_same_v<Value, double>) {
                writer.u8(kTagDouble);
                writer.f64(item);
            } else if constexpr (std::is_same_v<Value, bool>) {
                writer.u8(kTagBool);
                writer.u8(item ? 1 : 0);
            } else {
                writer.u8(kTagString);
                writer.string(item);
            }
        },
        value);
}

void write_bag(PayloadWriter& writer, const world::PropertyBag& properties) {
    writer.u32(static_cast<std::uint32_t>(properties.size()));
    for (const auto& [key, value] : properties) {
        writer.string(key);
        write_value(writer, value);
    }
}

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (offset_ + 1 > bytes_.size()) {
            return false;
        }
        value = static_cast<std::uint8_t>(bytes_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) {
        if (offset_ + 4 > bytes_.size()) {
            return false;
        }
        value = detail::read_le32(bytes_.data() + offset_);
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) {
        if (offset_ + 8 > bytes_.size()) {
            return false;
        }
        value = detail::read_le64(bytes_.data() + offset_);
        offset_ += 8;
        return true;
    }

    [[nodiscard]] bool i64(std::int64_t& value) {
        std::uint64_t raw{};
        if (!u64(raw)) {
            return false;
        }
        value = static_cast<std::int64_t>(raw);
        return true;
    }

    [[nodiscard]] bool f64(double& value) {
        std::uint64_t raw{};
        if (!u64(raw)) {
            return false;
        }
        value = std::bit_cast<double>(raw);
        return true;
    }

    [[nodiscard]] bool string(std::string& value) {
        std::uint32_t length{};
        if (!u32(length) || length > kMaxWorldStringBytes
            || offset_ + length > bytes_.size()) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] bool read_bag(PayloadReader& reader, world::PropertyBag& properties) {
    std::uint32_t count{};
    if (!reader.u32(count) || count > kMaxWorldCollectionItems) {
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string key;
        std::uint8_t tag{};
        if (!reader.string(key) || !reader.u8(tag)) {
            return false;
        }
        switch (tag) {
        case kTagInt: {
            std::int64_t item{};
            if (!reader.i64(item)) {
                return false;
            }
            if (!properties.emplace(std::move(key), item).second) return false;
            break;
        }
        case kTagDouble: {
            double item{};
            if (!reader.f64(item)) {
                return false;
            }
            if (!properties.emplace(std::move(key), item).second) return false;
            break;
        }
        case kTagBool: {
            std::uint8_t item{};
            if (!reader.u8(item) || item > 1) {
                return false;
            }
            if (!properties.emplace(std::move(key), item != 0).second) return false;
            break;
        }
        case kTagString: {
            std::string item;
            if (!reader.string(item)) {
                return false;
            }
            if (!properties.emplace(std::move(key), std::move(item)).second) return false;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

class WorldCheckpointProvider final : public CheckpointProvider {
public:
    explicit WorldCheckpointProvider(world::World& world) : world_(world) {}

    [[nodiscard]] CheckpointSchema schema() const override {
        return {std::string{kWorldProviderId}, kWorldSchemaVersion};
    }

    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const world::FrozenWorldSnapshot>(
            world::freeze_snapshot(world_))};
    }

    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& snapshot =
            *static_cast<const world::FrozenWorldSnapshot*>(frozen.data.get());
        PayloadWriter writer;
        writer.u64(snapshot.size());
        std::vector<const world::WorldObject*> objects;
        objects.reserve(snapshot.size());
        snapshot.for_each_object([&objects](const world::WorldObject& object) {
            objects.push_back(&object);
        });
        std::sort(objects.begin(), objects.end(), [](const auto* left, const auto* right) {
            return left->id < right->id;
        });
        for (const world::WorldObject* object_ptr : objects) {
            const world::WorldObject& object = *object_ptr;
            writer.u64(object.id.value);
            writer.u64(object.version);
            writer.u64(object.revision);
            writer.u8(static_cast<std::uint8_t>(object.lifecycle));
            writer.f64(object.position.x);
            writer.f64(object.position.y);
            writer.f64(object.position.z);
            writer.string(object.geometry_ref);
            writer.string(object.semantic_type);
            write_bag(writer, object.properties);
            write_bag(writer, object.state);
            writer.u32(static_cast<std::uint32_t>(object.relations.size()));
            for (const world::Relation& relation : object.relations) {
                writer.u64(relation.target.value);
                writer.string(relation.type);
                write_bag(writer, relation.attributes);
            }
            writer.u32(static_cast<std::uint32_t>(object.capabilities.size()));
            for (const std::string& capability : object.capabilities) {
                writer.string(capability);
            }
        }
        writer.u64(snapshot.next_revision());
        writer.u64(snapshot.erase_revision());
        return CheckpointBlock{schema(), writer.take()};
    }

    [[nodiscard]] PersistenceError validate_restore(
        std::span<const std::byte> payload, std::uint32_t schema_version) const override {
        world::World scratch;
        WorldCheckpointProvider validator{scratch};
        return validator.restore(payload, schema_version);
    }

    [[nodiscard]] PersistenceError restore(std::span<const std::byte> payload,
                                           std::uint32_t schema_version) override {
        if (schema_version != kWorldSchemaVersion) {
            return PersistenceError::provider_version_mismatch;
        }
        PayloadReader reader{payload};
        world::WorldSnapshot snapshot;
        std::uint64_t object_count{};
        if (!reader.u64(object_count) || object_count > kMaxWorldObjects) {
            return PersistenceError::checkpoint_invalid;
        }
        snapshot.objects.reserve(object_count);
        foundation::WorldId previous_id{};
        std::uint64_t maximum_revision{};
        for (std::uint64_t index = 0; index < object_count; ++index) {
            world::WorldObject object;
            std::uint8_t lifecycle_raw{};
            std::uint32_t relation_count{};
            std::uint32_t capability_count{};
            if (!reader.u64(object.id.value) || !reader.u64(object.version)
                || !reader.u64(object.revision) || !reader.u8(lifecycle_raw)
                || !reader.f64(object.position.x) || !reader.f64(object.position.y)
                || !reader.f64(object.position.z) || !reader.string(object.geometry_ref)
                || !reader.string(object.semantic_type) || !read_bag(reader, object.properties)
                || !read_bag(reader, object.state) || !reader.u32(relation_count)) {
                return PersistenceError::checkpoint_invalid;
            }
            if (!object.id.valid() || object.version == 0 || object.revision == 0
                || (previous_id.valid() && object.id <= previous_id)
                || relation_count > kMaxWorldCollectionItems) {
                return PersistenceError::checkpoint_invalid;
            }
            previous_id = object.id;
            maximum_revision = std::max(maximum_revision, object.revision);
            if (lifecycle_raw > static_cast<std::uint8_t>(world::LifecycleState::retired)) {
                return PersistenceError::checkpoint_invalid;
            }
            object.lifecycle = static_cast<world::LifecycleState>(lifecycle_raw);
            object.relations.reserve(relation_count);
            for (std::uint32_t relation_index = 0; relation_index < relation_count;
                 ++relation_index) {
                world::Relation relation;
                if (!reader.u64(relation.target.value) || !reader.string(relation.type)
                    || !read_bag(reader, relation.attributes)) {
                    return PersistenceError::checkpoint_invalid;
                }
                object.relations.push_back(std::move(relation));
            }
            if (!reader.u32(capability_count)
                || capability_count > kMaxWorldCollectionItems) {
                return PersistenceError::checkpoint_invalid;
            }
            object.capabilities.reserve(capability_count);
            for (std::uint32_t capability_index = 0; capability_index < capability_count;
                 ++capability_index) {
                std::string capability;
                if (!reader.string(capability)) {
                    return PersistenceError::checkpoint_invalid;
                }
                object.capabilities.push_back(std::move(capability));
            }
            snapshot.objects.push_back(std::move(object));
        }
        if (!reader.u64(snapshot.next_revision) || snapshot.next_revision < maximum_revision
            || !reader.u64(snapshot.erase_revision)
            || !reader.at_end()) {
            return PersistenceError::checkpoint_invalid;
        }
        world::restore_snapshot(world_, std::move(snapshot));
        return PersistenceError::none;
    }

private:
    static constexpr std::uint32_t kWorldSchemaVersion = 1;

    world::World& world_;
};

class ClockCheckpointProvider final : public CheckpointProvider {
public:
    explicit ClockCheckpointProvider(simulation::TickClock& clock) : clock_(clock) {}

    [[nodiscard]] CheckpointSchema schema() const override {
        return {std::string{kClockProviderId}, kClockSchemaVersion};
    }

    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const FrozenClock>(FrozenClock{
            clock_.tick() + 1, clock_.dt_microseconds()})};
    }

    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& state = *static_cast<const FrozenClock*>(frozen.data.get());
        PayloadWriter writer;
        writer.i64(state.next_tick);
        writer.i64(state.dt_microseconds);
        return CheckpointBlock{schema(), writer.take()};
    }

    [[nodiscard]] PersistenceError validate_restore(
        std::span<const std::byte> payload, std::uint32_t schema_version) const override {
        if (schema_version != kClockSchemaVersion) {
            return PersistenceError::provider_version_mismatch;
        }
        PayloadReader reader{payload};
        std::int64_t next_tick{};
        std::int64_t dt{};
        if (!reader.i64(next_tick) || next_tick < 0 || !reader.i64(dt) || !reader.at_end()) {
            return PersistenceError::checkpoint_invalid;
        }
        return dt == clock_.dt_microseconds() ? PersistenceError::none
                                               : PersistenceError::config_invalid;
    }

    [[nodiscard]] PersistenceError restore(std::span<const std::byte> payload,
                                           std::uint32_t schema_version) override {
        if (schema_version != kClockSchemaVersion) {
            return PersistenceError::provider_version_mismatch;
        }
        PayloadReader reader{payload};
        std::int64_t next_tick{};
        std::int64_t dt{};
        if (!reader.i64(next_tick) || !reader.i64(dt) || !reader.at_end()) {
            return PersistenceError::checkpoint_invalid;
        }
        // 固定步长不一致禁止恢复（docs/M5.md 恢复流程第 2 步）。
        if (dt != clock_.dt_microseconds()) {
            return PersistenceError::config_invalid;
        }
        clock_.restore_tick(next_tick);
        return PersistenceError::none;
    }

private:
    struct FrozenClock {
        std::int64_t next_tick{};
        std::int64_t dt_microseconds{};
    };

    static constexpr std::uint32_t kClockSchemaVersion = 1;

    simulation::TickClock& clock_;
};

} // namespace

std::shared_ptr<CheckpointProvider> make_world_provider(world::World& world) {
    return std::make_shared<WorldCheckpointProvider>(world);
}

std::shared_ptr<CheckpointProvider> make_clock_provider(simulation::TickClock& clock) {
    return std::make_shared<ClockCheckpointProvider>(clock);
}

} // namespace geoworld::persistence
