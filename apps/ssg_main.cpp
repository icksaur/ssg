// The `ssg` terminal editor entry point.  This file owns terminal I/O only:
// raw mode, size queries, byte reads, frame writes, and clean restoration.  All
// editor, workspace, and layout behavior is the ssg library's; the app attaches
// an in-process client to an EditorRuntime, renders the library's snapshot, and
// forwards input.
//
// Milestone 1 scope: launch over a path argument, draw the shell grid, and quit
// on the `ESC Q` chord.  Input translation through the library keymap and
// editing arrive in later milestones; quitting is an application lifecycle
// concern owned here.

#include "pointer_routing.h"
#include "ssg_terminal.h"

#include <ssg/EditorRuntime.h>
#include <ssg/HitTester.h>
#include <ssg/FindReplace.h>
#include <ssg/InitScriptCatalog.h>
#include <ssg/Keymap.h>
#include <ssg/LuaCommandHost.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Picker.h>
#include <ssg/platform_files.h>
#include <ssg/session_snapshot.h>
#include <ssg/TextInputCommands.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <any>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

// M10-1 startup instrumentation.  Records a CLOCK_MONOTONIC timestamp per cold-
// start phase to the file named by the SSG_STARTUP_TRACE env var, so the startup
// benchmark can attribute exec->first-frame time to phases.  The call sites are
// preprocessor-gated (STARTUP_MARK), so unless SSG_STARTUP_TRACE_ENABLED is
// defined (the shipped `ssg` binary) there is no function, symbol, or call at
// any optimization level.
#ifdef SSG_STARTUP_TRACE_ENABLED
void startup_mark_impl(char const* phase) {
    static char const* const path = std::getenv("SSG_STARTUP_TRACE");
    if (path == nullptr) return;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long const ns =
        static_cast<long long>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
    if (std::FILE* file = std::fopen(path, "a"); file != nullptr) {
        std::fprintf(file, "%s %lld\n", phase, ns);
        std::fclose(file);
    }
}
#define STARTUP_MARK(phase) startup_mark_impl(phase)
#else
#define STARTUP_MARK(phase) ((void)0)
#endif

void writeAll(std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto written =
            ::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
}

// Puts the terminal in raw mode on the alternate screen and restores the
// original mode, cursor, and primary screen on destruction (RAII).  restore()
// performs the same teardown eagerly and idempotently so a terminating-signal
// path (M9-X) can restore the terminal before re-raising, without the
// destructor undoing or repeating it.
class TerminalMode {
public:
    TerminalMode() {
        if (tcgetattr(STDIN_FILENO, &original_) != 0) return;
        termios raw = original_;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
        active_ = true;
        writeAll(ssg::app::terminal_setup_sequence());
    }

    ~TerminalMode() { restore(); }

    // Restore the terminal to its pre-launch state.  Safe to call more than once
    // (the destructor calls it again); only the first call does the work.
    void restore() noexcept {
        if (!active_) return;
        active_ = false;
        writeAll(ssg::app::terminal_restore_sequence());
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    }

    TerminalMode(TerminalMode const&) = delete;
    TerminalMode& operator=(TerminalMode const&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    termios original_{};
    bool active_ = false;
};

ssg::ViewportDimensions terminalSize() {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 &&
        size.ws_row > 0) {
        return {size.ws_col, size.ws_row};
    }
    return {80, 24};
}

// Which descriptors became ready within `timeout_ms` (negative blocks): the
// keyboard, the signal self-pipe, or both.  Every wait in the loop watches the
// signal pipe so a resize/terminate signal is observed promptly even mid-drag or
// mid-Escape (M9-W); the callers drain and act on `signal`.
struct FdReadiness {
    bool input = false;      // STDIN has bytes.
    bool signal = false;     // The signal self-pipe has pending tags.
    bool gitDiff = false;    // The runtime git-diff wake descriptor is readable.
    bool initScript = false; // The init-script reload wake descriptor is readable.
};

FdReadiness waitReadiness(int timeoutMs, int signalFd, int gitDiffFd,
                          int initScriptFd = -1) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    FD_SET(signalFd, &set);
    int maxFd = std::max(STDIN_FILENO, signalFd);
    if (gitDiffFd >= 0) {
        FD_SET(gitDiffFd, &set);
        maxFd = std::max(maxFd, gitDiffFd);
    }
    if (initScriptFd >= 0) {
        FD_SET(initScriptFd, &set);
        maxFd = std::max(maxFd, initScriptFd);
    }
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int const ready =
        ::select(maxFd + 1, &set, nullptr, nullptr,
                 timeoutMs < 0 ? nullptr : &timeout);
    if (ready <= 0) return {};
    return {
        FD_ISSET(STDIN_FILENO, &set) != 0, FD_ISSET(signalFd, &set) != 0,
        gitDiffFd >= 0 ? FD_ISSET(gitDiffFd, &set) != 0 : false,
        initScriptFd >= 0 ? FD_ISSET(initScriptFd, &set) != 0 : false};
}

constexpr int kEscapeTimeoutMs = 30;

// While a drag is held at the editor edge, wake this often to auto-scroll one
// line and re-extend the selection, even with no new pointer event (M8-S2).
constexpr int kEdgeScrollIntervalMs = 40;

// M9-W signal-event wakeup: the write end of a non-blocking self-pipe.  Signal
// handlers are the only writers and touch nothing else, so a raw fd in a
// sig_atomic-safe int is the whole async-signal-safe surface.  -1 until the pipe
// is created; a handler firing before then is a no-op.
volatile std::sig_atomic_t gSignalPipeWrite = -1;

// The sole action taken in async-signal context: write this signal's number as
// one tag byte to the self-pipe so the event loop wakes and handles it in normal
// context.  A full pipe (EAGAIN) already means "wake pending", so the result is
// ignored; write() is async-signal-safe.
extern "C" void signalTagHandler(int signo) {
    int const fd = gSignalPipeWrite;
    if (fd < 0) return;
    unsigned char const tag = static_cast<unsigned char>(signo);
    ssize_t const written = ::write(fd, &tag, 1);
    (void)written;
}

// Install the tag-writing handler for a signal, restarting interrupted syscalls
// (the self-pipe select() is the reliable wake, independent of SA_RESTART).
void installSignalTagHandler(int signo) {
    struct sigaction action{};
    action.sa_handler = signalTagHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(signo, &action, nullptr);
}

