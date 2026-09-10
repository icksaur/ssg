#pragma once
#include <ssg/ViewActionResult.h>
#include <ssg/LuaCommandHost.h>

#include <functional>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class Editor;

// The editor's Lua state, and everything that connects it to the editor.
//
// One ScriptHost lives for the process, so the Lua state outlives any single
// evaluation: a function a script defines is still callable long after the
// script that defined it finished running.  This
// is why it is a class rather than a function -- an `evaluateInitScript(script)`
// free function can only ever build a state, use it and destroy it.
//
// Each evaluate() is a generation: whatever the script registers replaces what
// the previous successful evaluation registered, atomically, so a reload never
// leaves the editor with a half-applied script.  A failed evaluation changes no
// registrations at all; it cannot undo effects the script already caused before
// failing, such as a theme it had already applied (L5).
//
// The Lua state belongs to the thread that constructed the host, and only that
// thread may evaluate or dispatch into it.
class ScriptHost {
public:
    using ViewActionSink =
        std::function<ViewActionResult(ViewAction const&)>;

    // Without a view-action sink, an immediate script request for a view-owned
    // command fails with `view_action_unavailable`.
    explicit ScriptHost(Editor& runtime);
    // The host-owned sink applies a view action; ScriptHost submits its
    // optional semantic transition once.
    ScriptHost(Editor& runtime, ViewActionSink viewActionSink);
    ~ScriptHost();

    ScriptHost(ScriptHost const&) = delete;
    ScriptHost& operator=(ScriptHost const&) = delete;

    // Runs `script` as the next generation.
    //
    // Must never be called with empty or whitespace-only content: an empty Lua
    // chunk is trivially valid, so it would read as a successful reload that
    // retires the previous generation and installs nothing.  Callers check for
    // real content before reading a file's contents this far.
    [[nodiscard]] LuaResult evaluate(std::string_view script);

private:
    LuaResult offerGeneration(std::vector<LuaCommand> const& commands);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
