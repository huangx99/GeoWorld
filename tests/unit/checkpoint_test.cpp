#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/recovery.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/wal.hpp"

#include "geoworld/debug/state_hash.hpp"
#include "geoworld/world/snapshot.hpp"
#include "geoworld/world/world.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

namespace simulation = geoworld::simulation;
namespace world = geoworld::world;

using geoworld::foundation::WorldId;
using geoworld::persistence::BranchId;
using geoworld::persistence::CheckpointAnchor;
using geoworld::persistence::CheckpointBlock;
using geoworld::persistence::CheckpointConfig;
using geoworld::persistence::CheckpointCoordinator;
using geoworld::persistence::CheckpointLoader;
using geoworld::persistence::CheckpointProvider;
using geoworld::persistence::CheckpointRegistry;
using geoworld::persistence::CheckpointSchema;
using geoworld::persistence::Durability;
using geoworld::persistence::FrozenProviderState;
using geoworld::persistence::Lsn;
using geoworld::persistence::PersistenceError;
using geoworld::persistence::RecoveryPlanner;
using geoworld::persistence::Result;
using geoworld::persistence::TailPolicy;
using geoworld::persistence::WalConfig;
using geoworld::persistence::WalRecord;
using geoworld::persistence::WalRecordKind;
using geoworld::persistence::WalWriter;

std::atomic<int> g_dir_counter{0};

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(std::string_view name) {
        path = std::filesystem::temp_directory_path()
               / ("gw-m5b-" + std::string{name} + "-" + std::to_string(::getpid()) + "-"
                  + std::to_string(++g_dir_counter));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

[[nodiscard]] BranchId test_branch() {
    return geoworld::persistence::parse_branch_id("01234567-89ab-cdef-0123-456789abcdef")
        .value_or(BranchId{});
}

[[nodiscard]] CheckpointConfig make_checkpoint_config(const std::filesystem::path& root) {
    CheckpointConfig config;
    config.world = WorldId{42};
    config.branch = test_branch();
    config.layout = geoworld::persistence::make_durable_layout(root, config.world,
                                                               config.branch);
    return config;
}

[[nodiscard]] CheckpointAnchor make_anchor(std::uint64_t completed_tick, std::uint64_t hash,
                                           Lsn included_lsn = Lsn{1}) {
    return CheckpointAnchor{completed_tick, completed_tick + 1, included_lsn, hash};
}

// 测试用 provider：8 字节小端状态，支持自定义 ID/版本/依赖。
class TestProvider final : public CheckpointProvider {
public:
    TestProvider(std::string id, std::uint32_t version, std::vector<std::string> deps = {})
        : id_(std::move(id)), version_(version), deps_(std::move(deps)) {}

    std::uint64_t state{};

    [[nodiscard]] CheckpointSchema schema() const override { return {id_, version_}; }

    [[nodiscard]] std::vector<std::string> restore_dependencies() const override {
        return deps_;
    }

    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const std::uint64_t>(state)};
    }

    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const std::uint64_t value = *static_cast<const std::uint64_t*>(frozen.data.get());
        std::vector<std::byte> payload(8);
        for (std::uint64_t index = 0; index < 8; ++index) {
            payload[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
        }
        return CheckpointBlock{schema(), std::move(payload)};
    }

    [[nodiscard]] PersistenceError restore(std::span<const std::byte> payload,
                                           std::uint32_t schema_version) override {
        if (schema_version != version_) {
            return PersistenceError::provider_version_mismatch;
        }
        if (payload.size() != 8) {
            return PersistenceError::checkpoint_invalid;
        }
        std::uint64_t value = 0;
        for (std::uint64_t index = 0; index < 8; ++index) {
            value |= static_cast<std::uint64_t>(payload[index]) << (index * 8U);
        }
        state = value;
        return PersistenceError::none;
    }

private:
    std::string id_;
    std::uint32_t version_;
    std::vector<std::string> deps_;
};

