#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/schema/value.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace geoworld::world {

class FrozenWorldSnapshot;

using WorldId = foundation::WorldId;

using PropertyValue = schema::Value;
using PropertyBag = schema::PropertyBag;

struct PositionEcef {
    double x{};
    double y{};
    double z{};
};

enum class LifecycleState { staged, active, suspended, retired };

struct Relation {
    WorldId target;
    std::string type;
    PropertyBag attributes;
};

struct WorldObject {
    WorldId id;
    std::string geometry_ref;
    PositionEcef position;
    std::string semantic_type;
    PropertyBag properties;
    PropertyBag state;
    std::vector<Relation> relations;
    std::vector<std::string> capabilities;
    LifecycleState lifecycle{LifecycleState::staged};
    std::uint64_t version{1};
    // 由 World::insert 分配的世界内单调序号；同一 WID 删除后重建必得到新值。
    // 仅供读侧做脏检查，不参与状态 hash 与规范化投影。
    std::uint64_t revision{};
};

class World {
public:
    World();
    ~World();
    World(World&&) noexcept;
    World& operator=(World&&) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    [[nodiscard]] bool insert(WorldObject object);
    [[nodiscard]] bool erase(WorldId id);
    [[nodiscard]] const WorldObject* find(WorldId id) const noexcept;
    [[nodiscard]] bool update(WorldId id,
                              const std::function<void(WorldObject&)>& mutation);
    [[nodiscard]] bool set_property(WorldId id, std::string key, PropertyValue value);
    [[nodiscard]] bool add_relation(WorldId source, Relation relation);
    [[nodiscard]] std::vector<WorldObject> snapshot() const;
    // 成功 erase 的累计次数：读侧可据此跳过无删除期间的失效扫描。
    [[nodiscard]] std::uint64_t erase_revision() const noexcept { return erase_revision_; }
    // insert 已分配到的单调序号；供快照恢复还原。
    [[nodiscard]] std::uint64_t next_revision() const noexcept { return next_revision_; }
    // COW 导致对象地址变化时递增；仅供缓存裸指针的读侧刷新索引。
    [[nodiscard]] std::uint64_t storage_revision() const noexcept;
    // 恢复专用：整体替换世界内容与单调计数器，不经过 insert 的序号分配。
    void restore(std::vector<WorldObject> objects, std::uint64_t next_revision,
                 std::uint64_t erase_revision);
    // 免拷贝只读遍历；迭代顺序未定义，调用方不得依赖顺序。
    void for_each_object(const std::function<void(const WorldObject&)>& callback) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    [[nodiscard]] WorldObject* writable_find(WorldId id) noexcept;

    struct Storage;
    std::unique_ptr<Storage> storage_;
    std::uint64_t next_revision_{};
    std::uint64_t erase_revision_{};

    friend FrozenWorldSnapshot freeze_snapshot(const World& world);
};

} // namespace geoworld::world
