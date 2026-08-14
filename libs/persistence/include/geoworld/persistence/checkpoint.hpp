#pragma once

#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/types.hpp"
#include "geoworld/ai/decision.hpp"
#include "geoworld/ecs/runtime.hpp"
#include "geoworld/foundation/random.hpp"
#include "geoworld/rules/event_bus.hpp"
#include "geoworld/simulation/command_buffer.hpp"
#include "geoworld/simulation/tick.hpp"
#include "geoworld/tooling/artifact.hpp"
#include "geoworld/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <compare>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace geoworld::persistence {

// 本批次核心 provider 的稳定 ID；注册即冻结，后续只能新增不能改名。
inline constexpr std::string_view kWorldProviderId = "world";
inline constexpr std::string_view kClockProviderId = "clock";
inline constexpr std::string_view kCommandBufferProviderId = "command_buffer";
inline constexpr std::string_view kEventBusProviderId = "event_bus";
inline constexpr std::string_view kAiIntentsProviderId = "ai_intents";
inline constexpr std::string_view kRandomStreamsProviderId = "random_streams";
inline constexpr std::string_view kArtifactsProviderId = "artifacts";
inline constexpr std::string_view kEcsActiveSetProviderId = "ecs_active_set";

// provider 自有 schema 版本；演进必须走显式逐版本 upcaster，不原地兼容。
struct CheckpointSchema {
    std::string provider_id;
    std::uint32_t schema_version{};

    auto operator<=>(const CheckpointSchema&) const = default;
};

// provider 在稳定 tick 边界自有的不可变冻结状态；后台线程只允许访问该副本。
struct FrozenProviderState {
    FrozenProviderState() = default;
    FrozenProviderState(std::shared_ptr<const void> frozen,
                        std::optional<Lsn> watermark = std::nullopt)
        : data(std::move(frozen)), included_lsn(watermark) {}

    std::shared_ptr<const void> data;
    // 仅输入队列 provider 设置；协调器以冻结值推导检查点水位，调用方不能伪造。
    std::optional<Lsn> included_lsn;
};

// 编码后的 provider 块：未压缩 payload，逐字节确定。
struct CheckpointBlock {
    CheckpointSchema schema;
    std::vector<std::byte> payload;
};

class CheckpointProvider {
public:
    virtual ~CheckpointProvider() = default;

    [[nodiscard]] virtual CheckpointSchema schema() const = 0;
    // 恢复顺序依赖：返回必须先于本 provider 恢复的稳定 provider ID。
    [[nodiscard]] virtual std::vector<std::string> restore_dependencies() const {
        return {};
    }
    // 稳定 tick 边界在 tick 线程调用：返回自有不可变副本，不得保留可写对象引用。
    [[nodiscard]] virtual FrozenProviderState freeze() const = 0;
    // 后台线程调用：相同冻结状态、版本和配置必须产生逐字节一致的 payload。
    [[nodiscard]] virtual CheckpointBlock encode(const FrozenProviderState& frozen) const = 0;
    // 恢复采用 validate-all/commit-all 两阶段，避免后一个 provider 失败后留下半恢复状态。
    [[nodiscard]] virtual PersistenceError validate_restore(
        std::span<const std::byte>, std::uint32_t schema_version) const {
        return schema_version == schema().schema_version
                   ? PersistenceError::none
                   : PersistenceError::provider_version_mismatch;
    }
    // 仅允许逐版本显式迁移；默认拒绝。返回 payload 必须是当前 schema() 版本。
    [[nodiscard]] virtual Result<std::vector<std::byte>> upcast(
        std::span<const std::byte>, std::uint32_t) const {
        return {{}, PersistenceError::provider_version_mismatch};
    }
    // eager 恢复：校验版本并重建 provider 状态；版本不兼容返回 provider_version_mismatch。
    [[nodiscard]] virtual PersistenceError restore(std::span<const std::byte> payload,
                                                   std::uint32_t schema_version) = 0;
};

