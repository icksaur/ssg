#include <ssg/application_auth.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace ssg {

void platform_secure_random(std::span<std::byte> bytes);

namespace {

class PlatformSecureRandom final : public SecureRandomSource {
public:
    void fill(std::span<std::byte> bytes) override {
        platform_secure_random(bytes);
    }
};

bool credentials_equal(std::string_view left, std::string_view right) {
    std::uint8_t difference =
        static_cast<std::uint8_t>(left.size() ^ right.size());
    auto const common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
        difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0 && left.size() == right.size();
}

}  // namespace

BearerCredential generate_bearer_credential(SecureRandomSource& source) {
    std::array<std::byte, 32> random{};
    source.fill(random);
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(random.size() * 2);
    for (auto byte : random) {
        auto const value = std::to_integer<unsigned>(byte);
        encoded.push_back(digits[value >> 4]);
        encoded.push_back(digits[value & 0x0f]);
    }
    return BearerCredential{std::move(encoded)};
}

BearerCredential generate_bearer_credential() {
    PlatformSecureRandom source;
    return generate_bearer_credential(source);
}

ApplicationAuthentication::ApplicationAuthentication(
    BearerCredential credential, SessionId session_id, ClientId client_id,
    ViewId view_id)
    : credential_{std::move(credential)},
      session_id_{std::move(session_id)},
      client_id_{client_id},
      view_id_{view_id} {}

std::optional<AuthenticatedSession> ApplicationAuthentication::authenticate(
    std::string_view presented_credential) const {
    if (!credentials_equal(credential_.value(), presented_credential)) {
        return std::nullopt;
    }
    return AuthenticatedSession{
        session_id_,
        InvocationPrincipal{
            client_id_, InvocationOrigin::websocket,
            std::vector<CapabilityId>{CapabilityId{"local_file_drop"}}},
        view_id_};
}

}  // namespace ssg