class UpcastingTestProvider final : public CheckpointProvider {
public:
    std::uint64_t state{};

    [[nodiscard]] CheckpointSchema schema() const override { return {"extra", 2}; }
    [[nodiscard]] FrozenProviderState freeze() const override {
        return {std::make_shared<const std::uint64_t>(state)};
    }
    [[nodiscard]] CheckpointBlock encode(const FrozenProviderState& frozen) const override {
        const auto value = *static_cast<const std::uint64_t*>(frozen.data.get());
        std::vector<std::byte> payload(8);
        for (std::uint64_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
        }
        return {schema(), std::move(payload)};
    }
    [[nodiscard]] Result<std::vector<std::byte>> upcast(
        std::span<const std::byte> payload, std::uint32_t from_version) const override {
        if (from_version != 1 || payload.size() != 8) {
            return {{}, PersistenceError::provider_version_mismatch};
        }
        return {{payload.begin(), payload.end()}, PersistenceError::none};
    }
    [[nodiscard]] PersistenceError restore(std::span<const std::byte> payload,
                                           std::uint32_t version) override {
        if (version != 2 || payload.size() != 8) {
            return PersistenceError::checkpoint_invalid;
        }
        state = 0;
        for (std::uint64_t index = 0; index < payload.size(); ++index) {
            state |= static_cast<std::uint64_t>(payload[index]) << (index * 8U);
        }
        return PersistenceError::none;
    }
};

[[nodiscard]] world::WorldObject make_object(std::uint64_t id) {
    world::WorldObject object;
    object.id = WorldId{id};
    object.geometry_ref = "geom-" + std::to_string(id);
    object.position = world::PositionEcef{1.5 * static_cast<double>(id), -2.5, 3.0};
    object.semantic_type = "vehicle";
    object.properties = {{"count", static_cast<std::int64_t>(id)}, {"flag", id % 2 == 0},
                         {"name", std::string{"obj"}}, {"speed", 2.5}};
    object.state = {{"fuel", std::int64_t{100}}};
    object.lifecycle = world::LifecycleState::active;
    object.capabilities = {"move", "sense"};
    return object;
}

[[nodiscard]] world::World make_sample_world() {
    world::World world;
    static_cast<void>(world.insert(make_object(2)));
    static_cast<void>(world.insert(make_object(1)));
    static_cast<void>(world.insert(make_object(3)));
    static_cast<void>(world.add_relation(
        WorldId{1}, world::Relation{WorldId{2}, "follows", {{"distance", 10.5}}}));
    static_cast<void>(world.erase(WorldId{3}));
    return world;
}

[[nodiscard]] bool worlds_equal(const world::World& left, const world::World& right) {
    const auto a = left.snapshot();
    const auto b = right.snapshot();
    if (a.size() != b.size() || left.next_revision() != right.next_revision()
        || left.erase_revision() != right.erase_revision()) {
        return false;
    }
    for (std::size_t index = 0; index < a.size(); ++index) {
        const auto& x = a[index];
        const auto& y = b[index];
        const bool same = x.id == y.id && x.geometry_ref == y.geometry_ref
                          && x.position.x == y.position.x && x.position.y == y.position.y
                          && x.position.z == y.position.z
                          && x.semantic_type == y.semantic_type && x.properties == y.properties
                          && x.state == y.state && x.lifecycle == y.lifecycle
                          && x.version == y.version && x.revision == y.revision
                          && x.capabilities == y.capabilities
                          && x.relations.size() == y.relations.size();
        if (!same) {
            return false;
        }
        for (std::size_t rel = 0; rel < x.relations.size(); ++rel) {
            if (x.relations[rel].target != y.relations[rel].target
                || x.relations[rel].type != y.relations[rel].type
                || x.relations[rel].attributes != y.relations[rel].attributes) {
                return false;
            }
        }
    }
    return true;
}

