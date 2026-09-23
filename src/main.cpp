#include <ssg/Terminal.h>
#include <ssg/TerminalClient.h>

#include <ssg/Editor.h>
#include <ssg/GridPresenter.h>
#include <ssg/TreeSitterGrammars.h>
#include <ssg/PlatformRuntime.h>
#include <ssg/RuntimeTiming.h>
#include <ssg/UserConfig.h>
#include <ssg/platform_files.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace ssg {

namespace fs = std::filesystem;

#ifdef SSG_STARTUP_TRACE_ENABLED
void recordStartupMark(char const* phase) {
    static char const* const path = std::getenv("SSG_STARTUP_TRACE");
    if (path == nullptr) return;
    const auto ns = monotonicTime().count();
    // seam-exempt: append-only diagnostic trace, not file content access
    if (std::FILE* file = std::fopen(path, "a"); file != nullptr) {
        std::fprintf(file, "%s %lld\n", phase,
                     static_cast<long long>(ns));
        std::fclose(file);
    }
}
#else
void recordStartupMark(char const*) {}
#endif

namespace {

constexpr int kDragFrameIntervalMs = 16;

struct LaunchTarget {
    fs::path cwd;
    std::optional<std::string> file;
};

LaunchTarget resolveLaunch(const fs::path& argument) {
    if (argument.empty()) return {fs::current_path(), std::nullopt};

    const auto status = ssg::statFile(argument);
    if (status && status->kind == ssg::FileKind::Directory) {
        return {fs::absolute(argument), std::nullopt};
    }
    const auto absolute = fs::absolute(argument);
    const auto parent =
        absolute.has_parent_path() ? absolute.parent_path() : fs::current_path();
    return {parent, absolute.filename().string()};
}

} // namespace

} // namespace ssg

