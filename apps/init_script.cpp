#include "init_script.h"

#include <ssg/EditorRuntime.h>
#include <ssg/ScriptHost.h>

#include <cstdio>

namespace ssg::app {

void evaluateInitScript(ScriptHost& scripts, EditorRuntime& runtime,
                        std::filesystem::path const& scriptPath,
                        std::string const& script) {
    auto const result = scripts.evaluate(script);
    if (!result.accepted()) {
        std::fprintf(stderr, "ssg: %s: %s\n", scriptPath.string().c_str(),
                     result.message.c_str());
    }
    // Push the currently PUBLISHED composition (already reflects rollback: a
    // rejected reload keeps the prior value) into the runtime on BOTH the
    // startup and auto-reload paths, since this is their shared funnel. An
    // unchanged composition is a no-op inside setComposedUi.
    runtime.setComposedUi(scripts.composedUi());
}

}  // namespace ssg::app
