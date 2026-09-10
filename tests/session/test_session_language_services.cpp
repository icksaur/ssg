#include "../test_helpers.h"
#include "../grid_test_frame.h"

#include <ssg/Editor.h>
#include <ssg/TextInputCommands.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path uniqueRoot() {
    auto root = testRuntimePath("runtime_language");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "code.txt"} << "abc";
    return root;
}

TEST(syntaxAndLspSectionsAreRuntimeOwnedWithoutTransport) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"code.txt"}}).accepted());
    ASSERT_TRUE(ssg::test::typeText(runtime, "x").accepted());

    auto completion = runtime.dispatch({"completion.open",  {}});
    ASSERT_FALSE(completion.accepted());
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->syntax.revision(), snapshot->documentRevision);
}

} // namespace

SSG_TEST_SUITE(test_session_language_services) {
    RUN(syntaxAndLspSectionsAreRuntimeOwnedWithoutTransport);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
