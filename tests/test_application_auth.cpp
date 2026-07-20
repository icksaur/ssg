#include "test_helpers.h"

#include <ssg/application_auth.h>

#include <array>
#include <cstddef>
#include <span>
#include <set>
#include <stdexcept>
#include <string>

namespace {

class FailingRandom final : public ssg::SecureRandomSource {
public:
    void fill(std::span<std::byte>) override {
        throw std::runtime_error{"injected random failure"};
    }
};

TEST(generated_bearers_have_32_random_bytes_in_lowercase_hex) {
    std::set<std::string> values;
    for (int count = 0; count < 64; ++count) {
        auto credential = ssg::generate_bearer_credential();
        ASSERT_EQ(credential.value().size(), std::size_t{64});
        for (char value : credential.value()) {
            ASSERT_TRUE((value >= '0' && value <= '9') ||
                        (value >= 'a' && value <= 'f'));
        }
        values.emplace(credential.value());
    }
    ASSERT_EQ(values.size(), std::size_t{64});
}

TEST(random_failure_is_not_replaced_with_a_weak_credential) {
    FailingRandom random;
    ASSERT_THROWS(ssg::generate_bearer_credential(random), std::runtime_error);
}

TEST(application_auth_accepts_only_current_bearer_with_exact_capability) {
    auto stale = ssg::generate_bearer_credential();
    auto current = ssg::generate_bearer_credential();
    auto const stale_value = std::string{stale.value()};
    auto const current_value = std::string{current.value()};
    ssg::ApplicationAuthentication authentication{
        std::move(current), ssg::SessionId{"application-session"},
        ssg::ClientId{41}, ssg::ViewId{42}};

    ASSERT_FALSE(authentication.authenticate("wrong").has_value());
    ASSERT_FALSE(authentication.authenticate(stale_value).has_value());
    auto accepted = authentication.authenticate(current_value);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_EQ(accepted->principal.client_id(), ssg::ClientId{41});
    ASSERT_EQ(accepted->principal.origin(), ssg::InvocationOrigin::Websocket);
    ASSERT_EQ(accepted->principal.capabilities().size(), std::size_t{1});
    ASSERT_EQ(accepted->principal.capabilities().front(),
              ssg::CapabilityId{"local_file_drop"});
    ASSERT_EQ(accepted->view_id, ssg::ViewId{42});
}

}  // namespace

int main() {
    RUN(generated_bearers_have_32_random_bytes_in_lowercase_hex);
    RUN(random_failure_is_not_replaced_with_a_weak_credential);
    RUN(application_auth_accepts_only_current_bearer_with_exact_capability);
    return failed == 0 ? 0 : 1;
}
