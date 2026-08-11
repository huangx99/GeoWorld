#include "geoworld/spatial/s2_key.hpp"

#if GW_HAS_S2
#include <s2/s2cell_id.h>
#include <s2/s2latlng.h>
#endif

namespace geoworld::spatial {

std::optional<S2GlobalKey> s2_key_for(Geodetic coordinate, std::uint8_t level) {
#if GW_HAS_S2
    if (level > 30) {
        return std::nullopt;
    }
    const auto lat_lng = S2LatLng::FromDegrees(
        coordinate.latitude_degrees, coordinate.longitude_degrees);
    if (!lat_lng.is_valid()) {
        return std::nullopt;
    }
    const auto cell = S2CellId{lat_lng}.parent(level);
    return S2GlobalKey{cell.id(), level};
#else
    static_cast<void>(coordinate);
    static_cast<void>(level);
    return std::nullopt;
#endif
}

} // namespace geoworld::spatial
