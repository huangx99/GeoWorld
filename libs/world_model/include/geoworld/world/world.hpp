#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/schema/value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace geoworld::world {

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
    [[nodiscard]] bool insert(WorldObject object);
    [[nodiscard]] bool erase(WorldId id);
    [[nodiscard]] WorldObject* find(WorldId id) noexcept;
    [[nodiscard]] const WorldObject* find(WorldId id) const noexcept;
    [[nodiscard]] bool set_property(WorldId id, std::string key, PropertyValue value);
    [[nodiscard]] bool add_relation(WorldId source, Relation relation);
    [[nodiscard]] std::vector<WorldObject> snapshot() const;
    // 成功 erase 的累计次数：读侧可据此跳过无删除期间的失效扫描。
    [[nodiscard]] std::uint64_t erase_revision() const noexcept { return erase_revision_; }
    // 免拷贝只读遍历；迭代顺序未定义，调用方不得依赖顺序。
    template <typename Callback>
    void for_each_object(Callback&& callback) const {
        for (const auto& [id, object] : objects_) {
            static_cast<void>(id);
            callback(object);
        }
    }
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<WorldId, WorldObject, foundation::WorldIdHash> objects_;
    std::uint64_t next_revision_{};
    std::uint64_t erase_revision_{};
};

} // namespace geoworld::world
