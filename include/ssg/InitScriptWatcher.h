#pragma once

#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ssg {
class ScriptHost;
class Editor;
}  // namespace ssg

namespace ssg {

// Evaluates `script` (already read from `scriptPath`, used only for diagnostic
// messages) through the process-lifetime ScriptHost -- the SINGLE funnel used by
// BOTH startup (loadInitScript) and every later auto-reload
// (InitScriptWatcher::drainAndEvaluate). A broken script prints a one-line
// stderr diagnostic and otherwise leaves the previous evaluation's registrations
// in place.
//
// MUST NEVER be called with an empty/whitespace-only `script`: an empty Lua
// chunk is trivially valid and would look like a silent successful "reload" of
// nothing; callers only invoke this when there is real content to run.
void evaluateInitScript(ScriptHost& scripts, Editor& runtime,
                        std::filesystem::path const& scriptPath,
                        std::string const& script);

[[nodiscard]] std::optional<std::filesystem::path> resolveInitScriptPath();
[[nodiscard]] std::optional<std::string> loadInitScript(
    ScriptHost& scripts, Editor& runtime);

class InitScriptWatcher {
public:
    InitScriptWatcher(std::filesystem::path scriptPath,
                      std::optional<std::string> alreadyApplied);
    ~InitScriptWatcher();

    InitScriptWatcher(InitScriptWatcher const&) = delete;
    InitScriptWatcher& operator=(InitScriptWatcher const&) = delete;

    [[nodiscard]] int wakeDescriptor() const noexcept;
    void drainAndEvaluate(ScriptHost& scripts, Editor& runtime);

private:
    void run();

    std::filesystem::path scriptPath_;
    int wakePipe_[2] = {-1, -1};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::string lastApplied_;
    std::optional<std::string> pendingScript_;
};

}  // namespace ssg
