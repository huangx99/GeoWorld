#include "geoworld/persistence/checkpoint.hpp"

#include "le_codec.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <set>
#include <type_traits>
#include <utility>

namespace geoworld::persistence {
namespace {

inline constexpr std::uint32_t kProviderSchemaVersion = 1;
inline constexpr std::uint64_t kMaxProviderItems = 2'000'000;
inline constexpr std::uint32_t kMaxProviderStringBytes = 16U * 1024U * 1024U;

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void u32(std::uint32_t value) {
        std::byte bytes[4];
        detail::write_le32(bytes, value);
        bytes_.insert(bytes_.end(), bytes, bytes + 4);
    }
    void u64(std::uint64_t value) {
        std::byte bytes[8];
        detail::write_le64(bytes, value);
        bytes_.insert(bytes_.end(), bytes, bytes + 8);
    }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void string(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        const auto* first = reinterpret_cast<const std::byte*>(value.data());
        bytes_.insert(bytes_.end(), first, first + value.size());
    }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (remaining() < 1) return false;
        value = static_cast<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& value) {
        if (remaining() < 4) return false;
        value = detail::read_le32(bytes_.data() + offset_);
        offset_ += 4;
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& value) {
        if (remaining() < 8) return false;
        value = detail::read_le64(bytes_.data() + offset_);
        offset_ += 8;
        return true;
    }
    [[nodiscard]] bool i64(std::int64_t& value) {
        std::uint64_t raw{};
        if (!u64(raw)) return false;
        value = static_cast<std::int64_t>(raw);
        return true;
    }
    [[nodiscard]] bool f64(double& value) {
        std::uint64_t raw{};
        if (!u64(raw)) return false;
        value = std::bit_cast<double>(raw);
        return true;
    }
    [[nodiscard]] bool string(std::string& value) {
        std::uint32_t size{};
        if (!u32(size) || size > kMaxProviderStringBytes || remaining() < size) return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    [[nodiscard]] bool count64(std::uint64_t& value) {
        return u64(value) && value <= kMaxProviderItems;
    }
    [[nodiscard]] bool count32(std::uint32_t& value) {
        return u32(value) && value <= kMaxProviderItems;
    }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }

private:
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

enum class ValueTag : std::uint8_t { integer, real, boolean, string };

void write_value(Writer& writer, const schema::Value& value) {
    std::visit([&writer](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::int64_t>) {
            writer.u8(static_cast<std::uint8_t>(ValueTag::integer));
            writer.i64(item);
        } else if constexpr (std::is_same_v<T, double>) {
            writer.u8(static_cast<std::uint8_t>(ValueTag::real));
            writer.f64(item);
        } else if constexpr (std::is_same_v<T, bool>) {
            writer.u8(static_cast<std::uint8_t>(ValueTag::boolean));
            writer.u8(item ? 1 : 0);
        } else {
            writer.u8(static_cast<std::uint8_t>(ValueTag::string));
            writer.string(item);
        }
    }, value);
}

bool read_value(Reader& reader, schema::Value& value) {
    std::uint8_t tag{};
    if (!reader.u8(tag)) return false;
    switch (static_cast<ValueTag>(tag)) {
    case ValueTag::integer: {
        std::int64_t item{};
        if (!reader.i64(item)) return false;
        value = item;
        return true;
    }
    case ValueTag::real: {
        double item{};
        if (!reader.f64(item)) return false;
        value = item;
        return true;
    }
    case ValueTag::boolean: {
        std::uint8_t item{};
        if (!reader.u8(item) || item > 1) return false;
        value = item != 0;
        return true;
    }
    case ValueTag::string: {
        std::string item;
        if (!reader.string(item)) return false;
        value = std::move(item);
        return true;
    }
    }
    return false;
}

void write_bag(Writer& writer, const schema::PropertyBag& bag) {
    writer.u32(static_cast<std::uint32_t>(bag.size()));
    for (const auto& [key, value] : bag) {
        writer.string(key);
        write_value(writer, value);
    }
}

