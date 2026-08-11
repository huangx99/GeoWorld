#include "geoworld/world/world.hpp"

#include <iostream>

int main() {
    geoworld::world::WorldObject object;
    object.id = geoworld::foundation::WorldId{42};
    std::cout << "geoworld-cli: WID=" << object.id.value << '\n';
    return 0;
}