// 测试夹具：anchor 均为合法构造，直接取 capture 结果。
[[nodiscard]] geoworld::persistence::CapturedCheckpoint
must_capture(const CheckpointCoordinator& coordinator, const CheckpointRegistry& registry,
             const CheckpointAnchor& anchor) {
    return coordinator.capture(registry, anchor).value;
}

[[nodiscard]] bool registry_order_dependencies_and_completeness() {
    CheckpointRegistry registry;
    auto base = std::make_shared<TestProvider>("a-base", 1);
    auto dependent = std::make_shared<TestProvider>("z-dep", 1, std::vector<std::string>{"a-base"});
    auto plain = std::make_shared<TestProvider>("m-plain", 1);
    if (registry.register_provider(dependent) != PersistenceError::none
        || registry.register_provider(plain) != PersistenceError::none
        || registry.register_provider(base) != PersistenceError::none) {
        return false;
    }
    // 重复注册与非法 schema 拒绝。
    if (registry.register_provider(std::make_shared<TestProvider>("a-base", 1))
        != PersistenceError::config_invalid) {
        return false;
    }
    // 捕获顺序按稳定 provider ID 排序，与注册顺序无关。
    const auto sorted = registry.providers_by_id();
    if (sorted.size() != 3 || sorted[0]->schema().provider_id != "a-base"
        || sorted[1]->schema().provider_id != "m-plain"
        || sorted[2]->schema().provider_id != "z-dep") {
        return false;
    }
    // 恢复顺序按依赖拓扑：z-dep 依赖 a-base，必须在其后。
    auto ordered = registry.providers_in_restore_order();
    if (!ordered.ok()) {
        return false;
    }
    std::size_t base_pos = 0;
    std::size_t dep_pos = 0;
    for (std::size_t index = 0; index < ordered.value.size(); ++index) {
        if (ordered.value[index]->schema().provider_id == "a-base") {
            base_pos = index;
        }
        if (ordered.value[index]->schema().provider_id == "z-dep") {
            dep_pos = index;
        }
    }
    if (base_pos >= dep_pos) {
        return false;
    }
    // 依赖环与未知依赖拒绝。
    CheckpointRegistry cyclic;
    static_cast<void>(cyclic.register_provider(
        std::make_shared<TestProvider>("x", 1, std::vector<std::string>{"y"})));
    static_cast<void>(cyclic.register_provider(
        std::make_shared<TestProvider>("y", 1, std::vector<std::string>{"x"})));
    if (cyclic.providers_in_restore_order().ok()) {
        return false;
    }
    CheckpointRegistry unknown_dep;
    static_cast<void>(unknown_dep.register_provider(
        std::make_shared<TestProvider>("x", 1, std::vector<std::string>{"ghost"})));
    if (unknown_dep.providers_in_restore_order().ok()) {
        return false;
    }
    // durable 完整性闸门：有状态模块缺失 provider 必须失败。
    CheckpointRegistry partial;
    static_cast<void>(partial.register_provider(std::make_shared<TestProvider>("world", 1)));
    if (partial.validate_completeness({"world", "clock"})
        != PersistenceError::provider_missing) {
        return false;
    }
    return registry.validate_completeness({"a-base", "m-plain", "z-dep"})
           == PersistenceError::none;
}

[[nodiscard]] bool world_provider_roundtrip_hash_and_counters() {
    world::World source = make_sample_world();
    const std::uint64_t source_hash = geoworld::debug::world_state_hash(source);

    world::World restored;
    auto capture_provider = geoworld::persistence::make_world_provider(source);
    auto restore_provider = geoworld::persistence::make_world_provider(restored);
    const auto frozen = capture_provider->freeze();
    const auto block = capture_provider->encode(frozen);
    if (restore_provider->restore(block.payload, block.schema.schema_version)
        != PersistenceError::none) {
        return false;
    }
    if (!worlds_equal(source, restored)) {
        return false;
    }
    // 恢复后语义等价：状态 hash 一致（revision 不参与 hash，但已一并还原）。
    if (geoworld::debug::world_state_hash(restored) != source_hash) {
        return false;
    }
    // 单调计数器还原：恢复后 insert 的 revision 接续原世界。
    auto* source_obj = source.find(WorldId{4});
    auto* restored_obj = restored.find(WorldId{4});
    if (source_obj != nullptr || restored_obj != nullptr) {
        return false;
    }
    static_cast<void>(source.insert(make_object(4)));
    static_cast<void>(restored.insert(make_object(4)));
    return source.find(WorldId{4})->revision == restored.find(WorldId{4})->revision
           && geoworld::debug::world_state_hash(source)
                  == geoworld::debug::world_state_hash(restored);
}