bool read_bag(Reader& reader, schema::PropertyBag& bag) {
    std::uint32_t count{};
    if (!reader.count32(count)) return false;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string key;
        schema::Value value;
        if (!reader.string(key) || key.empty() || !read_value(reader, value)
            || !bag.emplace(std::move(key), std::move(value)).second) return false;
    }
    return true;
}

void write_object(Writer& writer, const world::WorldObject& object) {
    writer.u64(object.id.value);
    writer.string(object.geometry_ref);
    writer.f64(object.position.x);
    writer.f64(object.position.y);
    writer.f64(object.position.z);
    writer.string(object.semantic_type);
    write_bag(writer, object.properties);
    write_bag(writer, object.state);
    writer.u32(static_cast<std::uint32_t>(object.relations.size()));
    for (const auto& relation : object.relations) {
        writer.u64(relation.target.value);
        writer.string(relation.type);
        write_bag(writer, relation.attributes);
    }
    writer.u32(static_cast<std::uint32_t>(object.capabilities.size()));
    for (const auto& capability : object.capabilities) writer.string(capability);
    writer.u8(static_cast<std::uint8_t>(object.lifecycle));
    writer.u64(object.version);
    writer.u64(object.revision);
}

bool read_object(Reader& reader, world::WorldObject& object) {
    std::uint32_t relations{};
    std::uint32_t capabilities{};
    std::uint8_t lifecycle{};
    if (!reader.u64(object.id.value) || !object.id.valid()
        || !reader.string(object.geometry_ref) || !reader.f64(object.position.x)
        || !reader.f64(object.position.y) || !reader.f64(object.position.z)
        || !reader.string(object.semantic_type) || !read_bag(reader, object.properties)
        || !read_bag(reader, object.state) || !reader.count32(relations)) return false;
    object.relations.reserve(relations);
    for (std::uint32_t index = 0; index < relations; ++index) {
        world::Relation relation;
        if (!reader.u64(relation.target.value) || !relation.target.valid()
            || !reader.string(relation.type) || relation.type.empty()
            || !read_bag(reader, relation.attributes)) return false;
        object.relations.push_back(std::move(relation));
    }
    if (!reader.count32(capabilities)) return false;
    object.capabilities.reserve(capabilities);
    for (std::uint32_t index = 0; index < capabilities; ++index) {
        std::string capability;
        if (!reader.string(capability) || capability.empty()) return false;
        object.capabilities.push_back(std::move(capability));
    }
    if (!reader.u8(lifecycle)
        || lifecycle > static_cast<std::uint8_t>(world::LifecycleState::retired)
        || !reader.u64(object.version) || object.version == 0
        || !reader.u64(object.revision)) return false;
    object.lifecycle = static_cast<world::LifecycleState>(lifecycle);
    return true;
}

void write_command(Writer& writer, const simulation::Command& command) {
    writer.u64(command.sequence);
    writer.u64(command.target_tick);
    writer.u64(command.meta.ingress_sequence);
    writer.u64(command.meta.durable_lsn);
    writer.u64(command.meta.expected_object_version);
    std::visit([&writer](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, simulation::CreateObjectCommand>) {
            writer.u8(0);
            write_object(writer, payload.object);
        } else if constexpr (std::is_same_v<T, simulation::DestroyObjectCommand>) {
            writer.u8(1);
            writer.u64(payload.id.value);
        } else {
            writer.u8(2);
            writer.u64(payload.id.value);
            writer.string(payload.key);
            write_value(writer, payload.value);
        }
    }, command.payload);
}

bool read_command(Reader& reader, simulation::Command& command) {
    std::uint8_t tag{};
    if (!reader.u64(command.sequence) || !reader.u64(command.target_tick)
        || !reader.u64(command.meta.ingress_sequence)
        || !reader.u64(command.meta.durable_lsn)
        || !reader.u64(command.meta.expected_object_version) || !reader.u8(tag)) return false;
    if (tag == 0) {
        world::WorldObject object;
        if (!read_object(reader, object)) return false;
        command.payload = simulation::CreateObjectCommand{std::move(object)};
    } else if (tag == 1) {
        foundation::WorldId id;
        if (!reader.u64(id.value) || !id.valid()) return false;
        command.payload = simulation::DestroyObjectCommand{id};
    } else if (tag == 2) {
        simulation::SetPropertyCommand property;
        if (!reader.u64(property.id.value) || !property.id.valid()
            || !reader.string(property.key) || property.key.empty()
            || !read_value(reader, property.value)) return false;
        command.payload = std::move(property);
    } else {
        return false;
    }
    return true;
}

