#include <ssg/ScriptHost.h>

#include <ssg/EditorRuntime.h>

#include "test_helpers.h"

#include <filesystem>
#include <memory>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

// Pid-unique so parallel ctest runs cannot remove a directory another test is
// still using.
fs::path uniqueRoot() {
    auto root = fs::current_path() /
                ("script_host_root_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    return root;
}

std::unique_ptr<ssg::EditorRuntime> makeRuntime(fs::path const& root) {
    auto created =
        ssg::EditorRuntime::create({root, root / "scratch", root / "recovery"});
    auto runtime = std::move(created.runtime);
    if (runtime) {
        (void)runtime->attach(
            {ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
            ssg::ViewId{1});
    }
    return runtime;
}

// ---------------------------------------------------------------------------

TEST(theScriptStateOutlivesTheScriptThatCreatedIt) {
    // The reason ScriptHost exists: a value one evaluation defines is still
    // there for the next one.  A per-evaluation state would lose it, and with
    // it any function a script registers for later use.
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(scripts.evaluate("remembered = 41").accepted());
    auto const second =
        scripts.evaluate("if remembered ~= 41 then error('forgotten') end");
    ASSERT_TRUE(second.accepted());
    fs::remove_all(root);
}

TEST(aScriptStillReachesTheEditorThroughTheOrdinaryCommandBoundary) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    auto const bound = scripts.evaluate(
        "ssg.command('keymap.bind', "
        "{sequence = 'Escape KeyF KeyQ', command = 'file.save'})");
    ASSERT_TRUE(bound.accepted());
    fs::remove_all(root);
}

TEST(aBrokenScriptIsReportedAndLeavesTheHostUsable) {
    // A broken config must never wedge the editor: the next evaluation still
    // runs on the same state.
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(scripts.evaluate("survivor = 7").accepted());
    auto const broken = scripts.evaluate("this is not lua");
    ASSERT_TRUE(!broken.accepted());
    ASSERT_TRUE(!broken.message.empty());
    ASSERT_TRUE(
        scripts.evaluate("if survivor ~= 7 then error('lost') end").accepted());
    fs::remove_all(root);
}

TEST(aCommandTheScriptClientMayNotCallIsRefused) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(!scripts.evaluate("ssg.command('text.insert')").accepted());
    fs::remove_all(root);
}

}  // namespace

int main() {
    RUN(theScriptStateOutlivesTheScriptThatCreatedIt);
    RUN(aScriptStillReachesTheEditorThroughTheOrdinaryCommandBoundary);
    RUN(aBrokenScriptIsReportedAndLeavesTheHostUsable);
    RUN(aCommandTheScriptClientMayNotCallIsRefused);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
