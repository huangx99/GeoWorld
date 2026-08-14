#pragma once

#include "geoworld/world/world.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace geoworld::world {

// 世界权威快照：全部语义对象（按 id 排序）与内部单调计数器。
// revision/erase_revision 不参与状态 hash，但影响后续 insert/erase 的序号分配，
// 恢复时必须原样还原，否则同一 WID 重建后的行为与崩溃前分叉。
struct WorldSnapshot {
    std::vector<WorldObject> objects;
    std::uint64_t next_revision{};
    std::uint64_t erase_revision{};
};

class FrozenWorldSnapshot {
public:
    FrozenWorldSnapshot() = default;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t next_revision() const noexcept;
    [[nodiscard]] std::uint64_t erase_revision() const noexcept;
    [[nodiscard]] const WorldObject* find(WorldId id) const noexcept;
    void for_each_object(const std::function<void(const WorldObject&)>& callback) const;

private:
    explicit FrozenWorldSnapshot(std::shared_ptr<const void> state)
        : state_(std::move(state)) {}
    std::shared_ptr<const void> state_;

    friend FrozenWorldSnapshot freeze_snapshot(const World& world);
};

// 持久化根快照为 O(1)；对象块由成熟持久化容器共享，后续写入走路径复制。
[[nodiscard]] FrozenWorldSnapshot freeze_snapshot(const World& world);

[[nodiscard]] WorldSnapshot capture_snapshot(const World& world) noexcept;

// 恢复专用：整体替换世界内容并恢复单调计数器。仅供持久化恢复路径调用。
void restore_snapshot(World& world, WorldSnapshot snapshot);

} // namespace geoworld::world