template <typename Snapshot, typename Owner>
class SnapshotProviderBase : public CheckpointProvider {
public:
    SnapshotProviderBase(std::string_view id, Owner& owner) : id_(id), owner_(owner) {}
    [[nodiscard]] CheckpointSchema schema() const override { return {id_, kProviderSchemaVersion}; }
protected:
    std::string id_;
    Owner& owner_;
};

class CommandProvider final
    : public SnapshotProviderBase<simulation::CommandBufferSnapshot, simulation::CommandBuffer> {
public:
    explicit CommandProvider(simulation::CommandBuffer& owner)
        : SnapshotProviderBase(kCommandBufferProviderId, owner) {}
    [[nodiscard]] FrozenProviderState freeze() const override {
        auto snapshot = owner_.snapshot();
        return {std::make_shared<const simulation::CommandBufferSnapshot>(snapshot),
                Lsn{snapshot.included_durable_lsn}};
    }
    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& snapshot = *static_cast<const simulation::CommandBufferSnapshot*>(frozen.data.get());
        Writer writer;
        writer.u64(snapshot.next_sequence);
        writer.u64(snapshot.included_durable_lsn);
        writer.u64(snapshot.pending.size());
        for (const auto& command : snapshot.pending) write_command(writer, command);
        return {schema(), writer.take()};
    }
    [[nodiscard]] PersistenceError validate_restore(std::span<const std::byte> payload,
                                                     std::uint32_t version) const override {
        simulation::CommandBuffer scratch;
        return decode_and_restore(scratch, payload, version);
    }
    [[nodiscard]] PersistenceError restore(std::span<const std::byte> payload,
                                            std::uint32_t version) override {
        return decode_and_restore(owner_, payload, version);
    }
private:
    static PersistenceError decode_and_restore(simulation::CommandBuffer& target,
                                               std::span<const std::byte> payload,
                                               std::uint32_t version) {
        if (version != kProviderSchemaVersion) return PersistenceError::provider_version_mismatch;
        Reader reader{payload};
        simulation::CommandBufferSnapshot snapshot;
        std::uint64_t count{};
        if (!reader.u64(snapshot.next_sequence) || !reader.u64(snapshot.included_durable_lsn)
            || !reader.count64(count)) return PersistenceError::checkpoint_invalid;
        snapshot.pending.reserve(count);
        for (std::uint64_t index = 0; index < count; ++index) {
            simulation::Command command;
            if (!read_command(reader, command)) return PersistenceError::checkpoint_invalid;
            snapshot.pending.push_back(std::move(command));
        }
        return reader.at_end() && target.restore(std::move(snapshot))
                   ? PersistenceError::none : PersistenceError::checkpoint_invalid;
    }
};

class EventProvider final : public SnapshotProviderBase<rules::EventBusSnapshot, rules::EventBus> {
public:
    explicit EventProvider(rules::EventBus& owner) : SnapshotProviderBase(kEventBusProviderId, owner) {}
    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const rules::EventBusSnapshot>(owner_.snapshot())};
    }
    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& snapshot = *static_cast<const rules::EventBusSnapshot*>(frozen.data.get());
        Writer writer;
        writer.u64(snapshot.next_sequence);
        writer.u64(snapshot.pending.size());
        for (const auto& event : snapshot.pending) {
            writer.u64(event.sequence); writer.u64(event.target_tick);
            writer.i64(event.priority); writer.string(event.type); writer.u64(event.subject.value);
            write_bag(writer, event.payload);
        }
        return {schema(), writer.take()};
    }
    [[nodiscard]] PersistenceError validate_restore(std::span<const std::byte> bytes,
                                                     std::uint32_t version) const override {
        rules::EventBus scratch;
        return decode(scratch, bytes, version);
    }
    [[nodiscard]] PersistenceError restore(std::span<const std::byte> bytes,
                                            std::uint32_t version) override {
        return decode(owner_, bytes, version);
    }