[[nodiscard]] bool clock_provider_roundtrip_and_dt_guard() {
    simulation::TickClock clock{simulation::TickConfig{20'000}};
    clock.advance();
    clock.advance();
    auto provider = geoworld::persistence::make_clock_provider(clock);
    const auto block = provider->encode(provider->freeze());

    simulation::TickClock restored{simulation::TickConfig{20'000}};
    auto restore_provider = geoworld::persistence::make_clock_provider(restored);
    if (restore_provider->restore(block.payload, block.schema.schema_version)
        != PersistenceError::none) {
        return false;
    }
    // 冻结于锚点（advance 之前）：恢复后下一 tick = 完成 tick + 1。
    if (restored.tick() != 3 || restored.dt_microseconds() != 20'000) {
        return false;
    }
    // 固定步长不一致拒绝恢复；版本不匹配拒绝。
    simulation::TickClock mismatched{simulation::TickConfig{40'000}};
    auto mismatched_provider = geoworld::persistence::make_clock_provider(mismatched);
    if (mismatched_provider->restore(block.payload, block.schema.schema_version)
        != PersistenceError::config_invalid) {
        return false;
    }
    return restore_provider->restore(block.payload, 99)
           == PersistenceError::provider_version_mismatch;
}

[[nodiscard]] bool capture_bytes_deterministic() {
    world::World first = make_sample_world();
    world::World second = make_sample_world();
    simulation::TickClock first_clock{};
    simulation::TickClock second_clock{};
    first_clock.advance();
    second_clock.advance();

    TempDir dir_a("det-a");
    TempDir dir_b("det-b");
    CheckpointRegistry registry_a;
    CheckpointRegistry registry_b;
    static_cast<void>(registry_a.register_provider(
        geoworld::persistence::make_world_provider(first)));
    static_cast<void>(registry_a.register_provider(
        geoworld::persistence::make_clock_provider(first_clock)));
    static_cast<void>(registry_b.register_provider(
        geoworld::persistence::make_world_provider(second)));
    static_cast<void>(registry_b.register_provider(
        geoworld::persistence::make_clock_provider(second_clock)));

    // provider 级：相同冻结状态两次编码逐字节一致。
    const auto provider = registry_a.find("world");
    const auto frozen = provider->freeze();
    if (provider->encode(frozen).payload != provider->encode(frozen).payload) {
        return false;
    }

    // 检查点级：两个独立目录、相同状态与锚点，数据块逐字节一致。
    const auto anchor = make_anchor(5, 12345);
    CheckpointCoordinator coordinator_a{make_checkpoint_config(dir_a.path)};
    CheckpointCoordinator coordinator_b{make_checkpoint_config(dir_b.path)};
    auto published_a =
        coordinator_a.publish(registry_a, must_capture(coordinator_a, registry_a, anchor));
    auto published_b =
        coordinator_b.publish(registry_b, must_capture(coordinator_b, registry_b, anchor));
    if (!published_a.ok() || !published_b.ok()) {
        return false;
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    CheckpointLoader loader_a{make_checkpoint_config(dir_a.path).layout, WorldId{42},
                              test_branch()};
    CheckpointLoader loader_b{make_checkpoint_config(dir_b.path).layout, WorldId{42},
                              test_branch()};
    const auto info_a = loader_a.latest_valid();
    const auto info_b = loader_b.latest_valid();
    if (!info_a.ok() || !info_b.ok()) {
        return false;
    }
    const auto bytes_a = ops->read_file(info_a.value.data_path);
    const auto bytes_b = ops->read_file(info_b.value.data_path);
    return bytes_a.ok() && bytes_b.ok() && bytes_a.value == bytes_b.value
           && !bytes_a.value.empty();
}

[[nodiscard]] bool capture_isolation_from_later_mutation() {
    world::World world = make_sample_world();
    simulation::TickClock clock{};
    CheckpointRegistry registry;
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_world_provider(world)));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_clock_provider(clock)));
    const std::uint64_t captured_hash = geoworld::debug::world_state_hash(world);

    TempDir dir("isolation");
    CheckpointCoordinator coordinator{make_checkpoint_config(dir.path)};
    auto captured = coordinator.capture(registry, make_anchor(7, captured_hash));
    if (!captured.ok()) {
        return false;
    }
    // 捕获后继续改变世界：新增、删除、修改。发布与恢复不得观察这些变化。
    static_cast<void>(world.insert(make_object(9)));
    static_cast<void>(world.erase(WorldId{1}));
    static_cast<void>(world.set_property(WorldId{2}, "speed", 99.0));
    auto published = coordinator.publish(registry, std::move(captured.value));
    if (!published.ok()) {
        return false;
    }

    world::World restored_world;
    simulation::TickClock restored_clock{};
    CheckpointRegistry restore_registry;
    static_cast<void>(restore_registry.register_provider(
        geoworld::persistence::make_world_provider(restored_world)));
    static_cast<void>(restore_registry.register_provider(
        geoworld::persistence::make_clock_provider(restored_clock)));
    CheckpointLoader loader{make_checkpoint_config(dir.path).layout, WorldId{42},
                            test_branch()};
    auto latest = loader.latest_valid();
    if (!latest.ok()) {
        return false;
    }
    auto loaded = loader.load(latest.value);
    if (!loaded.ok()
        || loader.restore_into(restore_registry, loaded.value) != PersistenceError::none) {
        return false;
    }
    return geoworld::debug::world_state_hash(restored_world) == captured_hash
           && restored_world.find(WorldId{9}) == nullptr
           && restored_world.find(WorldId{1}) != nullptr
           && loaded.value.info.anchor.resume_tick == 8;
}

[[nodiscard]] bool corrupted_checkpoint_rejected() {
    world::World world = make_sample_world();
    simulation::TickClock clock{};
    CheckpointRegistry registry;
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_world_provider(world)));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_clock_provider(clock)));

    TempDir dir("corrupt");
    const auto config = make_checkpoint_config(dir.path);
    CheckpointCoordinator coordinator{config};
    auto published = coordinator.publish(
        registry, must_capture(coordinator, registry, make_anchor(3, 777)));
    if (!published.ok()) {
        return false;
    }
    CheckpointLoader loader{config.layout, WorldId{42}, test_branch()};
    auto latest = loader.latest_valid();
    if (!latest.ok()) {
        return false;
    }
    // 数据块翻转一字节：CRC32C 校验失败，load 拒绝。
    auto data_bytes = geoworld::persistence::make_posix_file_ops()->read_file(
        latest.value.data_path);
    if (!data_bytes.ok() || data_bytes.value.empty()) {
        return false;
    }
    data_bytes.value.back() = data_bytes.value.back() ^ std::byte{0xFF};
    {
        std::ofstream stream(latest.value.data_path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data_bytes.value.data()),
                     static_cast<std::streamsize>(data_bytes.value.size()));
    }
    const auto corrupted_load = loader.load(latest.value);
    if (corrupted_load.ok() || corrupted_load.error != PersistenceError::checksum_mismatch) {
        return false;
    }
    // 损坏后不再成为恢复候选。
    if (loader.latest_valid().ok()) {
        return false;
    }
    // 截断 manifest：同样被排除。
    TempDir truncated_dir("truncated");
    const auto truncated_config = make_checkpoint_config(truncated_dir.path);
    CheckpointCoordinator truncated_coordinator{truncated_config};
    auto truncated_published = truncated_coordinator.publish(
        registry, must_capture(truncated_coordinator, registry, make_anchor(4, 888)));
    if (!truncated_published.ok()) {
        return false;
    }
    const auto truncated_path = truncated_published.value.manifest_path;
    const auto size = std::filesystem::file_size(truncated_path);
    std::filesystem::resize_file(truncated_path, size / 2);
    CheckpointLoader truncated_loader{truncated_config.layout, WorldId{42}, test_branch()};
    return !truncated_loader.latest_valid().ok();
}

