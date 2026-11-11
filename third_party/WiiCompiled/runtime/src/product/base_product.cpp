#include "runtime_product.h"
#include "prime_port_version.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::BaseGame,
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        PRIME_PORT_NAME,
#else
        "WiiCompiled",
#endif
    };
    return descriptor;
}

} // namespace RuntimeProduct