private:
    static PersistenceError decode(rules::EventBus& target, std::span<const std::byte> bytes,
                                   std::uint32_t version) {
        if (version != kProviderSchemaVersion) return PersistenceError::provider_version_mismatch;
        Reader reader{bytes}; rules::EventBusSnapshot snapshot; std::uint64_t count{};
        if (!reader.u64(snapshot.next_sequence) || !reader.count64(count))
            return PersistenceError::checkpoint_invalid;
        snapshot.pending.reserve(count);
        for (std::uint64_t index = 0; index < count; ++index) {
            rules::Event event; std::int64_t priority{};
            if (!reader.u64(event.sequence) || !reader.u64(event.target_tick)
                || !reader.i64(priority) || priority < std::numeric_limits<int>::min()
                || priority > std::numeric_limits<int>::max() || !reader.string(event.type)
                || !reader.u64(event.subject.value) || !read_bag(reader, event.payload))
                return PersistenceError::checkpoint_invalid;
            event.priority = static_cast<int>(priority);
            snapshot.pending.push_back(std::move(event));
        }
        return reader.at_end() && target.restore(std::move(snapshot))
                   ? PersistenceError::none : PersistenceError::checkpoint_invalid;
    }
};

class IntentProvider final
    : public SnapshotProviderBase<ai::DecisionIntentSnapshot, ai::DecisionIntentBuffer> {
public:
    explicit IntentProvider(ai::DecisionIntentBuffer& owner)
        : SnapshotProviderBase(kAiIntentsProviderId, owner) {}
    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const ai::DecisionIntentSnapshot>(owner_.snapshot())};
    }
    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& snapshot = *static_cast<const ai::DecisionIntentSnapshot*>(frozen.data.get());
        Writer writer; writer.u64(snapshot.next_sequence); writer.u64(snapshot.pending.size());
        for (const auto& pending : snapshot.pending) {
            writer.u64(pending.sequence); writer.u64(pending.target_tick);
            writer.u64(pending.intent.target.value); writer.string(pending.intent.key);
            write_value(writer, pending.intent.value);
        }
        return {schema(), writer.take()};
    }
    [[nodiscard]] PersistenceError validate_restore(std::span<const std::byte> bytes,
                                                     std::uint32_t version) const override {
        ai::DecisionIntentBuffer scratch; return decode(scratch, bytes, version);
    }
    [[nodiscard]] PersistenceError restore(std::span<const std::byte> bytes,
                                            std::uint32_t version) override {
        return decode(owner_, bytes, version);
    }
private:
    static PersistenceError decode(ai::DecisionIntentBuffer& target,
                                   std::span<const std::byte> bytes, std::uint32_t version) {
        if (version != kProviderSchemaVersion) return PersistenceError::provider_version_mismatch;
        Reader reader{bytes}; ai::DecisionIntentSnapshot snapshot; std::uint64_t count{};
        if (!reader.u64(snapshot.next_sequence) || !reader.count64(count))
            return PersistenceError::checkpoint_invalid;
        snapshot.pending.reserve(count);
        for (std::uint64_t index = 0; index < count; ++index) {
            ai::PendingIntent pending;
            if (!reader.u64(pending.sequence) || !reader.u64(pending.target_tick)
                || !reader.u64(pending.intent.target.value) || !reader.string(pending.intent.key)
                || !read_value(reader, pending.intent.value)) return PersistenceError::checkpoint_invalid;
            snapshot.pending.push_back(std::move(pending));
        }
        return reader.at_end() && target.restore(std::move(snapshot))
                   ? PersistenceError::none : PersistenceError::checkpoint_invalid;
    }
};

