#pragma once

#include <ssg/EditorSession.h>
#include <ssg/GridPresenter.h>
#include <ssg/ScriptHost.h>
#include <ssg/init_script.h>
#include <ssg/ssg_terminal.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace ssg::app {

struct LaunchArguments {
    bool capabilities = false;
    std::filesystem::path argument;
};

[[nodiscard]] LaunchArguments parseArguments(int argc, char** argv);
void recordStartupMark(char const* phase);

class Application {
public:
    explicit Application(LaunchArguments arguments);

    Application(Application const&) = delete;
    Application& operator=(Application const&) = delete;

    int runEventLoop();

private:
    std::unique_ptr<EditorSession> runtime;
    std::optional<GridPresenter> gridPresenter;
    std::optional<ScriptHost> scripts;
    std::optional<InitScriptWatcher> initScriptWatcher;
    std::optional<TerminalSession> terminal;
    int gitDiffWakeFd = -1;
    bool startsWithAnEditableDocument = false;
    bool openedNamedFile = false;
    bool ready = false;
};

}  // namespace ssg::app