// Delete one UTF-8 code point from the end of a client-local query string.
void popCodePoint(std::string& text) {
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
        text.pop_back();
    }
    if (!text.empty()) text.pop_back();
}

// The reserved client id for the one-shot init-script Lua host (distinct
// from the interactive client's ClientId{1}) -- attached ONLY while
// init.lua is being evaluated, then detached, so this synthetic client
// leaves no lasting per-client state (follow-edits tracking, viewport,
// etc.) once startup configuration has run.
ssg::ClientId const kInitScriptClientId{2};

// The capabilities the init-script Lua host is granted -- ONE named,
// explicit list shared by BOTH LuaCommandHost's own catalog check (via
// LuaCommandHostOptions.capabilities) AND the attached principal's real
// dispatch-time capability check (InvocationPrincipal::hasCapability),
// so there is exactly one place to update when a future capability-gated
// init-script command is added -- keeping these as two independently
// hand-maintained lists would let one drift out of sync with the other
// (passing one gate but silently denied, or vice versa, at the other).
// theme.define requires none today, so this is empty; see doc/spec-
// config.md's Risks: never a wildcard/all-capabilities grant.
std::vector<ssg::CapabilityId> initScriptCapabilities() { return {}; }

// The catalog of commands init.lua may call via ssg.command(id, args). A
// future capability-gated init-script command must be added BOTH here
// (with its required capabilities) and to initScriptCapabilities() above,
// or InvocationPrincipal::hasCapability denies it by default, same as any
// other Lua caller. Built from ssg::kInitScriptCommands (see
// InitScriptCatalog.h), the single source of truth doc/config.md's
// coverage test checks against.
std::vector<ssg::LuaCommand> initScriptCommandCatalog() {
    std::vector<ssg::LuaCommand> result;
    result.reserve(ssg::kInitScriptCommands.size());
    for (auto const& descriptor : ssg::kInitScriptCommands) {
        result.push_back({std::string{descriptor.id}, {}});
    }
    return result;
}

// Extracts a string field from a Lua flat-map argument table, or an empty
// string when absent -- keymap.bind/unbind treat an absent "context" as
// "*" (see KeymapBindArguments/KeymapUnbindArguments in Keymap.h), so the
// caller distinguishes "absent" from "empty" only for required fields.
std::string luaArgument(
    std::unordered_map<std::string, std::string> const& arguments,
    std::string_view key) {
    const auto it = arguments.find(std::string{key});
    return it == arguments.end() ? std::string{} : it->second;
}

// Translates one ssg.command(id, args) call from init.lua into the matching
// ClientCommand payload and dispatches it through the SAME command
// boundary the palette and every other caller uses. A later init-script
// command adds one more `if (invocation.commandId == ...)` branch here,
// not a new dispatch mechanism.
ssg::CommandHandlerResult dispatchInitScriptCommand(
    ssg::EditorRuntime& runtime, ssg::LuaInvocation const& invocation) {
    if (invocation.commandId == "theme.define") {
        // theme.define always requires its color-table argument; a bare
        // ssg.command("theme.define") with no table must fail loudly (a
        // caller mistake), not silently apply an empty no-op table.
        if (!invocation.arguments) {
            return ssg::CommandHandlerResult::failure(
                "theme.define requires a color-table argument");
        }
        ssg::ThemeDefineArguments arguments{*invocation.arguments};
        auto result = runtime.dispatch(
            kInitScriptClientId,
            {"theme.define", runtime.revision(), arguments});
        return result.accepted()
                   ? ssg::CommandHandlerResult::success()
                   : ssg::CommandHandlerResult::failure(result.message);
    }
    if (invocation.commandId == "keymap.bind") {
        if (!invocation.arguments) {
            return ssg::CommandHandlerResult::failure(
                "keymap.bind requires a sequence/command argument table");
        }
        ssg::KeymapBindArguments arguments{
            luaArgument(*invocation.arguments, "sequence"),
            luaArgument(*invocation.arguments, "command"),
            luaArgument(*invocation.arguments, "context")};
        auto result = runtime.dispatch(
            kInitScriptClientId,
            {"keymap.bind", runtime.revision(), arguments});
        return result.accepted()
                   ? ssg::CommandHandlerResult::success()
                   : ssg::CommandHandlerResult::failure(result.message);
    }
    if (invocation.commandId == "keymap.unbind") {
        if (!invocation.arguments) {
            return ssg::CommandHandlerResult::failure(
                "keymap.unbind requires a sequence argument table");
        }
        ssg::KeymapUnbindArguments arguments{
            luaArgument(*invocation.arguments, "sequence"),
            luaArgument(*invocation.arguments, "context")};
        auto result = runtime.dispatch(
            kInitScriptClientId,
            {"keymap.unbind", runtime.revision(), arguments});
        return result.accepted()
                   ? ssg::CommandHandlerResult::success()
                   : ssg::CommandHandlerResult::failure(result.message);
    }
    return ssg::CommandHandlerResult::failure(
        "unknown init-script command: " + std::string{invocation.commandId});
}

// True for empty or whitespace-only content -- treated as "nothing to
// run", same as an absent file, so a zero-byte read observed mid-truncate
// (or mid-save, before an editor writes real bytes) can never be queued
// or evaluated as a false "successful reload of nothing" (evaluateInitScript
// below must never be called with this).
bool isBlank(std::string const& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char byte) {
        return std::isspace(byte) != 0;
    });
}

