#include "test_helpers.h"

#include <ssg/SystemClipboardReader.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <pthread.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

class EnvironmentValue {
  public:
    EnvironmentValue(const char* name, const char* value) : name_{name} {
        if (const char* existing = std::getenv(name); existing != nullptr) {
            previous_ = existing;
        }
        if (value != nullptr) {
            (void)::setenv(name, value, 1);
        } else {
            (void)::unsetenv(name);
        }
    }

    ~EnvironmentValue() {
        if (previous_) {
            (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

std::filesystem::path helper(std::string_view name, std::string_view body) {
    static const auto root = testRuntimePath("linux_system_clipboard");
    std::filesystem::create_directories(root);
    const auto path = root / name;
    std::ofstream output{path};
    output << "#!/bin/sh\n" << body << '\n';
    output.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}

TEST(readerReturnsExactOutputAndTriesFallbacks) {
    const auto failure = helper("failure", "exit 1");
    const auto multiline = helper(
        "multiline",
        "[ \"$1\" = \"--no-newline\" ] || exit 2\nprintf 'one\\ntwo'");
    auto read = ssg::SystemClipboardReader{
                    {{failure, {}}, {multiline, {"--no-newline"}}}}
                    .read();
    ASSERT_TRUE(read.accepted());
    ASSERT_EQ(read.text, std::string{"one\ntwo"});

    const auto empty = helper("empty", "exit 0");
    read = ssg::SystemClipboardReader{{{empty, {}}}}.read();
    ASSERT_TRUE(read.accepted());
    ASSERT_TRUE(read.text.empty());
}

TEST(defaultReaderDiscoversHelpersFromTheLinuxEnvironment) {
    const auto executable = helper("wl-paste", "printf discovered");
    EnvironmentValue path{"PATH", executable.parent_path().c_str()};
    EnvironmentValue wayland{"WAYLAND_DISPLAY", "test"};
    EnvironmentValue display{"DISPLAY", nullptr};

    const auto read = ssg::SystemClipboardReader{}.read();
    ASSERT_TRUE(read.accepted());
    ASSERT_EQ(read.text, std::string{"discovered"});
}

TEST(readerRejectsInvalidAndOversizedOutput) {
    const auto invalid = helper("invalid", "printf '\\377'");
    auto read = ssg::SystemClipboardReader{{{invalid, {}}}}.read();
    ASSERT_EQ(read.status, ssg::SystemClipboardReadStatus::InvalidUtf8);

    const auto oversized = helper("oversized", "printf '12345'");
    read = ssg::SystemClipboardReader{
               {{oversized, {}}}, std::chrono::milliseconds{100}, 4}
               .read();
    ASSERT_EQ(read.status, ssg::SystemClipboardReadStatus::TooLarge);
}

TEST(timedOutHelpersAreReaped) {
    const auto marker = testRuntimePath("linux_system_clipboard_pid");
    std::filesystem::remove(marker);
    const auto stalled =
        helper("stalled", "printf $$ > '" + marker.string() + "'\nsleep 10");
    const auto read = ssg::SystemClipboardReader{
                          {{stalled, {}}}, std::chrono::milliseconds{50}, 1024}
                          .read();
    ASSERT_EQ(read.status, ssg::SystemClipboardReadStatus::TimedOut);

    std::ifstream input{marker};
    pid_t pid = 0;
    input >> pid;
    ASSERT_TRUE(pid > 0);
    if (pid > 0) {
        errno = 0;
        ASSERT_EQ(::kill(pid, 0), -1);
        ASSERT_EQ(errno, ESRCH);
    }
}

TEST(helperDoesNotInheritBlockedProcessSignals) {
    sigset_t blocked{};
    sigset_t previous{};
    ASSERT_EQ(::sigemptyset(&blocked), 0);
    ASSERT_EQ(::sigaddset(&blocked, SIGTERM), 0);
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &blocked, &previous), 0);

    const auto selfTerminate =
        helper("self-terminate", "kill -TERM $$\nprintf survived");
    const auto read =
        ssg::SystemClipboardReader{{{selfTerminate, {}}}}.read();

    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous, nullptr), 0);
    ASSERT_EQ(read.status, ssg::SystemClipboardReadStatus::Failed);
}

} // namespace

SSG_TEST_SUITE(test_linux_system_clipboard) {
    RUN(readerReturnsExactOutputAndTriesFallbacks);
    RUN(defaultReaderDiscoversHelpersFromTheLinuxEnvironment);
    RUN(readerRejectsInvalidAndOversizedOutput);
    RUN(timedOutHelpersAreReaped);
    RUN(helperDoesNotInheritBlockedProcessSignals);
    return failed == 0 ? 0 : 1;
}
