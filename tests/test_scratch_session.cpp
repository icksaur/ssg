#include "ssg/ScratchJournal.h"
#include "ssg/ScratchSession.h"
#include "test_helpers.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("ssg-scratch-session-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

ssg::JournalDocument document(std::string path, std::string contents) {
    return {ssg::JournalDocumentKey::saved(path),
            ssg::DocumentMode::Edit,
            true,
            std::move(contents)};
}

void makeRestorable(ssg::ScratchSession& session, std::string contents) {
    ssg::ScratchJournal{session.journalPath()}.appendDocument(
        document("file.txt", std::move(contents)));
}

class ChildProcess {
public:
    ChildProcess(const std::filesystem::path& executable,
                 const std::filesystem::path& root,
                 const std::filesystem::path& workspace,
                 const std::filesystem::path& ready) {
#ifdef _WIN32
        std::wstring command = L"\"" + executable.wstring() + L"\" --hold \"" +
                               root.wstring() + L"\" \"" + workspace.wstring() +
                               L"\" \"" + ready.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &startup, &process)) {
            throw std::runtime_error("CreateProcessW failed");
        }
        CloseHandle(process.hThread);
        handle_ = process.hProcess;
#else
        pid_ = fork();
        if (pid_ == 0) {
            execl(executable.c_str(), executable.c_str(), "--hold",
                  root.c_str(), workspace.c_str(), ready.c_str(), nullptr);
            _exit(127);
        }
        if (pid_ < 0) {
            throw std::runtime_error("fork failed");
        }
#endif
    }

    ~ChildProcess() { terminate(); }

    void terminate() noexcept {
#ifdef _WIN32
        if (handle_ != nullptr) {
            TerminateProcess(handle_, 9);
            WaitForSingleObject(handle_, INFINITE);
            CloseHandle(handle_);
            handle_ = nullptr;
        }
#else
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

std::string waitForReady(const std::filesystem::path& path) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        std::ifstream input(path);
        std::string value;
        if (input >> value) {
            return value;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("child did not become ready");
}

TEST(concurrentProcessIsHiddenUntilCrashReleasesLock) {
    TemporaryDirectory temporary;
    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace").lexically_normal();
    const auto ready = temporary.path() / "ready";
    ChildProcess child{std::filesystem::absolute("test_scratch_session"),
                       temporary.path(), workspace, ready};
    const auto childId = waitForReady(ready);

    auto current = ssg::ScratchSession::create(temporary.path(), workspace);
    ASSERT_FALSE(current.claimNewestRestorable().has_value());

    child.terminate();
    auto crashed = current.claimNewestRestorable();
    ASSERT_TRUE(crashed.has_value());
    ASSERT_EQ(crashed->id().value(), childId);
    const auto replayedCrash = crashed->replay();
    ASSERT_EQ(replayedCrash.recovery.documents.front().utf8Content,
              std::string{"child"});
}

TEST(newestUnlockedRemnantIsClaimedOnce) {
    TemporaryDirectory temporary;
    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace").lexically_normal();

    std::string oldId;
    {
        auto old = ssg::ScratchSession::create(temporary.path(), workspace);
        oldId = old.id().value();
        makeRestorable(old, "old");
    }
    std::this_thread::sleep_for(2ms);
    std::string newId;
    {
        auto recent = ssg::ScratchSession::create(temporary.path(), workspace);
        newId = recent.id().value();
        makeRestorable(recent, "new");
    }

    auto selectorA = ssg::ScratchSession::create(temporary.path(), workspace);
    auto selectorB = ssg::ScratchSession::create(temporary.path(), workspace);
    auto newest = selectorA.claimNewestRestorable();
    ASSERT_TRUE(newest.has_value());
    ASSERT_EQ(newest->id().value(), newId);
    const auto replayedNewest = newest->replay();
    ASSERT_EQ(replayedNewest.recovery.documents.front().utf8Content,
              std::string{"new"});
    auto older = selectorB.claimNewestRestorable();
    ASSERT_TRUE(older.has_value());
    ASSERT_EQ(older->id().value(), oldId);
    older.reset();

    newest->markRestored();
    newest.reset();
    auto remaining = selectorA.claimNewestRestorable();
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->id().value(), oldId);
}

TEST(staleEmptyAndOtherWorkspaceSessionsAreNotRestored) {
    TemporaryDirectory temporary;
    const auto workspaceA =
        std::filesystem::absolute(temporary.path() / "a").lexically_normal();
    const auto workspaceB =
        std::filesystem::absolute(temporary.path() / "b").lexically_normal();
    {
        auto empty = ssg::ScratchSession::create(temporary.path(), workspaceA);
    }
    {
        auto other = ssg::ScratchSession::create(temporary.path(), workspaceB);
        makeRestorable(other, "other");
    }

    auto current = ssg::ScratchSession::create(temporary.path(), workspaceA);
    makeRestorable(current, "live");
    ASSERT_FALSE(current.claimNewestRestorable().has_value());
    ASSERT_NE(ssg::scratchWorkspaceKey(workspaceA),
              ssg::scratchWorkspaceKey(workspaceB));
}

int childMain(const std::filesystem::path& root,
               const std::filesystem::path& workspace,
               const std::filesystem::path& ready) {
    auto session = ssg::ScratchSession::create(root, workspace);
    makeRestorable(session, "child");
    {
        std::ofstream output(ready);
        output << session.id().value() << '\n';
    }
    for (;;) {
        std::this_thread::sleep_for(1s);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view{argv[1]} == "--hold") {
        return childMain(argv[2], argv[3], argv[4]);
    }

    std::cout << "=== Scratch session locking ===\n";
    RUN(concurrentProcessIsHiddenUntilCrashReleasesLock);
    RUN(newestUnlockedRemnantIsClaimedOnce);
    RUN(staleEmptyAndOtherWorkspaceSessionsAreNotRestored);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