[[nodiscard]] bool corrupted_latest_falls_back_to_previous_generation() {
    world::World world = make_sample_world();
    simulation::TickClock clock{};
    CheckpointRegistry registry;
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_world_provider(world)));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_clock_provider(clock)));
    TempDir dir("fallback");
    const auto config = make_checkpoint_config(dir.path);
    CheckpointCoordinator coordinator{config};
    for (std::uint64_t tick = 1; tick <= 2; ++tick) {
        auto published = coordinator.publish(
            registry, must_capture(coordinator, registry, make_anchor(tick, tick + 100)));
        if (!published.ok()) return false;
    }
    CheckpointLoader loader{config.layout, WorldId{42}, test_branch()};
    auto latest = loader.latest_valid();
    if (!latest.ok() || latest.value.anchor.completed_tick != 2) return false;
    auto bytes = geoworld::persistence::make_posix_file_ops()->read_file(latest.value.data_path);
    if (!bytes.ok() || bytes.value.empty()) return false;
    bytes.value[bytes.value.size() / 2] ^= std::byte{0x5a};
    std::ofstream stream(latest.value.data_path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.value.data()),
                 static_cast<std::streamsize>(bytes.value.size()));
    stream.close();
    const auto fallback = loader.latest_valid();
    return fallback.ok() && fallback.value.anchor.completed_tick == 1;
}

