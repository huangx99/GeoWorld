#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/spatial/coordinates.hpp"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace geoworld::spatial {

struct CellKey {
    std::int64_t x{};
    std::int64_t y{};
    std::uint8_t level{};
    std::int32_t z{};

    bool operator==(const CellKey&) const = default;
    auto operator<=>(const CellKey&) const = default;
};

struct CellKeyHash {
    std::size_t operator()(const CellKey& key) const noexcept;
};

class CellGrid {
public:
    explicit CellGrid(double base_cell_size_meters = 128.0,
                      std::uint8_t max_level = 8) noexcept;

    [[nodiscard]] double cell_size(std::uint8_t level) const noexcept;
    [[nodiscard]] CellKey cell_for(Enu position, std::uint8_t level = 0) const noexcept;
    [[nodiscard]] CellKey cell_for_2_5d(Enu position, std::uint8_t level,
                                         double floor_height_meters) const noexcept;
    [[nodiscard]] CellKey parent(CellKey child) const noexcept;
    [[nodiscard]] std::vector<CellKey> neighbors(CellKey cell) const;

private:
    double base_cell_size_meters_;
    std::uint8_t max_level_;
};

struct CellMigration {
    foundation::WorldId world_id;
    CellKey previous;
    CellKey current;
};

class DynamicCellIndex {
public:
    explicit DynamicCellIndex(CellGrid grid, std::uint8_t level = 0,
                              double floor_height_meters = 0.0);

    [[nodiscard]] std::optional<CellMigration> upsert(foundation::WorldId id, Enu position);
    [[nodiscard]] bool erase(foundation::WorldId id);
    [[nodiscard]] std::vector<foundation::WorldId> members(CellKey cell) const;
    [[nodiscard]] std::vector<foundation::WorldId> query_candidates(Enu minimum,
                                                                     Enu maximum) const;
    [[nodiscard]] std::optional<CellKey> cell_of(foundation::WorldId id) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    CellGrid grid_;
    std::uint8_t level_;
    double floor_height_meters_;
    std::unordered_map<CellKey, std::unordered_set<foundation::WorldId,
                                                    foundation::WorldIdHash>, CellKeyHash> members_;
    std::unordered_map<foundation::WorldId, CellKey, foundation::WorldIdHash> locations_;
};

} // namespace geoworld::spatial