int main(int argc, char** argv) {
    using namespace ssg;

    recordStartupMark("main_entry");
    const fs::path processStartingDirectory = fs::current_path();
    const int firstOperand = argc > 1 && std::string_view{argv[1]} == "--" ? 2 : 1;
    const fs::path argument = argc > firstOperand ? argv[firstOperand] : fs::path{};
    auto target = resolveLaunch(argument);

    fs::path stateBase;
    if (const char* stateOverride = std::getenv("SSG_STATE_DIR"); stateOverride != nullptr && *stateOverride != '\0' && fs::path{stateOverride}.is_absolute()) {
        stateBase = stateOverride;
    } else {
        stateBase = ssg::userStateRoot("ssg");
    }
    (void)ssg::createDirectoriesDurably(stateBase / "archive");
    for (const auto& dir : {stateBase, stateBase / "archive"}) {
        const auto status = ssg::statFile(dir);
        if (status && status->kind == ssg::FileKind::Directory) {
            try {
                ssg::setOwnerOnlyPermissions(dir);
            } catch (const std::exception&) {
            }
        }
    }

    auto recoveryBase =
        fs::temp_directory_path() / ("ssg-" + std::to_string(processId()));
    (void)ssg::createDirectoriesDurably(recoveryBase / "recovery");

    PlatformEventLoop eventLoop;
    ssg::EditorConfig config;
    config.cwd = target.cwd;
    config.recoveryRoot = recoveryBase / "recovery";
    config.archiveRoot = stateBase / "archive";
    config.snapshotPath = sessionSnapshotPath(processStartingDirectory);
    config.deferEnrichment = true;
    config.syntaxParser = ssg::TreeSitterParserFactory::createDefault();
    auto created = ssg::createEditor(config);
    if (!created.accepted()) {
        std::fprintf(stderr, "ssg: %s\n", created.message.c_str());
        return 1;
    }
    auto runtimeOwner = std::move(created.session);
    auto& runtime = *runtimeOwner;
    const PlatformWake* gitDiffWake = runtime.gitDiffWake();
    const PlatformWake* syntaxWake = runtime.syntaxWake();
    recordStartupMark("post_create");

    ssg::GridPresenter presenter;
    recordStartupMark("post_presenter_init");
    try {
        ssg::applyUserConfig(runtime);
    } catch (std::exception const& error) {
        std::fprintf(stderr, "ssg: user config failed: %s\n", error.what());
        return 1;
    }

    bool startsWithAnEditableDocument = !runtime.tabs.viewState().tabs.empty();
    bool openedNamedFile = false;
    if (target.file) {
        auto const openResult = openStartupTarget(runtime, *target.file);
        if (!openResult.accepted) {
            std::fprintf(stderr, "ssg: %s\n", openResult.message.c_str());
        }
        startsWithAnEditableDocument =
            startsWithAnEditableDocument || openResult.accepted;
        openedNamedFile = openResult.accepted;
    }
    if (!startsWithAnEditableDocument) {
        startsWithAnEditableDocument = runtime.dispatch("file.new").accepted();
    }
    recordStartupMark("post_open");

    TerminalSession terminal;
    if (!terminal.active()) {
        std::fprintf(stderr, "ssg: stdin/stdout is not an interactive terminal\n");
        return 1;
    }

    TerminalClient client{runtime, presenter, terminal};
    auto& mode = terminal;
    terminal.write(client.beginProbe());
    bool quit = false;
    std::optional<std::chrono::steady_clock::time_point>
        pendingInputDeadline;

    // Once inside main, exception unwinding does not portably restore the terminal.
    bool firstFrameMarked = false;
    auto lastFrameAt = std::chrono::steady_clock::time_point{};
    try {
        while (!quit) {
            // Motion floods can otherwise render every event and pin a core.
            if (client.pointerInMotion()) {
                const auto sinceFrame = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastFrameAt).count();
                if (sinceFrame < kDragFrameIntervalMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{kDragFrameIntervalMs - sinceFrame});
                }
            }
            lastFrameAt = std::chrono::steady_clock::now();
            const bool wroteFrame = client.present();
            if (wroteFrame && !firstFrameMarked) {
                recordStartupMark("first_content_frame");
                firstFrameMarked = true;
                runtime.primeDeferred();
                // The file provider does not exist until deferred enrichment.
                // A named file should not start obscured by the sidebar.
                if (!openedNamedFile) {
                    if (auto const panelResult = runtime.dispatch("panel.show_files"); !panelResult.accepted()) {
                        std::fprintf(stderr, "ssg: could not open Files sidebar: %s\n", panelResult.message.c_str());
                    }
                }
                // Showing the sidebar focuses it, so restore the requested file.
                if (startsWithAnEditableDocument) {
                    runtime.focusEditor();
                }
                client.requestPresentation();
                continue;
            }

            // One read should contain enough of a motion burst to coalesce it.
            char bytes[4096];
            const auto dragEdge = client.dragEdge();
            std::vector<const PlatformWake*> wakes;
            std::optional<std::size_t> gitDiffWakeIndex;
            std::optional<std::size_t> syntaxWakeIndex;
            if (gitDiffWake != nullptr) {
                gitDiffWakeIndex = wakes.size();
                wakes.push_back(gitDiffWake);
            }
            if (syntaxWake != nullptr) {
                syntaxWakeIndex = wakes.size();
                wakes.push_back(syntaxWake);
            }
            const bool workspaceSearchPending =
                runtime.workspaceSearchPending();
            std::optional<std::chrono::milliseconds> pendingInputWait;
            if (pendingInputDeadline) {
                pendingInputWait = std::max(
                    std::chrono::milliseconds{0},
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        *pendingInputDeadline -
                        std::chrono::steady_clock::now()));
            }
            const auto timeout = selectRuntimeWaitTimeout({
                .ordinaryTimeout = pendingInputWait,
                .edgeScrollActive = dragEdge.has_value(),
                .workspaceSearchPending = workspaceSearchPending,
            });
            const auto wait = eventLoop.wait(timeout, wakes);
            if (wait.termination) {
                terminal.restore();
                terminateProcess(*wait.termination);
            }
            if (wait.resize) {
                client.requestPresentation(true);
            }
            const auto wakeReady = [&](std::optional<std::size_t> index) {
                return index &&
                       std::find(wait.wakes.begin(), wait.wakes.end(), *index) !=
                           wait.wakes.end();
            };

            auto consumeBufferedInput = [&](bool exhausted) {
                while (!quit) {
                    const auto consumed = client.consumeInput(exhausted);
                    if (consumed.status ==
                        InputConsumption::Status::Consumed) {
                        exhausted = false;
                        continue;
                    }
                    if (consumed.status == InputConsumption::Status::Quit) {
                        quit = true;
                    }
                    pendingInputDeadline =
                        consumed.status ==
                                InputConsumption::Status::NeedMoreInput
                            ? std::optional{
                                  std::chrono::steady_clock::now() +
                                  *consumed.deadline}
                            : std::nullopt;
                    break;
                }
            };

            if (wait.input) {
                const auto readBytes = eventLoop.readInput(bytes);
                if (readBytes == 0) break;
                client.appendInput(
                    std::string_view{bytes, readBytes});
                consumeBufferedInput(false);
            } else if (pendingInputDeadline && wait.wakes.empty() &&
                       !wait.resize) {
                consumeBufferedInput(true);
            } else if (dragEdge && wait.wakes.empty() && !wait.resize) {
                client.advanceDragEdge(*dragEdge);
            }

            // Already-ready user input is presented before worker results.
            if (wakeReady(gitDiffWakeIndex)) {
                (void)runtime.pump();
                client.requestPresentation();
            }
            if (wakeReady(syntaxWakeIndex)) {
                (void)runtime.pumpSyntax();
                client.requestPresentation();
            }
            if (workspaceSearchPending) {
                runtime.advanceWorkspaceSearch();
                client.requestPresentation();
            }
        }
    } catch (std::exception const& error) {
        mode.restore();
        std::fprintf(stderr, "ssg: %s\n", error.what());
        return 1;
    } catch (...) {
        mode.restore();
        std::fprintf(stderr, "ssg: terminated by an unknown error\n");
        return 1;
    }

    const auto saved = runtime.saveSession();
    if (!saved.accepted) {
        mode.restore();
        std::fprintf(stderr, "ssg: %s\n", saved.message.c_str());
        return 1;
    }
    return 0;
}
