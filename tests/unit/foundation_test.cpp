#include "geoworld/foundation/version.hpp"
#include "geoworld/foundation/ids.hpp"
#include "geoworld/foundation/random.hpp"

int main() {
    using namespace geoworld::foundation;
    if (major_version != 0 || WorldId{0}.valid() || !WorldId{1}.valid()) {
        return 1;
    }
    if (RuntimeId{4, 2} == RuntimeId{4, 3}) {
        return 1;
    }
    DeterministicRng left{1234};
    DeterministicRng right{1234};
    return left.next_u64() == right.next_u64() ? 0 : 1;
}
