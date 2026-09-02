#pragma once

#include <filesystem>
#include <string>

namespace ssg {
class ScriptHost;
class EditorSession;
}  // namespace ssg

namespace ssg::app {

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
void evaluateInitScript(ScriptHost& scripts, EditorSession& runtime,
                        std::filesystem::path const& scriptPath,
                        std::string const& script);

}  // namespace ssg::app