class RandomProvider final
    : public SnapshotProviderBase<std::vector<foundation::NamedRandomStreamState>,
                                  foundation::NamedRandomStreams> {
public:
    explicit RandomProvider(foundation::NamedRandomStreams& owner)
        : SnapshotProviderBase(kRandomStreamsProviderId, owner) {}
    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const std::vector<foundation::NamedRandomStreamState>>(
            owner_.snapshot())};
    }
    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& items = *static_cast<const std::vector<foundation::NamedRandomStreamState>*>(frozen.data.get());
        Writer writer; writer.u64(items.size());
        for (const auto& item : items) { writer.string(item.name); writer.u64(item.state); }
        return {schema(), writer.take()};
    }
    [[nodiscard]] PersistenceError validate_restore(std::span<const std::byte> bytes,
                                                     std::uint32_t version) const override {
        foundation::NamedRandomStreams scratch; return decode(scratch, bytes, version);
    }
    [[nodiscard]] PersistenceError restore(std::span<const std::byte> bytes,
                                            std::uint32_t version) override {
        return decode(owner_, bytes, version);
    }
private:
    static PersistenceError decode(foundation::NamedRandomStreams& target,
                                   std::span<const std::byte> bytes, std::uint32_t version) {
        if (version != kProviderSchemaVersion) return PersistenceError::provider_version_mismatch;
        Reader reader{bytes}; std::uint64_t count{};
        if (!reader.count64(count)) return PersistenceError::checkpoint_invalid;
        std::vector<foundation::NamedRandomStreamState> items; items.reserve(count);
        for (std::uint64_t index = 0; index < count; ++index) {
            foundation::NamedRandomStreamState item;
            if (!reader.string(item.name) || !reader.u64(item.state))
                return PersistenceError::checkpoint_invalid;
            items.push_back(std::move(item));
        }
        return reader.at_end() && target.restore(std::move(items))
                   ? PersistenceError::none : PersistenceError::checkpoint_invalid;
    }
};

class ArtifactProvider final
    : public SnapshotProviderBase<std::vector<tooling::ActiveArtifact>, tooling::ArtifactManifest> {
public:
    explicit ArtifactProvider(tooling::ArtifactManifest& owner)
        : SnapshotProviderBase(kArtifactsProviderId, owner) {}
    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const std::vector<tooling::ActiveArtifact>>(owner_.snapshot())};
    }
    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& items = *static_cast<const std::vector<tooling::ActiveArtifact>*>(frozen.data.get());
        Writer writer; writer.u64(items.size());
        for (const auto& item : items) {
            writer.string(item.logical_name); writer.string(item.header.type);
            writer.u32(item.header.schema_version); writer.string(item.header.source_hash);
            writer.u32(item.header.compiler_version);
        }
        return {schema(), writer.take()};
    }
    [[nodiscard]] PersistenceError validate_restore(std::span<const std::byte> bytes,
                                                     std::uint32_t version) const override {
        tooling::ArtifactManifest scratch; return decode(scratch, bytes, version);
    }
    [[nodiscard]] PersistenceError restore(std::span<const std::byte> bytes,
                                            std::uint32_t version) override {
        return decode(owner_, bytes, version);
    }
private:
    static PersistenceError decode(tooling::ArtifactManifest& target,
                                   std::span<const std::byte> bytes, std::uint32_t version) {
        if (version != kProviderSchemaVersion) return PersistenceError::provider_version_mismatch;
        Reader reader{bytes}; std::uint64_t count{};
        if (!reader.count64(count)) return PersistenceError::checkpoint_invalid;
        std::vector<tooling::ActiveArtifact> items; items.reserve(count);
        for (std::uint64_t index = 0; index < count; ++index) {
            tooling::ActiveArtifact item;
            if (!reader.string(item.logical_name) || !reader.string(item.header.type)
                || !reader.u32(item.header.schema_version) || !reader.string(item.header.source_hash)
                || !reader.u32(item.header.compiler_version)) return PersistenceError::checkpoint_invalid;
            items.push_back(std::move(item));
        }
        return reader.at_end() && target.restore(std::move(items))
                   ? PersistenceError::none : PersistenceError::checkpoint_invalid;
    }
};

