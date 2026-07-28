#include <ssg/ScriptHost.h>

#include <ssg/CommandCatalog.h>
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


TEST(aCommandAScriptRegistersIsAnOrdinaryCatalogCommand) {
    // The point of the whole exercise: a script's function is dispatched
    // through the same path as every built-in, with no Lua-shaped special case
    // anywhere downstream.
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(scripts
                    .evaluate("calls = 0\n"
                              "ssg.register_command('user.count', function()\n"
                              "  calls = calls + 1\n"
                              "end)")
                    .accepted());

    auto const* entry = runtime->commandCatalog()->find("user.count");
    ASSERT_TRUE(entry != nullptr);
    if (entry) ASSERT_EQ(entry->owner, std::string{"lua"});

    auto const dispatched = runtime->dispatch(
        ssg::ClientId{1}, {"user.count", runtime->revision(), {}});
    ASSERT_TRUE(dispatched.accepted());
    ASSERT_TRUE(scripts.evaluate("if calls ~= 1 then error('not called') end")
                    .accepted());
    fs::remove_all(root);
}

TEST(reloadingRetiresThePreviousGenerationsCommands) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(
        scripts.evaluate("ssg.register_command('user.first', function() end)")
            .accepted());
    auto const* first = runtime->commandCatalog()->find("user.first");
    ASSERT_TRUE(first != nullptr);

    ASSERT_TRUE(
        scripts.evaluate("ssg.register_command('user.second', function() end)")
            .accepted());
    ASSERT_TRUE(runtime->commandCatalog()->find("user.first") == nullptr);
    ASSERT_TRUE(runtime->commandCatalog()->find("user.second") != nullptr);
    fs::remove_all(root);
}

TEST(reloadingAnUnchangedScriptSucceeds) {
    // The reload that a persistent state makes easy to get wrong: the same ids
    // are registered again, and must not collide with their own predecessors.
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    std::string const script =
        "ssg.register_command('user.same', function() end)";
    for (int reload = 0; reload < 3; ++reload) {
        auto const result = scripts.evaluate(script);
        if (!result.accepted()) std::cout << "  msg: " << result.message << "\n";
        ASSERT_TRUE(result.accepted());
        ASSERT_TRUE(runtime->commandCatalog()->find("user.same") != nullptr);
    }
    fs::remove_all(root);
}

TEST(aFailedReloadKeepsThePreviousGenerationDispatchable) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(
        scripts.evaluate("ssg.register_command('user.kept', function() end)")
            .accepted());
    ASSERT_TRUE(!scripts.evaluate("this is not lua").accepted());

    ASSERT_TRUE(runtime->commandCatalog()->find("user.kept") != nullptr);
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"user.kept", runtime->revision(), {}})
                    .accepted());
    fs::remove_all(root);
}

TEST(aRetiredScriptCommandIsNoLongerDispatchable) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(
        scripts.evaluate("ssg.register_command('user.gone', function() end)")
            .accepted());
    ASSERT_TRUE(scripts.evaluate("noop = true").accepted());
    ASSERT_TRUE(!runtime
                     ->dispatch(ssg::ClientId{1},
                                {"user.gone", runtime->revision(), {}})
                     .accepted());
    fs::remove_all(root);
}

TEST(aScriptCommandCollidingWithABuiltInIsRefusedWithoutLosingTheEditor) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    ssg::ScriptHost scripts{*runtime};

    ASSERT_TRUE(
        !scripts.evaluate("ssg.register_command('file.save', function() end)")
             .accepted());
    // The built-in is untouched: the catalog refused the batch before applying
    // any of it.
    auto const* builtIn = runtime->commandCatalog()->find("file.save");
    ASSERT_TRUE(builtIn != nullptr);
    if (builtIn) ASSERT_TRUE(builtIn->owner != std::string{"lua"});
    fs::remove_all(root);
}


}  // namespace

int main() {
    RUN(theScriptStateOutlivesTheScriptThatCreatedIt);
    RUN(aScriptStillReachesTheEditorThroughTheOrdinaryCommandBoundary);
    RUN(aBrokenScriptIsReportedAndLeavesTheHostUsable);
    RUN(aCommandTheScriptClientMayNotCallIsRefused);
    RUN(aCommandAScriptRegistersIsAnOrdinaryCatalogCommand);
    RUN(reloadingRetiresThePreviousGenerationsCommands);
    RUN(reloadingAnUnchangedScriptSucceeds);
    RUN(aFailedReloadKeepsThePreviousGenerationDispatchable);
    RUN(aRetiredScriptCommandIsNoLongerDispatchable);
    RUN(aScriptCommandCollidingWithABuiltInIsRefusedWithoutLosingTheEditor);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
