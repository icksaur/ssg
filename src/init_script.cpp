#include <ssg/init_script.h>

#include <ssg/EditorSession.h>
#include <ssg/ScriptHost.h>

#include <cstdio>

namespace ssg::app {

void evaluateInitScript(ScriptHost& scripts, EditorSession& runtime,
                        std::filesystem::path const& scriptPath,
                        std::string const& script) {
    auto const result = scripts.evaluate(script);
    if (!result.accepted()) {
        std::fprintf(stderr, "ssg: %s: %s\n", scriptPath.string().c_str(),
                     result.message.c_str());
    }
}

}  // namespace ssg::app
