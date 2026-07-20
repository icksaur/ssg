#include <cstddef>
#include <span>
#include <stdexcept>
#include <system_error>

#include <cerrno>
#include <sys/random.h>

namespace ssg {

void platformSecureRandom(std::span<std::byte> bytes) {
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        auto const count =
            getrandom(bytes.data() + filled, bytes.size() - filled, 0);
        if (count > 0) {
            filled += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::system_error{
                errno, std::generic_category(), "getrandom failed"};
        }
    }
}

}  // namespace ssg
