#include <ssg/ssg.h>

#include "test_helpers.h"

TEST(test_library_name) {
    ASSERT_EQ(ssg::libraryName, "ssg");
}

int main() {
    std::cout << "=== SSG smoke tests ===" << std::endl;
    RUN(test_library_name);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << std::endl;
    return failed > 0 ? 1 : 0;
}
