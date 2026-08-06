#pragma once

#include <filesystem>
#include <string>

namespace ssg {
class ScriptHost;
class EditorRuntime;
}  // namespace ssg

namespace ssg::app {

// Evaluates `script` (already read from `scriptPath`, used only for diagnostic
// messages) through the process-lifetime ScriptHost -- the SINGLE funnel used by
// BOTH startup (loadInitScript) and every later auto-reload
// (InitScriptWatcher::drainAndEvaluate). A broken script prints a one-line
// stderr diagnostic and otherwise leaves the previous evaluation's registrations
// in place. After evaluating, the ScriptHost's currently PUBLISHED chrome
// composition (which already reflects rollback -- a rejected reload keeps the
// prior value) is pushed into the runtime, so a composed header/footer takes
// effect and a dropped `ssg.chrome` reverts to built-in on the same reload the
// commands do. Extracted here (rather than left in ssg_main.cpp's main TU) so a
// host-level test can pin that the composition-push happens on this shared path.
//
// MUST NEVER be called with an empty/whitespace-only `script`: an empty Lua
// chunk is trivially valid and would look like a silent successful "reload" of
// nothing; callers only invoke this when there is real content to run.
void evaluateInitScript(ScriptHost& scripts, EditorRuntime& runtime,
                        std::filesystem::path const& scriptPath,
                        std::string const& script);

}  // namespace ssg::app
