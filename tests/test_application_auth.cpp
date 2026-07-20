#include "test_helpers.h"

#include <ssg/ApplicationAuthentication.h>

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

TEST(generatedBearersHave32RandomBytesInLowercaseHex) {
    std::set<std::string> values;
    for (int count = 0; count < 64; ++count) {
        auto credential = ssg::generateBearerCredential();
        ASSERT_EQ(credential.value().size(), std::size_t{64});
        for (char value : credential.value()) {
            ASSERT_TRUE((value >= '0' && value <= '9') ||
                        (value >= 'a' && value <= 'f'));
        }
        values.emplace(credential.value());
    }
    ASSERT_EQ(values.size(), std::size_t{64});
}

TEST(randomFailureIsNotReplacedWithAWeakCredential) {
    FailingRandom random;
    ASSERT_THROWS(ssg::generateBearerCredential(random), std::runtime_error);
}

TEST(applicationAuthAcceptsOnlyCurrentBearerWithExactCapability) {
    auto stale = ssg::generateBearerCredential();
    auto current = ssg::generateBearerCredential();
    auto const staleValue = std::string{stale.value()};
    auto const currentValue = std::string{current.value()};
    ssg::ApplicationAuthentication authentication{
        std::move(current), ssg::SessionId{"application-session"},
        ssg::ClientId{41}, ssg::ViewId{42}};

    ASSERT_FALSE(authentication.authenticate("wrong").has_value());
    ASSERT_FALSE(authentication.authenticate(staleValue).has_value());
    auto accepted = authentication.authenticate(currentValue);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_EQ(accepted->principal.clientId(), ssg::ClientId{41});
    ASSERT_EQ(accepted->principal.origin(), ssg::InvocationOrigin::Websocket);
    ASSERT_EQ(accepted->principal.capabilities().size(), std::size_t{1});
    ASSERT_EQ(accepted->principal.capabilities().front(),
              ssg::CapabilityId{"local_file_drop"});
    ASSERT_EQ(accepted->viewId, ssg::ViewId{42});
}

}  // namespace

int main() {
    RUN(generatedBearersHave32RandomBytesInLowercaseHex);
    RUN(randomFailureIsNotReplacedWithAWeakCredential);
    RUN(applicationAuthAcceptsOnlyCurrentBearerWithExactCapability);
    return failed == 0 ? 0 : 1;
}