// Evaluates `script` (already read from `scriptPath`, used only for
// diagnostic messages) through a FRESH sandboxed LuaCommandHost attached as
// the synthetic Lua-origin client, then detaches it -- the SAME path used
// at startup and on every later auto-reload (never a persistent Lua state
// shared across evaluations). A broken script prints a one-line stderr
// diagnostic and otherwise changes nothing -- MUST NEVER be called with an
// empty/whitespace-only `script`, since an empty Lua chunk is trivially
// valid and would look like a silent successful "reload" of nothing; the
// callers below only invoke this when there is real content to run (see
// isBlank above, applied at every read site before this is called).
void evaluateInitScript(ssg::EditorRuntime& runtime,
                        std::filesystem::path const& scriptPath,
                        std::string const& script) {
    // keymap.bind/unbind's "reset-then-reapply" model: init.lua's current
    // content is the WHOLE keymap customization on every evaluation
    // (startup and every later auto-reload), never additive -- so a line
    // removed from init.lua reverts that binding on the next reload, same
    // as theme.define's replace-on-full-reload behavior.
    runtime.resetKeymapToDefault();
    if (!runtime
             .attach({kInitScriptClientId, ssg::InvocationOrigin::Lua,
                      initScriptCapabilities()},
                     ssg::ViewId{0})
             .accepted()) {
        std::fprintf(stderr, "ssg: could not start init-script host\n");
        return;
    }

    ssg::LuaCommandHostOptions options;
    options.pluginId = kInitScriptClientId;
    options.capabilities = initScriptCapabilities();
    options.commands = initScriptCommandCatalog();
    ssg::LuaCommandHost host{
        std::move(options), [&runtime](ssg::LuaInvocation const& invocation) {
            return dispatchInitScriptCommand(runtime, invocation);
        }};
    auto const result = host.evaluate(script);
    if (!result.accepted()) {
        std::fprintf(stderr, "ssg: %s: %s\n", scriptPath.string().c_str(),
                    result.message.c_str());
    }
    (void)runtime.detach(kInitScriptClientId);
}

// Resolves `init.lua`'s path for this OS. Returns nullopt (after printing a
// diagnostic) only if the config root itself is unresolvable (e.g. HOME
// unset) -- an unusual environment problem, distinct from the file simply
// not existing there.
std::optional<std::filesystem::path> resolveInitScriptPath() {
    try {
        return ssg::userConfigRoot("ssg") / "init.lua";
    } catch (std::exception const& error) {
        std::fprintf(stderr, "ssg: could not resolve config directory: %s\n",
                    error.what());
        return std::nullopt;
    }
}

// Reads `scriptPath`'s full content if it currently exists AND is not
// blank. Returns nullopt silently (NO diagnostic) if the file simply does
// not exist, or exists but is empty/whitespace-only -- both are "nothing
// to run", the normal no-config case, and also the steady state after a
// mid-run delete (see InitScriptWatcher below). Prints a diagnostic and
// returns nullopt for a genuine I/O error (a stat or read failure other
// than "does not exist").
std::optional<std::string> readInitScriptIfPresent(
    std::filesystem::path const& scriptPath) {
    std::error_code existsError;
    bool const present = std::filesystem::exists(scriptPath, existsError);
    if (existsError) {
        std::fprintf(stderr, "ssg: could not check %s: %s\n",
                    scriptPath.string().c_str(), existsError.message().c_str());
        return std::nullopt;
    }
    if (!present) return std::nullopt;

    std::ifstream input{scriptPath, std::ios::binary};
    if (!input) {
        std::fprintf(stderr, "ssg: could not read %s\n",
                    scriptPath.string().c_str());
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto text = buffer.str();
    if (isBlank(text)) return std::nullopt;
    return text;
}

// Loads and evaluates `init.lua` exactly once at startup, via
// resolveInitScriptPath + readInitScriptIfPresent + evaluateInitScript.
// Absent file, or any of the diagnostic-then-return-nullopt cases above:
// silently continue starting with defaults (see doc/spec-config.md's
// Invariants -- a broken config script must never block opening the
// editor). Returns the content actually applied (or nullopt), so the
// caller can seed InitScriptWatcher's "last applied" baseline and avoid
// redundantly re-evaluating the SAME unchanged content on its first poll.
std::optional<std::string> loadInitScript(ssg::EditorRuntime& runtime) {
    auto const scriptPath = resolveInitScriptPath();
    if (!scriptPath) return std::nullopt;
    auto script = readInitScriptIfPresent(*scriptPath);
    if (!script) return std::nullopt;
    evaluateInitScript(runtime, *scriptPath, *script);
    return script;
}

// How often the background thread re-reads init.lua's content to check for
// a change (doc/spec-config.md's auto-reload design). Content, not mtime/
// size, is compared -- a same-size rewrite within one filesystem timestamp
// tick would otherwise evade a stat-only check, and the content must be
// read anyway to queue it for evaluation.
constexpr std::chrono::milliseconds kInitScriptPollInterval{500};

// Watches init.lua for changes on a background thread and wakes the main
// loop to re-evaluate it -- mirrors EditorRuntime's OWN git-diff-worker
// shape (background poll thread + wake self-pipe + main-thread-only apply,
// src/EditorRuntime.cpp's startGitDiffWorker/drainGitDiffScans) as a
// SEPARATE, dedicated mechanism (not sharing that worker's thread or
// pipe): init.lua lives outside the workspace tree, where the library's
// FilesystemWatcher (a workspace-rooted native recursive watcher) does not
// apply. All Lua evaluation and runtime.dispatch() calls happen on the
// MAIN thread inside drainAndEvaluate(), never on the background thread,
// which only ever reads file bytes and compares strings.
class InitScriptWatcher {
public:
    // `alreadyApplied` is whatever content `loadInitScript()` already
    // evaluated at startup (or nullopt if none) -- seeding the "last
    // applied" baseline with it so the watcher's FIRST poll never
    // redundantly re-queues the SAME unchanged startup content as if it
    // were a new edit.
    InitScriptWatcher(std::filesystem::path scriptPath,
                      std::optional<std::string> alreadyApplied)
        : scriptPath_{std::move(scriptPath)},
          lastApplied_{std::move(alreadyApplied).value_or(std::string{})} {
        if (::pipe(wakePipe_) != 0) {
            wakePipe_[0] = wakePipe_[1] = -1;
            return;
        }
        for (int fd : wakePipe_) {
            int const flags = ::fcntl(fd, F_GETFL, 0);
            if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                (void)::close(wakePipe_[0]);
                (void)::close(wakePipe_[1]);
                wakePipe_[0] = wakePipe_[1] = -1;
                return;
            }
        }
        thread_ = std::thread([this] { run(); });
    }

    ~InitScriptWatcher() {
        if (thread_.joinable()) {
            {
                std::lock_guard lock{mutex_};
                stop_ = true;
            }
            // Notify rather than rely on the poll interval elapsing, so
            // shutdown is prompt instead of blocking for up to
            // kInitScriptPollInterval.
            wake_.notify_all();
            thread_.join();
        }
        if (wakePipe_[0] != -1) (void)::close(wakePipe_[0]);
        if (wakePipe_[1] != -1) (void)::close(wakePipe_[1]);
    }

    InitScriptWatcher(InitScriptWatcher const&) = delete;
    InitScriptWatcher& operator=(InitScriptWatcher const&) = delete;

    // -1 if the watcher failed to start (e.g. pipe()/fcntl() failure) --
    // the main loop simply never selects on it, so auto-reload is silently
    // unavailable rather than fatal (matching the "never block startup"
    // invariant extended to the reload mechanism itself).
    [[nodiscard]] int wakeDescriptor() const noexcept { return wakePipe_[0]; }

    // Drains the wake pipe and, if a stable new script is queued,
    // evaluates it on the CALLING (main) thread. Call this only after the
    // main loop's select() reports wakeDescriptor() readable.
    void drainAndEvaluate(ssg::EditorRuntime& runtime) {
        char buffer[64];
        while (::read(wakePipe_[0], buffer, sizeof buffer) > 0) {
        }
        std::optional<std::string> pending;
        {
            std::lock_guard lock{mutex_};
            pending = std::move(pendingScript_);
            pendingScript_.reset();
        }
        if (pending) {
            evaluateInitScript(runtime, scriptPath_, *pending);
        }
    }

private:
    void run() {
        // `lastRead` is the previous poll's reading (to detect two
        // consecutive identical reads = stable); `lastApplied_` (shared
        // with the constructor's seed) is the content last actually
        // queued for evaluation -- empty after a delete is observed, so a
        // later recreation compares against an empty baseline and reloads
        // exactly like any other change (doc/spec-config.md: recreate
        // behaves like the file appearing for the first time).
        std::optional<std::string> lastRead;
        std::unique_lock lock{mutex_};
        while (!stop_) {
            lock.unlock();
            auto current = readInitScriptIfPresentQuiet(scriptPath_);
            lock.lock();
            if (current && lastRead && *current == *lastRead &&
                *current != lastApplied_) {
                // Stable across two consecutive polls (this reading and
                // the last) AND different from what was last applied --
                // queue it and wake the main thread.
                lastApplied_ = *current;
                bool const wasEmpty = !pendingScript_.has_value();
                pendingScript_ = *current;
                if (wasEmpty) {
                    char const tag = 'i';
                    (void)::write(wakePipe_[1], &tag, 1);
                }
            } else if (!current) {
                // Deleted, unreadable, or blank (readInitScriptIfPresentQuiet
                // treats blank the same as absent -- see isBlank): clear the
                // applied baseline so a later recreation is treated as
                // fresh, per doc/spec-config.md -- but take NO action on
                // the runtime itself.
                lastApplied_.clear();
            }
            lastRead = current;
            wake_.wait_for(lock, kInitScriptPollInterval,
                           [this] { return stop_; });
        }
    }

    // Same shape as readInitScriptIfPresent, but never prints a diagnostic
    // -- this runs continuously on a background thread, so a transient
    // read failure (e.g. observed mid-rename) must not spam stderr; only
    // the eventual evaluateInitScript() call (on a STABLE, successfully
    // read script) can ever produce a diagnostic, exactly like startup.
    // Blank content (see isBlank) is treated the same as absent, so a
    // zero-byte read observed mid-truncate/mid-save can never be queued or
    // evaluated as a false "successful reload of nothing".
    static std::optional<std::string> readInitScriptIfPresentQuiet(
        std::filesystem::path const& scriptPath) {
        std::error_code existsError;
        bool const present = std::filesystem::exists(scriptPath, existsError);
        if (existsError || !present) return std::nullopt;
        std::ifstream input{scriptPath, std::ios::binary};
        if (!input) return std::nullopt;
        std::ostringstream buffer;
        buffer << input.rdbuf();
        auto text = buffer.str();
        if (isBlank(text)) return std::nullopt;
        return text;
    }

    std::filesystem::path scriptPath_;
    int wakePipe_[2] = {-1, -1};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::string lastApplied_;
    std::optional<std::string> pendingScript_;
};

}  // namespace