// provider 注册表：捕获按稳定 provider ID 排序，恢复按依赖拓扑排序（同层按 ID）。
class CheckpointRegistry {
public:
    [[nodiscard]] PersistenceError register_provider(std::shared_ptr<CheckpointProvider> provider);
    [[nodiscard]] std::vector<std::shared_ptr<CheckpointProvider>> providers_by_id() const;
    // 恢复顺序拓扑排序；环或未知依赖返回 checkpoint_invalid。
    [[nodiscard]] Result<std::vector<std::shared_ptr<CheckpointProvider>>>
    providers_in_restore_order() const;
    [[nodiscard]] std::shared_ptr<CheckpointProvider> find(std::string_view provider_id) const;
    // durable 模式完整性闸门：任何有状态模块缺失 provider 即返回 provider_missing。
    [[nodiscard]] PersistenceError validate_completeness(
        const std::vector<std::string_view>& authoritative_modules) const;

private:
    std::vector<std::shared_ptr<CheckpointProvider>> providers_;
};

// 检查点锚点，语义冻结于 docs/M5.md 稳定边界一节。
struct CheckpointAnchor {
    std::uint64_t completed_tick{};
    std::uint64_t resume_tick{};
    Lsn included_lsn{};
    std::uint64_t world_state_hash{};
};

// 保留策略默认值（命名常量；业务阈值进 CheckpointConfig）。
inline constexpr std::size_t kDefaultCheckpointsKept = 2;
inline constexpr int kDefaultCheckpointCompressionLevel = 3;
inline constexpr std::size_t kDefaultCheckpointCompressionMinimumBytes = 64U * 1024U;

struct CheckpointConfig {
    DurableLayout layout{};
    geoworld::foundation::WorldId world{};
    BranchId branch{};
    // 已发布且验证通过的最近检查点保留数量。
    std::size_t keep_last_checkpoints{kDefaultCheckpointsKept};
    // 发布成功后回收被 included_lsn 覆盖的已关闭 WAL segment。
    bool prune_wal_after_publish{false};
    // 生产 durable 模式必须提供完整权威集合；capture 会强制校验。
    std::vector<std::string_view> authoritative_modules;
    bool compression_enabled{true};
    int compression_level{kDefaultCheckpointCompressionLevel};
    std::size_t compression_minimum_bytes{kDefaultCheckpointCompressionMinimumBytes};
};

// 已在 tick 边界冻结的检查点：自有全部 provider 块，可移交后台线程发布。
struct CapturedCheckpoint {
    CheckpointAnchor anchor{};
    std::vector<FrozenProviderState> frozen;
    std::vector<CheckpointSchema> schemas;
};

struct PublishedCheckpoint {
    CheckpointAnchor anchor{};
    std::uint64_t checkpoint_content_hash{};
    std::filesystem::path manifest_path;
    std::uint64_t data_length{};
};

// 捕获/发布编排：capture 在稳定 tick 边界调用（只冻结，不编码），
// publish 可在后台线程执行编码、校验、原子发布、重新打开校验与保留回收。
class CheckpointCoordinator {
public:
    CheckpointCoordinator(CheckpointConfig config, std::shared_ptr<FileOps> file_ops = {});

    // 第 1-2 步：按稳定 provider ID 顺序冻结全部 provider。
    [[nodiscard]] Result<CapturedCheckpoint> capture(const CheckpointRegistry& registry,
                                                     const CheckpointAnchor& anchor) const;
    // 第 3-8 步：确定性编码 -> 数据临时文件 flush -> manifest 临时文件 flush ->
    // 原子 rename + 目录 flush -> 重新打开校验 -> 保留回收。任何一步失败保留旧检查点。
    [[nodiscard]] Result<PublishedCheckpoint> publish(const CheckpointRegistry& registry,
                                                      CapturedCheckpoint captured);

private:
    CheckpointConfig config_;
    std::shared_ptr<FileOps> ops_;
};

// world/clock 核心 provider 工厂：捕获读、恢复写同一对象；
// freeze 只在稳定边界被调用，恢复只在服务开放前被调用。
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_world_provider(world::World& world);
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_clock_provider(
    simulation::TickClock& clock);
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_command_buffer_provider(
    simulation::CommandBuffer& commands);
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_event_bus_provider(rules::EventBus& events);
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_ai_intents_provider(
    ai::DecisionIntentBuffer& intents);
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_random_streams_provider(
    foundation::NamedRandomStreams& streams);
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_artifacts_provider(
    tooling::ArtifactManifest& artifacts);
[[nodiscard]] std::shared_ptr<CheckpointProvider> make_ecs_active_set_provider(
    ecs::Runtime& runtime, world::World& world);

} // namespace geoworld::persistence
