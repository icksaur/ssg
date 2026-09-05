// add_subdirectory consumer oracle (tests/consumer/main.cpp)
//
// Verifies that SSG's public headers are reachable and the ssg library links
// correctly when SSG is consumed via add_subdirectory from a host project.
// This is a compile+link+run check; all logic is in CMakeLists.txt.

#include <ssg/Settings.h>
#include <ssg/types.h>

int main() {
    // Exercise construction paths for both header groups.
    std::uint64_t  r{42};
    ssg::ByteOffset o{100};
    ssg::TabWidth  w{4};

    // Verify values are reachable (prevents the compiler optimising everything
    // away and gives a concrete return value in case of UB).
    return (r == 42 && o.value() == 100 && w.value() == 4)
               ? 0
               : 1;
}