int main(int argc, char** argv) {
    STARTUP_MARK("main_entry");
    fs::path argument = argc > 1 ? fs::path{argv[1]} : fs::path{};
    auto target = ssg::app::resolve_launch(argument);

    auto base = fs::temp_directory_path() / ("ssg-" + std::to_string(::getpid()));
    std::error_code code;
    fs::create_directories(base / "scratch", code);
    fs::create_directories(base / "recovery", code);

    ssg::EditorRuntimeConfig config;
    config.cwd = target.cwd;
    config.scratchRoot = base / "scratch";
    config.recoveryRoot = base / "recovery";
    // M10 fast startup: defer the workspace tree scan and syntax highlighting off
    // the first-frame path; prime_deferred() runs them once the first frame is
    // drawn.
    config.deferEnrichment = true;
    config.syntaxParser = ssg::defaultSyntaxParser();
    auto created = ssg::EditorRuntime::create(config);
    if (!created.accepted()) {
        std::fprintf(stderr, "ssg: %s\n", created.message.c_str());
        return 1;
    }
    auto& runtime = *created.runtime;
    int const gitDiffWakeFd = runtime.gitDiffWakeDescriptor();
    STARTUP_MARK("post_create");

    ssg::ClientId client{1};
    if (!runtime.attach({client, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1})
             .accepted()) {
        std::fprintf(stderr, "ssg: failed to attach client\n");
        return 1;
    }
    STARTUP_MARK("post_attach");

    auto const appliedInitScript = loadInitScript(runtime);
    STARTUP_MARK("post_init_script");

    // doc/spec-config.md's auto-reload: watches the SAME path just loaded
    // above, on a background thread, and wakes the main loop's select() to
    // re-evaluate it when it changes. Absent if the config root itself
    // could not be resolved (already diagnosed by loadInitScript above).
    // Seeded with whatever loadInitScript already applied, so the
    // watcher's first poll never redundantly re-queues the SAME unchanged
    // startup content as if it were a new edit.
    std::optional<InitScriptWatcher> initScriptWatcher;
    if (auto scriptPath = resolveInitScriptPath()) {
        initScriptWatcher.emplace(*scriptPath, appliedInitScript);
    }

    // Set when a command-line file argument is successfully opened below;
    // used after the deferred-startup panel.show_files dispatch (in the
    // render loop) to re-assert editor focus, since that dispatch's
    // showPanelProvider side effect moves focus to the panel.
    bool fileOpenedAtStartup = false;
    if (target.file) {
        if (fs::exists(target.cwd / *target.file)) {
            auto const openResult = runtime.dispatch(
                client, {"file.open", runtime.revision(), *target.file});
            fileOpenedAtStartup = openResult.accepted();
            // panel.show_files (dispatched later, once the deferred tree
            // scan below completes) moves focus to the panel as a side
            // effect; when a file was explicitly named on the command
            // line, the user wants to start editing it, so focus is
            // re-asserted onto the editor AFTER that dispatch runs (see
            // fileOpenedAtStartup's use below), not here.
        }
    }
    STARTUP_MARK("post_open");

    TerminalMode mode;
    if (!mode.active()) {
        std::fprintf(stderr, "ssg: stdin/stdout is not an interactive terminal\n");
        return 1;
    }

    // M9-W: a non-blocking self-pipe the event loop selects on, woken by signal
    // handlers that write one tag byte each.  Both ends are non-blocking so the
    // handler never blocks and a drain never stalls.
    int signalPipe[2] = {-1, -1};
    if (::pipe(signalPipe) != 0) {
        std::fprintf(stderr, "ssg: failed to create signal pipe\n");
        return 1;
    }
    ::fcntl(signalPipe[0], F_SETFL,
            ::fcntl(signalPipe[0], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(signalPipe[1], F_SETFL,
            ::fcntl(signalPipe[1], F_GETFL, 0) | O_NONBLOCK);
    gSignalPipeWrite = signalPipe[1];
    installSignalTagHandler(SIGWINCH);
    installSignalTagHandler(SIGTERM);
    installSignalTagHandler(SIGHUP);

    // Detect the terminal color depth once at startup (M9-C2); the frame encoder
    // adapts the theme's 16 colors to it via the library's resolve_color.
    ssg::ColorDepth const colorDepth =
        ssg::app::detect_color_depth(std::getenv("SSG_COLOR_DEPTH"),
                                     std::getenv("COLORTERM"), std::getenv("TERM"),
                                     std::getenv("TERM_PROGRAM"));

    // Drain and classify any pending signal tags.  Returns false to keep looping;
    // a terminating signal does not return — it restores the terminal in normal
    // context (tcsetattr is not async-signal-safe) and re-raises with the default
    // disposition so the exit status reflects the signal (M9-X).  A resize needs
    // no work here: the next snapshot at the loop top re-queries terminal_size().
    auto drainSignals = [&]() -> void {
        char scratch[64];
        std::string tags;
        for (;;) {
            auto const n = ::read(signalPipe[0], scratch, sizeof scratch);
            if (n <= 0) break;
            tags.append(scratch, static_cast<std::size_t>(n));
        }
        auto const events = ssg::app::classify_signal_tags(tags);
        if (events.terminate) {
            int const signo = *events.terminate;
            mode.restore();
            ::signal(signo, SIG_DFL);
            ::raise(signo);
        }
    };

    std::string buffer;             // Raw bytes read but not yet decoded.
    ssg::KeySequence chord;         // The pending (mid-entry) key chord.
    bool quit = false;
    auto focus = ssg::FocusTarget::Editor;
    // Client-owned palette state: query and selection are local (reported for
    // library presentation), ranked against the server's published candidates.
    bool pickerOpen = false;
    // Which picker the open prompt is, adopted from the snapshot rather than
    // invented locally, so submit routes to that picker's command.
    ssg::SearchMode pickerMode = ssg::SearchMode::Command;
    // One value, not four parallel locals: the library's own window type carries
    // query, selection, scroll offset and pane height together, so a second
    // picker cannot introduce a drifting copy of half of them.
    // The pane height is cached from the last snapshot (the palette pane == the
    // editor pane, so it is populated before any picker opens; see
    // doc/spec-scroll.md R3). The window is resolved with the shared list-scroll
    // primitive.
    ssg::PaletteWindowState picker{};
    // Mouse drag state (M8): a left press on the editor records the anchor and
    // enters dragging; subsequent motion extends the selection. Client-local and
    // transient — the server only ever sees cursor.set_position / select.set_range.
    bool dragging = false;
    std::optional<ssg::DocumentPosition> dragAnchor;
    // The last pointer cell (0-based) from a press/drag, so a drag held still at
    // the editor edge can auto-scroll on a timer without a fresh pointer event
    // (M8-S2).
    int lastPointerColumn = 0;
    int lastPointerRow = 0;
    // Find prompt: the client holds no authoritative query.  It reads the
    // published controller query (adopted in refresh), edits it, and reports the
    // full next string via find.update_query.  find_open mirrors the controller.
    bool findOpen = false;
    std::string findQuery;
    // Replace prompt: the query row is display-only; the client edits the
    // replacement the same no-copy way as the find query (reads the published
    // replacement, mutates it, dispatches replace.update_replacement).
    bool replaceOpen = false;
    std::string replaceReplacement;
    ssg::KeymapViewState keymap;
    std::vector<ssg::PaletteCandidate> candidates;

    auto dispatch = [&](std::string_view id, std::any payload = {}) {
        (void)runtime.dispatch(client, {std::string{id}, runtime.revision(),
                                        std::move(payload)});
    };
    // Re-center the client-owned palette window on the current selection
    // (keep-visible). Called ONLY when the selection changes (arrow navigation,
    // open, type, backspace); the per-frame build_report otherwise honors the
    // free offset so a wheel scroll persists (see doc/spec-m8.md M8-P). Mirrors
    // the tree's reveal_tree_selection.
    auto revealPaletteSelection = [&] {
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        if (picker.selected >= order.size()) {
            picker.selected = order.empty() ? 0 : order.size() - 1;
        }
        std::optional<std::uint32_t> selected =
            order.empty() ? std::nullopt
                          : std::optional<std::uint32_t>{
                                static_cast<std::uint32_t>(picker.selected)};
        auto scroll = ssg::Viewport{}.listScrollView(
            static_cast<std::uint32_t>(order.size()), picker.paneRows,
            picker.firstVisible, selected, /*keep_selection_visible=*/true);
        picker.firstVisible = scroll.firstVisible;
    };
    // Scroll the client-owned palette window by `delta` rows WITHOUT moving the
    // selection (a wheel over the open palette), clamped to [0, maximum_first_row].
    // Saturating: `delta` is a decoded int64, so guard the extremes before adding.
    auto scrollPalette = [&](std::int64_t delta) {
        if (!pickerOpen) return;
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        auto probe = ssg::Viewport{}.listScrollView(
            static_cast<std::uint32_t>(order.size()), picker.paneRows,
            picker.firstVisible, std::nullopt, /*keep_selection_visible=*/false);
        auto const maximum =
            static_cast<std::int64_t>(probe.scrollbar.maximumFirstRow);
        auto const current = static_cast<std::int64_t>(picker.firstVisible);
        std::int64_t next;
        if (delta >= maximum) {
            next = maximum;
        } else if (delta <= -maximum) {
            next = 0;
        } else {
            next = std::clamp<std::int64_t>(current + delta, 0, maximum);
        }
        picker.firstVisible = static_cast<std::uint32_t>(next);
    };
    // Submit routes to the open picker's command, chosen from the mode the
    // server published rather than assumed: a candidate id means different
    // things per picker (a command id for the command palette), so a single
    // hardcoded submit would silently misinterpret another picker's ids.
    auto submitSelectedCandidate = [&] {
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        if (order.empty() || picker.selected >= order.size()) return;
        auto const& id = candidates[order[picker.selected]].id;
        switch (pickerMode) {
        case ssg::SearchMode::Command:
            dispatch("palette.execute", ssg::PaletteExecuteArguments{id});
            break;
        case ssg::SearchMode::File:
            // file.open has no prompt side effects (unlike palette.execute,
            // which cancels the prompt as part of executing), so the picker must
            // be closed explicitly or it survives its own submit.
            dispatch("file.open", id);
            dispatch("palette.close");
            break;
        default:
            break;
        }
    };
    // Dispatch a resolved command, fulfilling prompt-context commands against the
    // client-local palette view when a palette prompt is open (doc/spec-keymap.md
    // Prompt-focus fulfillment).  Palette open/closed is reconciled from the
    // server focus on the next snapshot, not forced here, so a failed submit (no
    // candidate / rejected execute) leaves the prompt open rather than
    // desynchronizing the client.
    auto dispatchResolved = [&](std::string const& id) {
        if (pickerOpen && focus == ssg::FocusTarget::Prompt) {
            if (id == "prompt.submit") { submitSelectedCandidate(); return; }
            if (id == "prompt.cancel") { dispatch("palette.close"); return; }
            if (id == "prompt.next" || id == "palette.next") {
                ++picker.selected;
                revealPaletteSelection();
                return;
            }
            if (id == "prompt.previous" || id == "palette.previous") {
                if (picker.selected > 0) --picker.selected;
                revealPaletteSelection();
                return;
            }
        }
        dispatch(id);
        // Any picker's open command starts a fresh window.  Driven off the
        // catalog rather than a hardcoded "palette.open" so adding a picker
        // cannot forget to reset the query and selection -- which silently
        // inherits the previous picker's filter.
        bool opensAPicker = false;
        for (auto const& descriptor : ssg::pickerCatalog().descriptors()) {
            if (id == descriptor.openCommandId) opensAPicker = true;
        }
        if (opensAPicker) {
            pickerOpen = true;
            picker.query.clear();
            picker.selected = 0;
            picker.firstVisible = 0;
            revealPaletteSelection();
        }
    };
    auto routeText = [&](std::string const& text) {
        switch (ssg::SemanticInputRouter{}.textRouting(ssg::focusTargetName(focus))) {
        case ssg::TextRouting::Insert:
            dispatch("text.insert", ssg::TextInputArguments{text});
            break;
        case ssg::TextRouting::PromptQuery:
            if (pickerOpen) { picker.query += text; picker.selected = 0; revealPaletteSelection(); }
            else if (replaceOpen) { dispatch("replace.update_replacement", ssg::FindQueryArguments{replaceReplacement + text}); }
            else if (findOpen) { dispatch("find.update_query", ssg::FindQueryArguments{findQuery + text}); }
            break;
        case ssg::TextRouting::Ignore:
            break;
        }
    };

    auto buildReport = [&] {
        ssg::PaletteReport report;
        if (pickerOpen) {
            // The library owns the palette projection: rank the published
            // candidates, window them, and assemble the bounded report. The app
            // supplies only the client-owned window (query/selection/scroll) and
            // adopts back the clamped selection and resolved offset, inventing no
            // product data (INV-derived-view-bounded).
            report = ssg::PaletteSearcher{}.report(candidates, picker);
        }
        return report;
    };
    // Take a fresh snapshot and adopt its authoritative client state (focus,
    // keymap, published candidates).  Called before every input event so that
    // coalesced input after a focus-changing command routes against the new
    // focus rather than a stale one.
    auto refresh = [&]() -> std::optional<ssg::SessionSnapshot> {
        auto snapshot = runtime.snapshot(client, terminalSize(), chord, buildReport());
        if (snapshot) {
            focus = snapshot->sections().shell.focus;
            keymap = snapshot->sections().keymap;
            candidates = snapshot->sections().palette.candidates;
            pickerMode = snapshot->sections().palette.mode;
            // Cache the palette pane height for the next window computation: the
            // palette pane is the editor pane, so this is populated every frame,
            // including before the palette opens (no cold start). The window is
            // computed from the PREVIOUS frame's height, so a terminal resize
            // lags one frame before keep-visible re-settles — the same one-frame
            // clamp the editor's server-side scroll offset already has, and it
            // self-corrects on the next snapshot.
            auto const& shell = snapshot->sections().shell;
            if (!shell.panes.empty()) {
                picker.paneRows = static_cast<std::uint32_t>(
                    std::max(shell.panes.front().content.height, 1));
            }
            // Derive find fulfillment from the ACTIVE prompt kind, not merely the
            // controller being open under prompt focus: a palette/settings prompt
            // may be active while the find controller is still open, and find
            // fulfillment must not hijack that unrelated prompt's keys.
            auto const& findView = snapshot->sections().findReplace;
            auto const& activePrompt = snapshot->sections().promptStatus.prompt;
            pickerOpen =
                activePrompt && activePrompt->kind == ssg::PromptKind::Palette;
            bool const findPromptActive =
                activePrompt && activePrompt->kind == ssg::PromptKind::Find;
            findOpen = findView.open && findPromptActive;
            findQuery = findView.query;
            // The replace prompt edits the replacement, not the query.
            bool const replacePromptActive =
                activePrompt && activePrompt->kind == ssg::PromptKind::Replace;
            replaceOpen = findView.open && replacePromptActive;
            replaceReplacement = findView.replacement;
        }
        return snapshot;
    };

    // Top-level boundary (M9-X): an exception escaping the loop is not portably
    // guaranteed to unwind `mode` once past main, so restore the terminal here
    // before it propagates.
    bool firstFrameMarked = false;
    try {
        while (!quit) {
            auto snapshot = refresh();
            if (snapshot) {
                // The library renders every screen branch, including the declined-
                // layout "too small" placeholder (M11-L); the app only encodes.
                auto grid = ssg::Renderer{}.render(*snapshot);
                std::string frame = "\x1b[?25l";  // Hide the cursor while redrawing.
                frame += ssg::app::encode_ansi_frame(grid, colorDepth);
                if (grid.caret) {
                    frame += "\x1b[" + std::to_string(grid.caret->row + 1) + ";" +
                             std::to_string(grid.caret->column + 1) + "H\x1b[?25h";
                }
                if (!firstFrameMarked) {
                    // M10-1 stop mark: the first content frame (an actual rendered
                    // payload), not the earlier terminal-setup bytes.
                    STARTUP_MARK("first_content_frame");
                    firstFrameMarked = true;
                    writeAll(frame);
                    // M10-3/M10-4: the first frame is on screen; now run the
                    // enrichment (tree scan, syntax) deferred off the startup
                    // path.  It publishes on the next snapshot at the loop top.
                    runtime.primeDeferred();
                    // Always open the Files sidebar once the workspace tree
                    // scan above has populated the "filesystem" tree
                    // provider -- lets a user immediately browse for a file
                    // to open without a separate keystroke. Uses the SAME
                    // panel.show_files command Escape-B/Escape-O reach
                    // interactively; this is not a new mechanism, only
                    // sequenced AFTER primeDeferred() rather than before
                    // it: dispatching it earlier (pre-loop, before the
                    // deferred tree scan has run) would fail with "files
                    // tree provider is unavailable" every startup, since
                    // the M10 fast-startup path defers registering that
                    // provider until exactly this point.
                    if (auto const panelResult = runtime.dispatch(
                            client, {"panel.show_files", runtime.revision(), {}});
                        !panelResult.accepted()) {
                        std::fprintf(stderr, "ssg: could not open Files sidebar: %s\n",
                                    panelResult.message.c_str());
                    }
                    // panel.show_files (just above) moves focus to the
                    // panel as a side effect (ShellState::showPanelProvider
                    // -> togglePanel); when a file was explicitly named on
                    // the command line and successfully opened earlier, the
                    // user wants to start editing it, so re-assert editor
                    // focus here, AFTER panel.show_files, so it's the final
                    // word on where focus lands.
                    if (fileOpenedAtStartup) {
                        runtime.focusEditor();
                    }
                    continue;
                }
                writeAll(frame);
            }

            char bytes[64];
            // Edge auto-scroll (M8-S2): if a drag is held past the top/bottom of the
            // editor content, don't block indefinitely on input — wake on a timer to
            // scroll one line and re-extend the selection to the new edge cell, so a
            // drag held still at the edge keeps scrolling and selecting.
            std::optional<int> dragEdge;
            if (dragging && snapshot && !snapshot->sections().shell.panes.empty()) {
                dragEdge = ssg::app::edge_scroll(
                    dragging, lastPointerRow,
                    snapshot->sections().shell.panes.front().content);
            }
            if (dragEdge) {
                auto const ready = waitReadiness(kEdgeScrollIntervalMs, signalPipe[0], -1);
                if (ready.signal) {
                    // A resize/terminate signal arrived mid-drag: drain it now rather
                    // than deferring until the drag releases.  Terminate does not
                    // return (restore + re-raise); resize re-snapshots at the loop
                    // top with the drag preserved.
                    drainSignals();
                    continue;
                }
                if (!ready.input) {
                    dispatch("view.scroll_lines", ssg::ScrollLinesArguments{*dragEdge});
                    auto scrolled = refresh();
                    if (scrolled && dragAnchor &&
                        !scrolled->sections().shell.panes.empty()) {
                        auto const content = scrolled->sections().shell.panes.front().content;
                        int const edgeRow =
                            *dragEdge < 0 ? content.y : content.bottom() - 1;
                        int const column = std::clamp(lastPointerColumn, content.x,
                                                      content.right() - 1);
                        auto hit = ssg::HitTester{*scrolled}.at(column, edgeRow);
                        if (hit.region == ssg::HitRegion::Editor) {
                            auto active = ssg::SelectionNavigator::resolvePosition(
                                scrolled->sections().document.text,
                                ssg::ByteOffset{hit.byteOffset});
                            if (active) {
                                dispatch("select.set_range",
                                         ssg::SelectionCommandArguments{
                                             std::nullopt,
                                             ssg::Selection{*dragAnchor, *active}});
                            }
                        }
                    }
                    continue;  // re-render with the scrolled viewport, then re-evaluate
                }
                // Keyboard input arrived during the drag: fall through and read it.
            }
            // Block until keyboard input OR a signal-driven self-pipe wake (M9-W)
            // OR a runtime git-diff wake OR an init-script reload wake. A bare
            // read() could not be interrupted reliably by resize/terminate or
            // background diff/config updates.
            auto const wait = waitReadiness(
                -1, signalPipe[0], gitDiffWakeFd,
                initScriptWatcher ? initScriptWatcher->wakeDescriptor() : -1);
            if (wait.signal) {
                // Terminate does not return (restore + re-raise); a resize just
                // re-snapshots at the loop top.
                drainSignals();
                if (!wait.input) continue;
            }
            if (wait.initScript) {
                // Unlike git-diff (auto-drained inside EditorRuntime::snapshot()),
                // nothing else drains this -- evaluate the reloaded script here,
                // on the main thread, exactly like startup's loadInitScript.
                initScriptWatcher->drainAndEvaluate(runtime);
                if (!wait.input) continue;
            }
            if (wait.gitDiff && !wait.input) continue;
            if (!wait.input) continue;  // Wake-only cycle: re-render and retry.
            auto readBytes = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (readBytes <= 0) break;
            buffer.append(bytes, static_cast<std::size_t>(readBytes));

        bool firstEvent = true;
        while (!buffer.empty() && !quit) {
            std::size_t consumed = 0;
            auto decoded = ssg::app::decode_input(buffer, false, consumed);
            if (decoded.status == ssg::app::DecodeStatus::incomplete) {
                // A partial sequence (lone ESC or truncated CSI) remains.  Wait
                // briefly for the disambiguating bytes; if none arrive, force the
                // bounded-Escape resolution.  The wait also watches the signal
                // pipe so a resize/terminate is not deferred by a lone ESC.
                auto const ready = waitReadiness(kEscapeTimeoutMs, signalPipe[0], -1);
                if (ready.signal) drainSignals();
                if (ready.input) {
                    auto more = ::read(STDIN_FILENO, bytes, sizeof bytes);
                    if (more > 0) {
                        buffer.append(bytes, static_cast<std::size_t>(more));
                        continue;
                    }
                }
                decoded = ssg::app::decode_input(buffer, true, consumed);
                if (decoded.status == ssg::app::DecodeStatus::incomplete) break;
            }
            buffer.erase(0, consumed);

            // Adopt fresh authoritative focus before every event after the first
            // (the first uses the snapshot already taken at the top of the loop).
            // Capture the refreshed snapshot so pointer hit-testing sees the
            // current frame's layout.
            if (!firstEvent) snapshot = refresh();
            firstEvent = false;

            if (decoded.status == ssg::app::DecodeStatus::pointer) {
                lastPointerColumn = decoded.pointer.column;
                lastPointerRow = decoded.pointer.row;
                // Classify the cell via the library hit_test, resolve the target
                // the hit needs, then let the pure route_pointer decide the
                // command sequence and drag-state change.
                ssg::RegionHit hit;
                ssg::app::PointerTargets targets;
                if (snapshot) {
                    hit = ssg::HitTester{*snapshot}.at( decoded.pointer.column,
                                        decoded.pointer.row);
                    if (hit.region == ssg::HitRegion::Editor) {
                        targets.document_position = ssg::SelectionNavigator::resolvePosition(
                            snapshot->sections().document.text,
                            ssg::ByteOffset{hit.byteOffset});
                    } else if (hit.region == ssg::HitRegion::Tab) {
                        auto const& tabs = snapshot->sections().tabs.tabs;
                        if (hit.tabIndex < tabs.size()) {
                            targets.tab_id = tabs[hit.tabIndex].id;
                        }
                    } else if (hit.region == ssg::HitRegion::Palette) {
                        // Map the absolute rank index to its candidate id using
                        // the same ranked order the client renders.
                        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
                        if (hit.itemIndex < order.size()) {
                            targets.picker_candidate_id =
                                candidates[order[hit.itemIndex]].id;
                            targets.picker_mode = pickerMode;
                        }
                    } else if (hit.region == ssg::HitRegion::HeaderField ||
                               hit.region == ssg::HitRegion::FooterField) {
                        targets.field_command_id = hit.commandId;
                    }
                }
                auto plan =
                    ssg::app::route_pointer(hit, decoded.pointer.button,
                                            decoded.pointer.kind, dragging,
                                            dragAnchor, targets);
                for (auto const& command : plan.commands) {
                    dispatch(command.command_id, command.payload);
                }
                if (plan.begins_drag) {
                    dragging = true;
                    dragAnchor = targets.document_position;
                }
                if (plan.ends_drag) {
                    dragging = false;
                    dragAnchor.reset();
                }
                chord.clear();
                continue;
            }

            if (decoded.status == ssg::app::DecodeStatus::scroll) {
                // Route the wheel to the region under the pointer: the side panel
                // scrolls its tree, the open palette scrolls its client-owned
                // window, everything else scrolls the editor document.
                ssg::HitRegion region = ssg::HitRegion::None;
                if (snapshot) {
                    region = ssg::HitTester{*snapshot}.at( decoded.pointer.column,
                                           decoded.pointer.row).region;
                }
                switch (ssg::app::route_wheel(region)) {
                    case ssg::app::WheelTarget::editor:
                        dispatch("view.scroll_lines",
                                 ssg::ScrollLinesArguments{decoded.scroll});
                        break;
                    case ssg::app::WheelTarget::tree:
                        dispatch("tree.scroll",
                                 ssg::ScrollLinesArguments{decoded.scroll});
                        break;
                    case ssg::app::WheelTarget::palette:
                        scrollPalette(decoded.scroll);
                        break;
                    case ssg::app::WheelTarget::none:
                        break;
                }
                chord.clear();
                continue;
            }
            if (decoded.status != ssg::app::DecodeStatus::key) {
                // A recognized but unhandled byte (unknown CSI, stray control):
                // a non-matching continuation that clears any pending chord.
                chord.clear();
                continue;
            }

            // A printable without a keycode (e.g. multibyte text) cannot be a
            // chord; route it straight to the text sink.
            if (decoded.stroke.code.empty()) {
                routeText(decoded.text);
                chord.clear();
                continue;
            }

            chord.push_back(decoded.stroke);
            auto resolution = ssg::KeymapMatcher{keymap}.resolveSequence(chord, ssg::focusTargetName(focus));
            if (resolution.kind == ssg::KeymapMatchKind::Resolved) {
                dispatchResolved(resolution.commandId);
                chord.clear();
            } else if (resolution.kind == ssg::KeymapMatchKind::Pending) {
                // Keep collecting; the leader hint renders next frame.
            } else {
                // No binding.  Quit is the sole app-local chord (process
                // lifecycle); everything else clears the chord and, for a
                // printable, still routes as text.
                const bool quitChord =
                    chord.size() == 2 && chord[0].code == "Escape" &&
                    chord[1].code == "KeyQ";
                const auto stroke = decoded.stroke;
                chord.clear();
                if (quitChord) {
                    quit = true;
                } else if (pickerOpen && focus == ssg::FocusTarget::Prompt &&
                           stroke.code == "Backspace") {
                    popCodePoint(picker.query);
                    picker.selected = 0;
                    revealPaletteSelection();
                } else if (findOpen && focus == ssg::FocusTarget::Prompt &&
                           stroke.code == "Backspace") {
                    auto next = findQuery;
                    popCodePoint(next);
                    dispatch("find.update_query", ssg::FindQueryArguments{next});
                } else if (replaceOpen && focus == ssg::FocusTarget::Prompt &&
                           stroke.code == "Backspace") {
                    auto next = replaceReplacement;
                    popCodePoint(next);
                    dispatch("replace.update_replacement", ssg::FindQueryArguments{next});
                } else if (!decoded.text.empty()) {
                    routeText(decoded.text);
                }
            }
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

    return 0;
}
