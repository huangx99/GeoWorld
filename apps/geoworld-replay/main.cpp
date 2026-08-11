#include "geoworld/simulation/tick.hpp"

#include <iostream>

int main() {
    geoworld::simulation::TickClock clock;
    std::cout << "geoworld-replay: tick=" << clock.tick() << '\n';
    return 0;
}