class EcsActiveSetProvider final : public CheckpointProvider {
public:
    EcsActiveSetProvider(ecs::Runtime& runtime, world::World& world)
        : runtime_(runtime), world_(world) {}
    [[nodiscard]] CheckpointSchema schema() const override {
        return {std::string{kEcsActiveSetProviderId}, kProviderSchemaVersion};
    }
    [[nodiscard]] std::vector<std::string> restore_dependencies() const override {
        return {std::string{kWorldProviderId}};
    }
    [[nodiscard]] FrozenProviderState freeze() const override {
        std::vector<foundation::WorldId> ids;
        const auto snapshot = runtime_.snapshot(); ids.reserve(snapshot.entities.size());
        for (const auto& entity : snapshot.entities) ids.push_back(entity.world_id);
        return {std::make_shared<const std::vector<foundation::WorldId>>(std::move(ids))};
    }
    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto& ids = *static_cast<const std::vector<foundation::WorldId>*>(frozen.data.get());
        Writer writer; writer.u64(ids.size());
        for (const auto id : ids) writer.u64(id.value);
        return {schema(), writer.take()};
    }
    [[nodiscard]] PersistenceError validate_restore(std::span<const std::byte> bytes,
                                                     std::uint32_t version) const override {
        return decode_ids(bytes, version).error;
    }
    [[nodiscard]] PersistenceError restore(std::span<const std::byte> bytes,
                                            std::uint32_t version) override {
        auto decoded = decode_ids(bytes, version);
        if (!decoded.ok()) return decoded.error;
        for (const auto id : decoded.value) {
            if (world_.find(id) == nullptr) return PersistenceError::checkpoint_invalid;
        }
        runtime_.clear();
        for (const auto id : decoded.value) {
            if (!runtime_.activate(*world_.find(id)).has_value()) {
                runtime_.clear();
                return PersistenceError::checkpoint_invalid;
            }
        }
        return PersistenceError::none;
    }
private:
    static Result<std::vector<foundation::WorldId>> decode_ids(
        std::span<const std::byte> bytes, std::uint32_t version) {
        Result<std::vector<foundation::WorldId>> result;
        if (version != kProviderSchemaVersion) {
            result.error = PersistenceError::provider_version_mismatch; return result;
        }
        Reader reader{bytes}; std::uint64_t count{};
        if (!reader.count64(count)) { result.error = PersistenceError::checkpoint_invalid; return result; }
        result.value.reserve(count); foundation::WorldId previous{};
        for (std::uint64_t index = 0; index < count; ++index) {
            foundation::WorldId id;
            if (!reader.u64(id.value) || !id.valid() || (previous.valid() && id <= previous)) {
                result.error = PersistenceError::checkpoint_invalid; return result;
            }
            previous = id; result.value.push_back(id);
        }
        if (!reader.at_end()) result.error = PersistenceError::checkpoint_invalid;
        return result;
    }
    ecs::Runtime& runtime_;
    world::World& world_;
};

} // namespace

std::shared_ptr<CheckpointProvider> make_command_buffer_provider(
    simulation::CommandBuffer& commands) { return std::make_shared<CommandProvider>(commands); }
std::shared_ptr<CheckpointProvider> make_event_bus_provider(rules::EventBus& events) {
    return std::make_shared<EventProvider>(events);
}
std::shared_ptr<CheckpointProvider> make_ai_intents_provider(ai::DecisionIntentBuffer& intents) {
    return std::make_shared<IntentProvider>(intents);
}
std::shared_ptr<CheckpointProvider> make_random_streams_provider(
    foundation::NamedRandomStreams& streams) { return std::make_shared<RandomProvider>(streams); }
std::shared_ptr<CheckpointProvider> make_artifacts_provider(tooling::ArtifactManifest& artifacts) {
    return std::make_shared<ArtifactProvider>(artifacts);
}
std::shared_ptr<CheckpointProvider> make_ecs_active_set_provider(
    ecs::Runtime& runtime, world::World& world) {
    return std::make_shared<EcsActiveSetProvider>(runtime, world);
}

} // namespace geoworld::persistence