[[nodiscard]] bool unknown_provider_and_missing_block_rejected() {
    world::World world = make_sample_world();
    simulation::TickClock clock{};
    auto extra = std::make_shared<TestProvider>("extra", 1);
    extra->state = 55;
    CheckpointRegistry full_registry;
    static_cast<void>(full_registry.register_provider(
        geoworld::persistence::make_world_provider(world)));
    static_cast<void>(full_registry.register_provider(
        geoworld::persistence::make_clock_provider(clock)));
    static_cast<void>(full_registry.register_provider(extra));

    TempDir dir("unknown");
    const auto config = make_checkpoint_config(dir.path);
    CheckpointCoordinator coordinator{config};
    auto published = coordinator.publish(
        full_registry, must_capture(coordinator, full_registry, make_anchor(2, 999)));
    if (!published.ok()) {
        return false;
    }
    CheckpointLoader loader{config.layout, WorldId{42}, test_branch()};
    auto loaded = loader.load(loader.latest_valid().value);
    if (!loaded.ok()) {
        return false;
    }
    // 恢复注册表缺少 extra：检查点含未知 provider，拒绝。
    world::World restored_world;
    simulation::TickClock restored_clock{};
    CheckpointRegistry narrower;
    static_cast<void>(narrower.register_provider(
        geoworld::persistence::make_world_provider(restored_world)));
    static_cast<void>(narrower.register_provider(
        geoworld::persistence::make_clock_provider(restored_clock)));
    if (loader.restore_into(narrower, loaded.value) != PersistenceError::provider_unknown) {
        return false;
    }
    // 反向：注册表有 extra 而检查点（缺 extra 的）不含其块，拒绝。
    TempDir narrow_dir("narrow");
    const auto narrow_config = make_checkpoint_config(narrow_dir.path);
    CheckpointCoordinator narrow_coordinator{narrow_config};
    auto narrow_published = narrow_coordinator.publish(
        narrower, must_capture(narrow_coordinator, narrower, make_anchor(2, 555)));
    if (!narrow_published.ok()) {
        return false;
    }
    CheckpointLoader narrow_loader{narrow_config.layout, WorldId{42}, test_branch()};
    auto narrow_loaded = narrow_loader.load(narrow_loader.latest_valid().value);
    if (!narrow_loaded.ok()) {
        return false;
    }
    CheckpointRegistry wider;
    static_cast<void>(wider.register_provider(
        geoworld::persistence::make_world_provider(restored_world)));
    static_cast<void>(wider.register_provider(
        geoworld::persistence::make_clock_provider(restored_clock)));
    static_cast<void>(wider.register_provider(std::make_shared<TestProvider>("extra", 1)));
    if (narrow_loader.restore_into(wider, narrow_loaded.value)
        != PersistenceError::checkpoint_incomplete) {
        return false;
    }
    // provider 版本不匹配拒绝（无 upcaster）：须用含 extra 块的完整检查点。
    CheckpointRegistry wrong_version;
    static_cast<void>(wrong_version.register_provider(
        geoworld::persistence::make_world_provider(restored_world)));
    static_cast<void>(wrong_version.register_provider(
        geoworld::persistence::make_clock_provider(restored_clock)));
    static_cast<void>(
        wrong_version.register_provider(std::make_shared<TestProvider>("extra", 2)));
    if (loader.restore_into(wrong_version, loaded.value)
        != PersistenceError::provider_version_mismatch) {
        return false;
    }
    auto migrating = std::make_shared<UpcastingTestProvider>();
    CheckpointRegistry migrated;
    static_cast<void>(migrated.register_provider(
        geoworld::persistence::make_world_provider(restored_world)));
    static_cast<void>(migrated.register_provider(
        geoworld::persistence::make_clock_provider(restored_clock)));
    static_cast<void>(migrated.register_provider(migrating));
    return loader.restore_into(migrated, loaded.value) == PersistenceError::none
           && migrating->state == 55;
}

