#include "test_helpers.h"

#include <ssg/InitScriptWatcher.h>
#include <ssg/PlatformRuntime.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

using namespace std::chrono_literals;

TEST(stableChangedScriptNotifiesRuntime) {
    const auto root = testRuntimePath("init_script_watcher");
    std::filesystem::create_directories(root);
    const auto script = root / "init.lua";
    {
        std::ofstream output{script};
        output << "ssg.command('old')\n";
    }

    ssg::InitScriptWatcher watcher{script, "ssg.command('old')\n"};
    std::this_thread::sleep_for(600ms);
    {
        std::ofstream output{script};
        output << "ssg.command('new')\n";
    }

    ssg::PlatformEventLoop loop{{false, false}};
    const std::array<const ssg::PlatformWake*, 1> wakes{&watcher.wake()};
    const auto readiness = loop.wait(2s, wakes);
    ASSERT_EQ(readiness.wakes.size(), std::size_t{1});
}

} // namespace

SSG_TEST_SUITE(test_init_script_watcher) {
    RUN(stableChangedScriptNotifiesRuntime);
    return failed == 0 ? 0 : 1;
}
