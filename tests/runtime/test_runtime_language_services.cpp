#include "../test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/TextInputCommands.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path uniqueRoot() {
    auto root = std::filesystem::current_path() / "runtime_language";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "code.txt"} << "abc";
    return root;
}

TEST(syntaxAndLspSectionsAreRuntimeOwnedWithoutTransport) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"code.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"x"}}).accepted());

    auto completion = runtime.dispatch(ssg::ClientId{1}, {"completion.open", runtime.revision(), {}});
    ASSERT_FALSE(completion.accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().syntax.revision(), snapshot->sections().document.revision);
    ASSERT_FALSE(snapshot->sections().lspFeatures.status.empty());
}

} // namespace

int main() {
    RUN(syntaxAndLspSectionsAreRuntimeOwnedWithoutTransport);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