[[nodiscard]] bool retention_recycles_only_published() {
    world::World world = make_sample_world();
    simulation::TickClock clock{};
    CheckpointRegistry registry;
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_world_provider(world)));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_clock_provider(clock)));

    TempDir dir("retention");
    auto config = make_checkpoint_config(dir.path);
    config.prune_wal_after_publish = true;
    config.keep_last_checkpoints = 2;
    CheckpointCoordinator coordinator{config};
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        auto published = coordinator.publish(
            registry, must_capture(coordinator, registry, make_anchor(tick, tick * 100)));
        if (!published.ok()) {
            return false;
        }
    }
    // 保留最近 2 代：tick 1 的检查点对已回收，tick 2/3 保留。
    std::size_t manifest_count = 0;
    std::size_t data_count = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(config.layout.checkpoint_dir())) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".gwckm")) {
            ++manifest_count;
        }
        if (name.ends_with(".gwck")) {
            ++data_count;
        }
    }
    if (manifest_count != 2 || data_count != 2) {
        return false;
    }
    if (std::filesystem::exists(config.layout.checkpoint_dir()
                                / "ckpt-00000000000000000001.gwckm")) {
        return false;
    }
    CheckpointLoader loader{config.layout, WorldId{42}, test_branch()};
    auto latest = loader.latest_valid();
    return latest.ok() && latest.value.anchor.completed_tick == 3;
}

