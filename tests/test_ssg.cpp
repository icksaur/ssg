#include <ssg/ssg.h>

#include "test_helpers.h"

TEST(testLibraryName) {
    ASSERT_EQ(ssg::libraryName, "ssg");
}

int main() {
    std::cout << "=== SSG smoke tests ===" << std::endl;
    RUN(testLibraryName);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << std::endl;
    return failed > 0 ? 1 : 0;
}
