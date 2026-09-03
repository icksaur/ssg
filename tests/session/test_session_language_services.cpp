#include "../test_helpers.h"

#include <ssg/EditorSession.h>
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
    auto created = ssg::EditorSession::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open", runtime.revision(), std::string{"code.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"text.insert", runtime.revision(), ssg::TextInputArguments{"x"}}).accepted());

    auto completion = runtime.dispatch({"completion.open", runtime.revision(), {}});
    ASSERT_FALSE(completion.accepted());
    auto snapshot = runtime.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().syntax.revision(), snapshot->sections().document.revision);
    ASSERT_FALSE(snapshot->sections().lspFeatures.status.empty());
}

} // namespace

SSG_TEST_SUITE(test_session_language_services) {
    RUN(syntaxAndLspSectionsAreRuntimeOwnedWithoutTransport);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
