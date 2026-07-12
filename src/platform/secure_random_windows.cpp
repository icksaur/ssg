#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace ssg {

void platform_secure_random(std::span<std::byte> bytes) {
    if (bytes.size() > std::numeric_limits<ULONG>::max()) {
        throw std::runtime_error{"secure random request is too large"};
    }
    auto const status = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        throw std::runtime_error{"BCryptGenRandom failed"};
    }
}

}  // namespace ssg