[[nodiscard]] bool wal_segments_pruned_after_publish() {
    TempDir dir("prune");
    const auto identity_world = WorldId{42};
    const auto identity_branch = test_branch();
    const auto layout = geoworld::persistence::make_durable_layout(dir.path, identity_world,
                                                                   identity_branch);

    // WAL：小 segment 强制 rotation，制造 seg-1/seg-3 关闭段与 seg-5 活跃段。
    WalConfig wal_config;
    wal_config.durable_root = dir.path;
    wal_config.world = identity_world;
    wal_config.branch = identity_branch;
    wal_config.segment_max_bytes = 16 + 2 * (4 + 24 + 8 + 4);
    wal_config.max_record_bytes = 4 + 24 + 8 + 4;
    {
        WalWriter writer(wal_config);
        if (!writer.start().ok()) {
            return false;
        }
        for (std::uint64_t index = 0; index < 6; ++index) {
            WalRecord record;
            record.kind = WalRecordKind::external_command;
            record.target_tick = index;
            record.payload.assign(8, std::byte{'p'});
            auto ticket = writer.append(std::move(record));
            if (!ticket.ok() || !ticket.value.wait().ok()) {
                return false;
            }
        }
        writer.shutdown();
    }

    world::World world = make_sample_world();
    simulation::TickClock clock{};
    CheckpointRegistry registry;
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_world_provider(world)));
    static_cast<void>(registry.register_provider(
        geoworld::persistence::make_clock_provider(clock)));

    auto config = make_checkpoint_config(dir.path);
    config.prune_wal_after_publish = true;
    CheckpointCoordinator coordinator{config};
    // included_lsn = 2：seg-1（LSN 1-2）被覆盖可回收，seg-3/活跃段保留。
    auto published = coordinator.publish(
        registry, must_capture(coordinator, registry, make_anchor(9, 42, Lsn{2})));
    if (!published.ok()) {
        return false;
    }
    const auto ops = geoworld::persistence::make_posix_file_ops();
    const auto remaining = ops->list_files(layout.wal_dir());
    if (!remaining.ok()) {
        return false;
    }
    bool has_seg1 = false;
    bool has_seg3 = false;
    bool has_active = false;
    for (const auto& path : remaining.value) {
        const std::string name = path.filename().string();
        has_seg1 = has_seg1 || name == "seg-00000000000000000001.gwal";
        has_seg3 = has_seg3 || name == "seg-00000000000000000003.gwal";
        has_active = has_active || name == "seg-00000000000000000005.gwal.active";
    }
    if (has_seg1 || !has_seg3 || !has_active) {
        return false;
    }

    RecoveryPlanner planner{layout, identity_world, identity_branch};
    const auto recovery = planner.build(TailPolicy::strict);
    if (!recovery.ok() || !recovery.value.checkpoint.has_value()
        || recovery.value.replay_records.size() != 4
        || recovery.value.replay_records.front().lsn != Lsn{3}
        || recovery.value.replay_records.back().lsn != Lsn{6}) {
        return false;
    }

    WalWriter unanchored{wal_config};
    if (unanchored.start().error != PersistenceError::lsn_discontinuity) {
        return false;
    }
    wal_config.recovery_floor_lsn = Lsn{2};
    WalWriter resumed{wal_config};
    if (!resumed.start().ok()) {
        return false;
    }
    WalRecord appended;
    appended.kind = WalRecordKind::state_hash;
    appended.target_tick = 10;
    appended.payload.assign(8, std::byte{'r'});
    auto ticket = resumed.append(std::move(appended));
    if (!ticket.ok()) {
        return false;
    }
    const auto outcome = ticket.value.wait();
    resumed.shutdown();
    return outcome.ok() && outcome.lsn == Lsn{7};
}

} // namespace

int main() {
    if (!registry_order_dependencies_and_completeness()) {
        return 1;
    }
    if (!world_provider_roundtrip_hash_and_counters()) {
        return 2;
    }
    if (!clock_provider_roundtrip_and_dt_guard()) {
        return 3;
    }
    if (!capture_bytes_deterministic()) {
        return 4;
    }
    if (!capture_isolation_from_later_mutation()) {
        return 5;
    }
    if (!corrupted_checkpoint_rejected()) {
        return 6;
    }
    if (!corrupted_latest_falls_back_to_previous_generation()) {
        return 10;
    }
    if (!unknown_provider_and_missing_block_rejected()) {
        return 7;
    }
    if (!retention_recycles_only_published()) {
        return 8;
    }
    if (!wal_segments_pruned_after_publish()) {
        return 9;
    }
    return 0;
}
