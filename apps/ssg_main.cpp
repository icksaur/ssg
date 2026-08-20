// The `ssg` terminal editor entry point.  This file owns terminal I/O only:
// raw mode, size queries, byte reads, frame writes, and clean restoration.  All
// editor, workspace, and layout behavior is the ssg library's; the app attaches
// an in-process client to an EditorRuntime, renders the library's snapshot, and
// forwards input.
//
// Milestone 1 scope: launch over a path argument, draw the shell grid, and quit
// on Alt+Q.  Input translation through the library keymap and
// editing arrive in later milestones; quitting is an application lifecycle
// concern owned here.

#include "http_serve.h"
#include "pointer_routing.h"
#include "ssg_terminal.h"

#include <ssg/EditorRuntime.h>
#include <ssg/TreeSitterGrammars.h>
#include <ssg/HitTester.h>
#include <ssg/FindReplace.h>
#include <ssg/Keymap.h>
#include <ssg/LuaCommandHost.h>
#include <ssg/ScriptHost.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PaletteSubmit.h>
#include <ssg/Picker.h>
#include <ssg/platform_files.h>
#include <ssg/session_snapshot.h>
#include <ssg/TextInputCommands.h>

#include "init_script.h"

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
#include <charconv>
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

using ssg::app::evaluateInitScript;

// Env-gated raw-input diagnostic: when SSG_LOG_INPUT names a file, every chunk
// of bytes read from stdin is appended as space-separated two-digit hex. This is
// how we discover exactly what a given terminal transmits for a key (e.g. what
// Alt+Home actually sends) without guessing. Off unless the variable is set, and
// it never affects decoding -- it only observes.
void logRawInput(char const* bytes, std::size_t count) {
    static char const* const path = std::getenv("SSG_LOG_INPUT");
    if (path == nullptr || count == 0) return;
    // seam-exempt: append-only keystroke-byte diagnostic, not file content access
    std::FILE* file = std::fopen(path, "a");
    if (file == nullptr) return;
    static char const* const hex = "0123456789abcdef";
    for (std::size_t i = 0; i < count; ++i) {
        auto const byte = static_cast<unsigned char>(bytes[i]);
        std::fputc(hex[byte >> 4], file);
        std::fputc(hex[byte & 0x0f], file);
        std::fputc(' ', file);
    }
    std::fputc('\n', file);
    std::fclose(file);
}

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
    // seam-exempt: append-only diagnostic trace, not file content access
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
//
// The escape-sequence half is delegated to TerminalModes: each mode names its
// own exit, and the bytes that undo whatever is currently entered are kept as
// data so a fatal-signal handler can write them without traversing anything
//.  termios is not a mode in that
// sense -- it is restored by value, and tcsetattr is not async-signal-safe --
// so it stays here.
class TerminalMode {
public:
    TerminalMode()
        : modes_{[](std::string_view bytes) { writeAll(std::string{bytes}); }} {
        if (tcgetattr(STDIN_FILENO, &original_) != 0) return;
        termios raw = original_;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
        active_ = true;
        // Entry order is the curated order, and the guards leave in reverse:
        // mouse reporting is disabled before the alternate screen is left, or
        // reporting would stay on in the primary screen.
        entered_.push_back(modes_.enter(ssg::app::kAlternateScreen));
        entered_.push_back(modes_.enter(ssg::app::kCursorStyleBar));
        entered_.push_back(modes_.enter(ssg::app::kMouseButtons));
        entered_.push_back(modes_.enter(ssg::app::kMouseMotion));
        entered_.push_back(modes_.enter(ssg::app::kMouseSgrCoordinates));
        entered_.push_back(modes_.enter(ssg::app::kBracketedPaste));
    }

    ~TerminalMode() { restore(); }

    // Restore the terminal to its pre-launch state.  Safe to call more than once
    // (the destructor calls it again); only the first call does the work.
    void restore() noexcept {
        if (!active_) return;
        active_ = false;
        // Dropping the guards in reverse writes every leave sequence.  Nothing
        // is written by hand here: the cursor is hidden and shown within each
        // frame, so no cursor debt survives a frame for teardown to settle.
        entered_.clear();
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    }

    TerminalMode(TerminalMode const&) = delete;
    TerminalMode& operator=(TerminalMode const&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }

    // Enter the Kitty keyboard protocol once, when the terminal has answered the
    // capability query.  Separate from the constructor's unconditional modes
    // because the probe reply lands asynchronously, after construction; latched so
    // a repeated capability check does not push flag 1 twice.  The Guard rides the
    // same `entered_` teardown as every other mode, so restore()/the destructor
    // pop it -- the pop is never written unless this push was.
    void enableKeyboardProtocol() {
        if (!active_ || keyboardProtocolEntered_) return;
        entered_.push_back(modes_.enter(ssg::app::kKeyboardProtocol));
        keyboardProtocolEntered_ = true;
    }

private:
    ssg::app::TerminalModes modes_;
    std::vector<ssg::app::TerminalModes::Guard> entered_;
    termios original_{};
    bool active_ = false;
    bool keyboardProtocolEntered_ = false;
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

// Idle wake cadence so the runtime's autosave debounce (default 10s, a library
// setting) gets a periodic "a moment passed" tick even when the user is not
// typing; the tick itself decides nothing, it only lets the library flush drafts
// of open dirty documents that are due (single-file draft recovery, M15).
constexpr int kAutosaveTickMs = 1000;

// Minimum interval between rendered frames WHILE a pointer drag is held. A drag
// (scrollbar thumb or text selection) makes the terminal emit motion events at a
// rate far above a useful refresh rate; without a cap the loop renders a full
// frame per event and pins a core. Coalescing collapses each read's events to
// one, and this cap bounds how often a coalesced result is rendered, so input
// accumulates for the rest of the window instead of driving another frame. ~60fps
// is smooth for a drag; the drag end position is always honoured because the last
// event is processed on release.
constexpr int kDragFrameIntervalMs = 16;

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
// mid-run delete (see InitScriptWatcher below). A genuine I/O error (a read
// failure other than "does not exist") also yields nullopt, and is reported
// only when `reportDiagnostics` is set.
//
// One read decides absent-vs-blank-vs-unreadable. The previous
// exists()-then-open pair could call a file that appeared between the two
// calls unreadable, and a file deleted between them a genuine I/O error.
std::optional<std::string> readInitScriptIfPresent(
    std::filesystem::path const& scriptPath, bool reportDiagnostics) {
    auto result = ssg::readFile(scriptPath);
    if (result.status == ssg::FileIoStatus::NotFound) return std::nullopt;
    if (!result.ok()) {
        if (reportDiagnostics) {
            std::fprintf(stderr, "ssg: could not read %s: %s\n",
                        scriptPath.string().c_str(), result.message.c_str());
        }
        return std::nullopt;
    }
    std::string text{reinterpret_cast<char const*>(result.bytes.data()),
                     result.bytes.size()};
    if (isBlank(text)) return std::nullopt;
    return text;
}

// Loads and evaluates `init.lua` exactly once at startup, via
// resolveInitScriptPath + readInitScriptIfPresent + evaluateInitScript.
// Absent file, or any of the diagnostic-then-return-nullopt cases above:
// silently continue starting with defaults (a broken config script must
// never block opening the
// editor). Returns the content actually applied (or nullopt), so the
// caller can seed InitScriptWatcher's "last applied" baseline and avoid
// redundantly re-evaluating the SAME unchanged content on its first poll.
std::optional<std::string> loadInitScript(ssg::ScriptHost& scripts,
                                          ssg::EditorRuntime& runtime) {
    auto const scriptPath = resolveInitScriptPath();
    if (!scriptPath) return std::nullopt;
    auto script = readInitScriptIfPresent(*scriptPath, true);
    if (!script) return std::nullopt;
    evaluateInitScript(scripts, runtime, *scriptPath, *script);
    return script;
}
// How often the background thread re-reads init.lua's content to check for
// a change. Content, not mtime/
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
    void drainAndEvaluate(ssg::ScriptHost& scripts, ssg::EditorRuntime& runtime) {
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
            evaluateInitScript(scripts, runtime, scriptPath_, *pending);
        }
    }

private:
    void run() {
        // `lastRead` is the previous poll's reading (to detect two
        // consecutive identical reads = stable); `lastApplied_` (shared
        // with the constructor's seed) is the content last actually
        // queued for evaluation -- empty after a delete is observed, so a
        // later recreation compares against an empty baseline and reloads
        // exactly like any other change (recreate
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
                // fresh -- but take NO action on
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
    // evaluated as a false "successful reload of nothing". Diagnostics are
    // suppressed here because a watcher fires on transient states a user did
    // not ask about; startup uses the same reader with reporting on.
    static std::optional<std::string> readInitScriptIfPresentQuiet(
        std::filesystem::path const& scriptPath) {
        return readInitScriptIfPresent(scriptPath, false);
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

namespace {

// `--capabilities`: ask the terminal what it can do and print the answers, then
// exit.  This is the only way a user can see what SSG believes about their
// terminal, which is what makes a wrong answer diagnosable and the SSG_TERM_*
// overrides actionable.
//
// It deliberately does NOT enter the alternate screen -- the report has to stay
// on the user's scrollback -- but it does need raw mode, or the replies would be
// line-buffered and echoed into the report itself.
int reportCapabilities() {
    ssg::app::TerminalCapabilities capabilities{
        [](std::string_view name) { return std::getenv(std::string{name}.c_str()); }};

    termios original{};
    bool raw = false;
    if (tcgetattr(STDIN_FILENO, &original) == 0) {
        termios probe = original;
        probe.c_lflag &= ~(ICANON | ECHO);
        probe.c_cc[VMIN] = 0;
        probe.c_cc[VTIME] = 0;
        raw = tcsetattr(STDIN_FILENO, TCSAFLUSH, &probe) == 0;
    }
    if (raw) {
        writeAll(capabilities.beginProbe());
        // Drain for the WHOLE window rather than stopping at the fence.  The
        // editor can stop early -- it has a frame to draw -- but the diagnostic's
        // job is to show what the terminal said, including anything that arrived
        // too late to be believed.
        auto const deadline = std::chrono::steady_clock::now() +
                              ssg::app::TerminalCapabilities::kProbeWindow;
        std::string buffer;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!waitReadiness(10, -1, -1).input) continue;
            char bytes[256];
            auto const count = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (count <= 0) continue;
            buffer.append(bytes, static_cast<std::size_t>(count));
            while (!buffer.empty()) {
                std::size_t consumed = 0;
                auto const decoded = ssg::app::decode_input(buffer, true, consumed);
                if (decoded.status == ssg::app::DecodeStatus::incomplete ||
                    consumed == 0) {
                    break;
                }
                if (decoded.status == ssg::app::DecodeStatus::reply) {
                    capabilities.observeReply(decoded.reply);
                }
                buffer.erase(0, consumed);
            }
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    }

    char const* const term = std::getenv("TERM");
    std::printf("TERM=%s\n", term == nullptr ? "(unset)" : term);
    std::printf("%-24s %s\n", "color_depth",
                ssg::app::color_depth_name(capabilities.colorDepth()).data());
    for (auto const capability : ssg::app::kAllCapabilities) {
        std::printf("%-24s %s\n",
                    std::string{ssg::app::capability_name(capability)}.c_str(),
                    capabilities.has(capability) ? "yes" : "no");
    }
    if (!raw) {
        std::printf(
            "\nstdin is not a terminal, so nothing was asked: every queried\n"
            "capability above reports its default.\n");
    }

    // What the terminal actually said.  A "no" above means one of three very
    // different things, and only these lines distinguish them: silence, a reply
    // in a shape SSG does not parse, or a reply that arrived after the DA1 fence
    // closed the window.
    auto const visible = [](std::string_view bytes) {
        std::string shown;
        for (unsigned char const byte : bytes) {
            if (byte == 0x1b) {
                shown += "<ESC>";
            } else if (byte < 0x20 || byte == 0x7f) {
                shown += '.';
            } else {
                shown += static_cast<char>(byte);
            }
        }
        return shown;
    };
    auto const& log = capabilities.probeLog();
    if (raw) {
        std::printf("\nreplies (%zu):\n", log.believed.size());
        for (auto const& reply : log.believed) {
            std::printf("  %s\n", visible(reply).c_str());
        }
        if (log.believed.empty()) {
            std::printf("  (none -- the terminal answered nothing at all)\n");
        }
        if (!log.ignored.empty()) {
            std::printf(
                "\nreplies that arrived AFTER the fence closed the window, and\n"
                "were therefore not believed (%zu):\n",
                log.ignored.size());
            for (auto const& reply : log.ignored) {
                std::printf("  %s\n", visible(reply).c_str());
            }
        }
    }
    std::printf(
        "\nOverride any answer with SSG_TERM_<NAME>=on|off (for example\n"
        "SSG_TERM_SYNCHRONIZED_OUTPUT=off), or the color depth with\n"
        "SSG_COLOR_DEPTH.  An override always beats what the terminal reports.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    STARTUP_MARK("main_entry");
    // `--` ends option parsing, so a file genuinely named `--capabilities` stays
    // openable (`ssg -- --capabilities`).  Without it the first flag this binary
    // ever grew would have quietly made a filename unreachable.
    int firstOperand = 1;
    std::optional<unsigned short> httpPort;
    if (argc > 1 && std::string_view{argv[1]} == "--") {
        firstOperand = 2;
    } else if (argc > 1 && std::string_view{argv[1]} == "--capabilities") {
        return reportCapabilities();
    } else if (argc > 1 && std::string_view{argv[1]} == "--http") {
        if (argc <= 2) {
            std::fprintf(stderr, "ssg: --http requires a PORT\n");
            return 2;
        }
        std::string_view const portText{argv[2]};
        unsigned long value = 0;
        auto const parsed = std::from_chars(
            portText.data(), portText.data() + portText.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != portText.data() + portText.size() || value == 0 ||
            value > 65535) {
            std::fprintf(stderr, "ssg: --http PORT must be 1-65535\n");
            return 2;
        }
        httpPort = static_cast<unsigned short>(value);
        // An optional path operand may follow the port (`ssg --http 8080 dir`).
        firstOperand = 3;
    }
    fs::path argument =
        argc > firstOperand ? fs::path{argv[firstOperand]} : fs::path{};
    auto target = ssg::app::resolve_launch(argument);

    // A STABLE per-user state root so drafts and their archive survive a restart
    // (single-file draft recovery, M15) -- unlike the old per-PID temp dir, which
    // vanished with the process. XDG_STATE_HOME (via userStateRoot) is the
    // standard override; SSG_STATE_DIR is a direct absolute-path override for
    // tests and packaging.
    fs::path stateBase;
    if (const char* stateOverride = std::getenv("SSG_STATE_DIR");
        stateOverride != nullptr && *stateOverride != '\0' &&
        fs::path{stateOverride}.is_absolute()) {
        stateBase = stateOverride;
    } else {
        stateBase = ssg::userStateRoot("ssg");
    }
    std::error_code code;
    fs::create_directories(stateBase / "scratch", code);
    fs::create_directories(stateBase / "archive", code);
    // The state subtree holds unsaved and deleted user content, so keep it
    // private regardless of the ambient umask; the ScratchStore hardens its own
    // journal files, but the enclosing directories are ours to protect.
    for (const auto& dir : {stateBase, stateBase / "scratch", stateBase / "archive"}) {
        std::error_code permissionError;
        if (fs::is_directory(dir, permissionError)) {
            try {
                ssg::setOwnerOnlyPermissions(dir);
            } catch (const std::exception&) {
                // Best-effort: a filesystem that cannot express owner-only
                // permissions must not stop the editor from launching.
            }
        }
    }

    // Recovery records are in-session undo of destructive filesystem ops; they
    // have no cross-restart requirement and MUST stay per-process, so a stable
    // shared recovery root (with no inter-process budget/eviction coordination)
    // cannot let concurrent instances race each other's records.
    auto recoveryBase =
        fs::temp_directory_path() / ("ssg-" + std::to_string(::getpid()));
    fs::create_directories(recoveryBase / "recovery", code);

    ssg::EditorRuntimeConfig config;
    config.cwd = target.cwd;
    config.scratchRoot = stateBase / "scratch";
    config.recoveryRoot = recoveryBase / "recovery";
    config.archiveRoot = stateBase / "archive";
    // M10 fast startup: defer the workspace tree scan and syntax highlighting off
    // the first-frame path; prime_deferred() runs them once the first frame is
    // drawn.
    config.deferEnrichment = true;
    config.syntaxParser = ssg::TreeSitterParserFactory::createDefault();
    auto created = ssg::EditorRuntime::create(config);
    if (!created.accepted()) {
        std::fprintf(stderr, "ssg: %s\n", created.message.c_str());
        return 1;
    }
    auto& runtime = *created.runtime;
    if (httpPort) {
        // Apply the same init.lua the TUI does BEFORE the web host attaches and
        // compiles the keymap, so both clients share the user's configured
        // bindings -- the library owns the keymap, and a host must not diverge
        // from it. `scripts` outlives run_http_server (which blocks until the
        // server stops), keeping any function init.lua defines callable. Live
        // reload of init.lua on the web path is deferred; startup parity is the
        // correctness fix.
        ssg::ScriptHost httpScripts{runtime};
        (void)loadInitScript(httpScripts, runtime);
        return ssg::app::run_http_server(runtime, *httpPort);
    }
    int const gitDiffWakeFd = runtime.gitDiffWakeDescriptor();
    STARTUP_MARK("post_create");

    ssg::ClientId client{1};
    if (!runtime.attach({client, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1})
             .accepted()) {
        std::fprintf(stderr, "ssg: failed to attach client\n");
        return 1;
    }
    STARTUP_MARK("post_attach");

    // Lives for the rest of the process, so a function init.lua defines
    // remains callable long after the script that defined it has finished.
    ssg::ScriptHost scripts{runtime};
    auto const appliedInitScript = loadInitScript(scripts, runtime);
    STARTUP_MARK("post_init_script");

    // Auto-reload: watches the SAME path just loaded
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

    // Set when startup leaves a document ready to type into -- either the file
    // named on the command line, or the unnamed buffer opened below. Used after
    // the deferred panel.show_files dispatch to re-assert editor focus, since
    // that dispatch moves focus to the panel. The condition is "is there
    // something to edit", NOT "was a file opened": a new buffer is just as
    // editable, and treating it otherwise silently swallows everything typed.
    bool startsWithAnEditableDocument = false;
    // Distinct from the above: did a file get opened BY NAME?  The Files sidebar
    // is opened at startup only when it was not -- someone who named a file came
    // to edit that file, and a sidebar covering a third of the screen is in the
    // way.  With no argument there is nothing to look at but an empty buffer, so
    // the sidebar is the useful thing to show.
    bool openedNamedFile = false;
    if (target.file) {
        if (fs::exists(target.cwd / *target.file)) {
            auto const openResult = runtime.dispatch(
                client, {"file.open", runtime.revision(), *target.file});
            startsWithAnEditableDocument = openResult.accepted();
            openedNamedFile = openResult.accepted();
            // panel.show_files (dispatched later, once the deferred tree
            // scan below completes) moves focus to the panel as a side
            // effect; when a file was explicitly named on the command
            // line, the user wants to start editing it, so focus is
            // re-asserted onto the editor AFTER that dispatch runs (see
            // startsWithAnEditableDocument's use below), not here.
        }
    }
    // Nothing to edit otherwise: no argument, an argument naming a file that
    // does not exist, or an open that failed. Starting on an unnamed buffer
    // means the editor is always typeable, and `file.save` prompts for a name
    // when the user is ready to keep it.
    if (!startsWithAnEditableDocument) {
        startsWithAnEditableDocument =
            runtime.dispatch(client, {"file.new", runtime.revision(), {}})
                .accepted();
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

    // What the terminal can do, resolved in one place (INV-capability-single-
    // source).  Color depth is answered from the environment immediately; the
    // queried capabilities stay at their conservative defaults until their
    // replies land, so nothing here blocks the first frame.
    ssg::app::TerminalCapabilities capabilities{
        [](std::string_view name) { return std::getenv(std::string{name}.c_str()); }};
    ssg::ColorDepth const colorDepth = capabilities.colorDepth();

    // Ask the terminal what it can do.  The answers arrive on stdin, which the
    // readiness loop below already watches, so nothing waits for them here and
    // the first frame renders against the conservative defaults
    // (INV-startup-unblocked).
    writeAll(capabilities.beginProbe());

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
    // editor pane, so it is populated before any picker opens). The window is
    // resolved with the shared list-scroll
    // primitive.
    ssg::PaletteWindowState picker{};
    // Mouse drag state (M8): a left press on the editor records the anchor and
    // enters dragging; subsequent motion extends the selection. Client-local and
    // transient — the server only ever sees cursor.set_position / select.set_range.
    bool dragging = false;
    std::optional<ssg::DocumentPosition> dragAnchor;
    // Alt-drag (multi-cursor) state: fixed at press, held for the gesture. The
    // baseline is the selection set captured BEFORE the press added its caret, so
    // every motion rebuilds the whole set from an immutable list (baseline + the
    // dragged range) and the other cursors never move.
    bool altDrag = false;
    std::vector<ssg::Selection> altDragBaseline;
    // Which scrollbar gutter a press landed on, held until release.  A drag is
    // routed to THIS gutter regardless of where the pointer has since moved, and
    // the cursor is hidden while it is set: during a drag the caret is not what
    // the user is looking at, and the terminal cursor otherwise flickers across
    // the screen chasing the frame's caret position.
    // A live scrollbar-thumb drag: the region, the gutter's top row, the thumb's
    // travel (viewportRows - thumbSize) and the offset from the thumb top to the
    // grabbed point, all captured at press.  Held so each drag motion moves the
    // grabbed point of the thumb to the cursor, rather than mapping the absolute
    // pointer row to the scroll position.
    struct GutterDrag {
        ssg::HitRegion region;
        int gutterY;
        int travel;
        int grabOffset;
    };
    std::optional<GutterDrag> draggingGutter;
    // Double-click tracking (client-side: the terminal reports no click count).
    // A second left editor press on the same cell within the window selects the
    // word.
    ssg::app::ClickTracker clickTracker;
    static constexpr auto kDoubleClickWindow = std::chrono::milliseconds{400};
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
    // Path prompt: same no-copy discipline. The library owns the value and
    // publishes it in the prompt view; the client reads it, edits it, and
    // reports the full next string via prompt.update_value.
    // A single-line text-entry prompt (Path or CommandArgument): both keep their
    // authoritative value in the library, publish it in the prompt view, and
    // collect edits through prompt.update_value on input index 0.
    bool textPromptOpen = false;
    std::string textPromptValue;
    // The last clipboard write served to the terminal, so one copy produces one
    // OSC 52 rather than one per frame for as long as it stays published.
    ssg::app::SystemClipboardWriter clipboardWriter;
    std::vector<ssg::PaletteCandidate> candidates;

    // Lever 3 per-drain snapshot coalescing. `refresh()` is expensive; instead
    // of taking a fresh snapshot before every buffered event, the coalescer
    // tracks whether the routing state (what a key/paste reads) or the geometry
    // (what a pointer/wheel hit-tests) has been dirtied since the last snapshot,
    // and the loop refreshes lazily only before an event that consumes a dirty
    // axis. Every dispatch merges its authoritative DispatchEffects here;
    // host-local picker mutations mark it dirty. It lives at function scope so
    // the dispatch lambdas below can update it.
    ssg::app::SnapshotCoalescer coalescer;
    auto noteEffects = [&](ssg::DispatchEffects effects) {
        coalescer.noteEffects(effects);
    };
    // A host-local picker mutation (query narrowing, selection move, window
    // scroll) changes the PaletteReport that buildReport() feeds the next
    // snapshot, so it dirties both axes exactly as a runtime dispatch would.
    auto markPickerDirty = [&] { coalescer.markPickerDirty(); };

    auto dispatch = [&](std::string_view id, std::any payload = {}) {
        noteEffects(runtime
                        .dispatch(client, {std::string{id}, runtime.revision(),
                                           std::move(payload)})
                        .effects);
    };
    // The keystroke path's dispatch: the command is already identified, so no
    // name is constructed, hashed or compared.
    auto dispatchHandle = [&](ssg::CommandName const& command,
                              std::any payload = {}) {
        noteEffects(
            runtime.dispatch(client, {command, runtime.revision(), std::move(payload)})
                .effects);
    };
    // Re-center the client-owned palette window on the current selection
    // (keep-visible). Called ONLY when the selection changes (arrow navigation,
    // open, type, backspace); the per-frame build_report otherwise honors the
    // free offset so a wheel scroll persists. Mirrors
    // the tree's reveal_tree_selection.
    auto revealPaletteSelection = [&] {
        markPickerDirty();
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        if (picker.selected >= order.size()) {
            picker.selected = order.empty() ? 0 : order.size() - 1;
        }
        std::optional<std::uint32_t> selected =
            order.empty() ? std::nullopt
                          : std::optional<std::uint32_t>{
                                static_cast<std::uint32_t>(picker.selected)};
        if (!selected) return;
        ssg::ScrollOffset offset{picker.firstVisible};
        offset.revealSelection(*selected,
                               static_cast<std::uint32_t>(order.size()),
                               picker.paneRows);
        picker.firstVisible = offset.firstVisible();
    };
    // Scroll the client-owned palette window by `delta` rows WITHOUT moving the
    // selection (a wheel over the open palette). The clamp-and-shift is the same
    // ScrollOffset the editor and tree use; only the ownership differs.
    auto scrollPalette = [&](std::int64_t delta) {
        if (!pickerOpen) return;
        markPickerDirty();
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        ssg::ScrollOffset offset{picker.firstVisible};
        offset.byLines(delta, static_cast<std::uint32_t>(order.size()),
                       picker.paneRows);
        picker.firstVisible = offset.firstVisible();
    };
    // Position the client-owned palette window along its track, as from a gutter
    // click or thumb drag. The picker's counterpart to view.scroll_to_fraction,
    // kept client-local for the same latency reason its ranking is.
    auto scrollPaletteToFraction = [&](std::uint32_t numerator,
                                       std::uint32_t denominator) {
        if (!pickerOpen) return;
        markPickerDirty();
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        ssg::ScrollOffset offset{picker.firstVisible};
        offset.toFraction(numerator, denominator,
                          static_cast<std::uint32_t>(order.size()),
                          picker.paneRows);
        picker.firstVisible = offset.firstVisible();
    };
    // Submit routes to the open picker's command, chosen from the mode the
    // server published rather than assumed: a candidate id means different
    // things per picker (a command id for the command palette), so a single
    // hardcoded submit would silently misinterpret another picker's ids.
    auto submitSelectedCandidate = [&] {
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        if (order.empty() || picker.selected >= order.size()) return;
        auto const& id = candidates[order[picker.selected]].id;
        if (auto const submit = ssg::paletteSubmitCommand(pickerMode, id)) {
            dispatch(submit->command.name(), submit->payload);
        }
    };
    auto applyClientOwnedInput = [&](ssg::ClientOwnedInput const& input) {
        switch (input.kind) {
        case ssg::ClientOwnedInputKind::AppendText:
            picker.query += input.text;
            picker.selected = 0;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::DeleteGraphemeBackward:
        case ssg::ClientOwnedInputKind::DeleteWordBackward:
            popCodePoint(picker.query);
            picker.selected = 0;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::SelectNext:
            ++picker.selected;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::SelectPrevious:
            if (picker.selected > 0) --picker.selected;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::Submit:
            submitSelectedCandidate();
            break;
        }
    };
    auto routeInput = [&](ssg::KeyStroke stroke, std::string text) {
        auto result = runtime.input(client, {stroke, std::move(text)});
        if (result.clientOwned) applyClientOwnedInput(*result.clientOwned);
        return result.outcome;
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
    // Take a fresh snapshot and adopt its authoritative client state. Called
    // before every input event so coalesced input after a focus-changing command
    // routes against the new focus rather than a stale one.
    auto refresh = [&]() -> std::optional<ssg::SessionSnapshot> {
        auto snapshot = runtime.present(client, terminalSize(), buildReport());
        if (snapshot) {
            focus = effectiveFocusFromSections(snapshot->sections());
            candidates = snapshot->sections().palette.candidates;
            pickerMode = snapshot->sections().palette.mode;
            // Cache the palette pane height for the next window computation: the
            // palette pane is the editor pane, so this is populated every frame,
            // including before the palette opens (no cold start). The window is
            // computed from the PREVIOUS frame's height, so a terminal resize
            // lags one frame before keep-visible re-settles — the same one-frame
            // clamp the editor's server-side scroll offset already has, and it
            // self-corrects on the next snapshot.
            auto const& shell = snapshot->presentation()->shell;
            if (!shell.panes.empty()) {
                picker.paneRows = static_cast<std::uint32_t>(
                    std::max(shell.panes.front().content.height, 1));
            }
            // A copy or cut offers its text for the SYSTEM clipboard.  Serve it
            // with OSC 52, which over SSH is the only way the remote editor can
            // reach the local clipboard at all.  Fire and forget: keyed by id so
            // one copy is written once, with nothing reported back, and skipped
            // entirely when the terminal did not advertise the capability --
            // where it would be an unrecognised sequence rather than a copy.
            if (auto const bytes = clipboardWriter.bytesFor(
                    snapshot->sections().clipboard.systemWrite,
                    capabilities.has(ssg::app::Capability::ClipboardWrite))) {
                writeAll(*bytes);
            }
            // Derive find fulfillment from the ACTIVE prompt kind, not merely the
            // controller being open under prompt focus: a palette/settings prompt
            // may be active while the find controller is still open, and find
            // fulfillment must not hijack that unrelated prompt's keys.
            auto const& findView = snapshot->sections().findReplace;
            // The AUTHORITATIVE active-prompt kind: present even for a
            // header-hosted prompt (palette / file
            // finder) whose query renders in the header input line and so
            // produces no footer `prompt` layout view. Deriving the open-flags
            // from this -- rather than from `promptStatus.prompt->kind`, which is
            // nullopt for a header-hosted prompt -- is what lets typed text reach
            // the picker query.
            auto const activeKind = snapshot->sections().promptStatus.activeKind;
            auto const& activePrompt = snapshot->presentation()->prompt;
            bool const wasPickerOpen = pickerOpen;
            pickerOpen = activeKind == ssg::PromptKind::Palette;
            if (pickerOpen && !wasPickerOpen) {
                picker.query.clear();
                picker.selected = 0;
                picker.firstVisible = 0;
            }
            bool const findPromptActive = activeKind == ssg::PromptKind::Find;
            findOpen = findView.open && findPromptActive;
            findQuery = findView.query;
            // The replace prompt edits the replacement, not the query.
            bool const replacePromptActive =
                activeKind == ssg::PromptKind::Replace;
            replaceOpen = findView.open && replacePromptActive;
            replaceReplacement = findView.replacement;
            // A single-line text-entry prompt (save-as/open path, a
            // CommandArgument prompt such as go-to-line, or the settings query)
            // keeps its authoritative value in the library, which publishes it in
            // the prompt view. These all collect edits through prompt.update_value
            // on input index 0 rather than a dedicated controller (unlike
            // find/replace/palette). Mirroring the value here (rather than a
            // client-side copy) means the two cannot drift when the library
            // rewrites it -- a rejected save-as, say.
            textPromptOpen =
                activeKind == ssg::PromptKind::Path ||
                activeKind == ssg::PromptKind::CommandArgument ||
                activeKind == ssg::PromptKind::Settings;
            textPromptValue.clear();
            if (textPromptOpen && activePrompt && !activePrompt->controls.empty()) {
                textPromptValue = activePrompt->controls.front().value;
            }
        }
        return snapshot;
    };

    // Top-level boundary (M9-X): an exception escaping the loop is not portably
    // guaranteed to unwind `mode` once past main, so restore the terminal here
    // before it propagates.
    bool firstFrameMarked = false;
    auto lastFrameAt = std::chrono::steady_clock::time_point{};
    // Borrowed by Renderer::render to reuse shaped on-screen lines across frames.
    ssg::LineLayoutCache renderLineCache;
    try {
        while (!quit) {
            // Render-rate cap while a drag is held: a scrollbar or selection drag
            // floods motion events, and rendering a full frame per event pins a
            // core. When a drag is active and the previous frame was less than one
            // drag-frame interval ago, wait out the remainder so the queued motion
            // events accumulate (and coalesce) into a single frame instead of many.
            if (draggingGutter.has_value() || dragging) {
                const auto sinceFrame =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - lastFrameAt)
                        .count();
                if (sinceFrame < kDragFrameIntervalMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{
                        kDragFrameIntervalMs - sinceFrame});
                }
            }
            lastFrameAt = std::chrono::steady_clock::now();
            auto snapshot = refresh();
            // The loop-top snapshot is fresh, so nothing is dirty until an event
            // in this drain mutates state.
            coalescer.noteRefreshed();
            if (snapshot) {
                // The library renders every screen branch, including the declined-
                // layout "too small" placeholder (M11-L); the app only encodes.
                auto grid = ssg::Renderer{}.render(*snapshot, &renderLineCache);
                std::string frame = ssg::app::encode_frame(
                    grid, colorDepth, !draggingGutter.has_value());
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
                    // Open the Files sidebar only when startup did NOT open a
                    // named file: with nothing but an empty buffer the sidebar is
                    // the only thing worth showing, but over a file the user
                    // asked for by name it just covers the text they came to
                    // edit.  Uses the SAME panel.show_files command Escape-B and
                    // Escape-O reach interactively; this is not a new mechanism,
                    // only sequenced AFTER primeDeferred() rather than before it:
                    // dispatching it earlier (pre-loop, before the deferred tree
                    // scan has run) would fail with "files tree provider is
                    // unavailable" every startup, since the M10 fast-startup path
                    // defers registering that provider until exactly this point.
                    if (!openedNamedFile) {
                        if (auto const panelResult = runtime.dispatch(
                                client, {"panel.show_files", runtime.revision(), {}});
                            !panelResult.accepted()) {
                            std::fprintf(stderr,
                                         "ssg: could not open Files sidebar: %s\n",
                                         panelResult.message.c_str());
                        }
                    }
                    // panel.show_files moves focus to the panel as a side effect
                    // (ShellState::showPanelProvider -> togglePanel), so editor
                    // focus is re-asserted here, AFTER it, to be the final word.
                    if (startsWithAnEditableDocument) {
                        runtime.focusEditor();
                    }
                    continue;
                }
                writeAll(frame);
            }

            // A read buffer large enough to pull a whole burst of terminal input
            // (a mouse-drag motion flood, or a paste) in one read, so the inner
            // decode loop can coalesce a run of drag events into a single frame
            // instead of rendering once per 64-byte chunk.
            char bytes[4096];
            // Edge auto-scroll (M8-S2): if a drag is held past the top/bottom of the
            // editor content, don't block indefinitely on input — wake on a timer to
            // scroll one line and re-extend the selection to the new edge cell, so a
            // drag held still at the edge keeps scrolling and selecting.
            std::optional<int> dragEdge;
            if (dragging && snapshot && !snapshot->presentation()->shell.panes.empty()) {
                dragEdge = ssg::app::edge_scroll(
                    dragging, lastPointerRow,
                    snapshot->presentation()->shell.panes.front().content);
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
                        !scrolled->presentation()->shell.panes.empty()) {
                        auto const content = scrolled->presentation()->shell.panes.front().content;
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
                                if (altDrag) {
                                    std::vector<ssg::Selection> ranges =
                                        altDragBaseline;
                                    ranges.push_back(
                                        ssg::Selection{*dragAnchor, *active});
                                    dispatch("select.set_ranges",
                                             ssg::SelectionCommandArguments{
                                                 std::nullopt, std::nullopt,
                                                 std::move(ranges)});
                                } else {
                                    dispatch("select.set_range",
                                             ssg::SelectionCommandArguments{
                                                 std::nullopt,
                                                 ssg::Selection{*dragAnchor,
                                                                *active}});
                                }
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
            // Block until a real event (input / signal / background wake), but
            // wake on the autosave cadence to flush due drafts. A pure-timeout
            // tick that writes nothing must NOT repaint (encode_frame is a full
            // frame, so an idle repaint every second is wasteful) — keep waiting
            // without re-rendering. Only a flush that actually wrote something
            // falls through to re-render, since it may change a recovery badge.
            FdReadiness wait;
            while (true) {
                wait = waitReadiness(
                    kAutosaveTickMs, signalPipe[0], gitDiffWakeFd,
                    initScriptWatcher ? initScriptWatcher->wakeDescriptor() : -1);
                if (wait.input || wait.signal || wait.gitDiff || wait.initScript) {
                    break;
                }
                if (runtime.flushDueAutosaveDrafts() > 0) break;
            }
            if (wait.signal) {
                // Terminate does not return (restore + re-raise); a resize just
                // re-snapshots at the loop top.
                drainSignals();
                if (!wait.input) continue;
            }
            if (wait.initScript) {
                // Worker completion is accepted only by EditorRuntime::pump();
                // nothing else drains this -- evaluate the reloaded script here,
                // on the main thread, exactly like startup's loadInitScript.
                initScriptWatcher->drainAndEvaluate(scripts, runtime);
                if (!wait.input) continue;
            }
            if (wait.gitDiff) {
                (void)runtime.pump();
                if (!wait.input) continue;
            }
            if (!wait.input) continue;  // Wake-only cycle: re-render and retry.
            auto readBytes = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (readBytes <= 0) break;
            buffer.append(bytes, static_cast<std::size_t>(readBytes));
            logRawInput(bytes, static_cast<std::size_t>(readBytes));

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

            // A terminal report answering a startup query, not something the user
            // typed.  It is consumed either way; whether it is believed is the
            // capability object's decision, not this loop's.
            if (decoded.status == ssg::app::DecodeStatus::reply) {
                capabilities.observeReply(decoded.reply);
                // The moment the terminal confirms the keyboard protocol, enable
                // it (latched).  Gated solely on the capability -- a terminal that
                // never answers never gets the enable, which is the whole escape
                // hatch (INV-capability-single-source).
                if (capabilities.has(ssg::app::Capability::KeyboardProtocol)) {
                    mode.enableKeyboardProtocol();
                }
                continue;
            }

            // Coalesce a burst of pointer drag events: only the final position
            // matters for a scrollbar drag or a drag-select, and taking a full
            // snapshot and encoding a full frame for every motion event the
            // terminal emits at motion rate spikes CPU. If this is a drag and
            // another pointer event is already buffered behind it, drop this one;
            // the last drag in the run is processed normally, so the end position
            // is honoured without the intermediate work.
            if (decoded.status == ssg::app::DecodeStatus::pointer &&
                decoded.pointer.kind == ssg::app::PointerKind::drag) {
                std::size_t peekConsumed = 0;
                const auto next =
                    ssg::app::decode_input(buffer, false, peekConsumed);
                if (next.status == ssg::app::DecodeStatus::pointer &&
                    next.pointer.kind == ssg::app::PointerKind::drag) {
                    continue;
                }
            }

            // Per-drain snapshot coalescing (Lever 3): a fresh snapshot before
            // this event is needed only when the event consumes an axis that an
            // earlier event in this drain dirtied. A key/paste routes by the
            // interaction routing state; a pointer/wheel hit-tests geometry;
            // reply/none/incomplete consume neither. A pure keyboard burst that
            // changes only geometry (cursor moves) therefore refreshes nothing.
            // A refresh rebuilds the whole snapshot, clearing both axes.
            const auto consumes = ssg::app::consumed_axes(decoded.status);
            if (!firstEvent && coalescer.needsRefresh(consumes)) {
                snapshot = refresh();
                coalescer.noteRefreshed();
            }
            firstEvent = false;

            if (decoded.status == ssg::app::DecodeStatus::pointer) {
                lastPointerColumn = decoded.pointer.column;
                lastPointerRow = decoded.pointer.row;
                // Classify the cell via the library hit_test, resolve the target
                // the hit needs, then let the pure route_pointer decide the
                // command sequence and drag-state change.
                ssg::RegionHit hit;
                ssg::app::PointerTargets targets;
                std::optional<ssg::DocumentPosition> doubleClickPosition;
                if (snapshot) {
                    ssg::HitTester tester{*snapshot};
                    if (draggingGutter &&
                        decoded.pointer.kind == ssg::app::PointerKind::drag) {
                        // Continue the drag using the offset captured at press:
                        // the grabbed point of the thumb tracks the cursor.  The
                        // pointer may wander off the one-column gutter
                        // horizontally without dropping the drag, so this keys off
                        // the held region, never a fresh column hit-test.
                        hit = ssg::RegionHit{};
                        hit.region = draggingGutter->region;
                        int const rel =
                            decoded.pointer.row - draggingGutter->gutterY;
                        auto const fraction = ssg::app::gutter_fraction(
                            rel, draggingGutter->grabOffset,
                            draggingGutter->travel);
                        hit.scrollNumerator = fraction.numerator;
                        hit.scrollDenominator = fraction.denominator;
                    } else {
                        hit = tester.at(decoded.pointer.column,
                                        decoded.pointer.row);
                        // A left press on a gutter grabs the thumb: capture the
                        // grab offset and replace the raw absolute-row fraction
                        // with the grab fraction, so the press and the drag that
                        // follows compute the position the same way.
                        const bool leftPress =
                            decoded.pointer.kind == ssg::app::PointerKind::press &&
                            decoded.pointer.button == ssg::app::PointerButton::left;
                        if (leftPress &&
                            ssg::app::is_scrollbar_region(hit.region)) {
                            if (auto const thumb = tester.gutterThumb(hit.region)) {
                                int const rel = decoded.pointer.row - thumb->gutterY;
                                int const travel =
                                    static_cast<int>(thumb->viewportRows) -
                                    static_cast<int>(thumb->thumbSize);
                                int const grabOffset =
                                    ssg::app::scrollbar_grab_offset(
                                        rel, static_cast<int>(thumb->thumbStart),
                                        static_cast<int>(thumb->thumbSize));
                                draggingGutter = GutterDrag{hit.region,
                                                            thumb->gutterY, travel,
                                                            grabOffset};
                                auto const fraction = ssg::app::gutter_fraction(
                                    rel, grabOffset, travel);
                                hit.scrollNumerator = fraction.numerator;
                                hit.scrollDenominator = fraction.denominator;
                            }
                        } else if (leftPress) {
                            draggingGutter.reset();
                        }
                    }
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
                               hit.region == ssg::HitRegion::FooterField ||
                               hit.region == ssg::HitRegion::StatusAction) {
                        targets.field_command_id = hit.commandId;
                        targets.status_invocation = hit.statusInvocation;
                    } else if (hit.region == ssg::HitRegion::ExternalAction &&
                               hit.externalFileId && hit.commandId) {
                        // Resolve the runtime-minted file id into the select-then-act
                        // targets the router dispatches (external.select then the
                        // payload-less action command).
                        targets.external_file_id =
                            ssg::DiffFileId{*hit.externalFileId};
                        targets.external_action_command = hit.commandId;
                    }
                    // A second left click on the same editor cell within the
                    // window selects the word there instead of just placing the
                    // caret. Detected here so the
                    // word geometry stays server-side; the client only recognizes
                    // the gesture. The active tab id keys the tracker so a fast
                    // click on the same cell of a DIFFERENT document does not pair.
                    const bool leftEditorPress =
                        decoded.pointer.kind == ssg::app::PointerKind::press &&
                        decoded.pointer.button == ssg::app::PointerButton::left &&
                        hit.region == ssg::HitRegion::Editor;
                    if (leftEditorPress && targets.document_position &&
                        ssg::app::register_click_is_double(
                            clickTracker, std::chrono::steady_clock::now(),
                            snapshot->sections().tabs.active
                                ? snapshot->sections().tabs.active->value()
                                : 0,
                            decoded.pointer.row, decoded.pointer.column,
                            kDoubleClickWindow)) {
                        doubleClickPosition = targets.document_position;
                    }
                }
                // A recognized double-click selects the word (no drag); every
                // other press goes through the normal router. Both yield a
                // PointerDispatch handled uniformly below. The router decides the
                // command from the EFFECTIVE Alt: a press establishes the gesture
                // from its own Alt bit; a drag/release reuses the established
                // altDrag so a dropped modifier mid-drag cannot flip it.
                bool const effectiveAlt =
                    decoded.pointer.kind == ssg::app::PointerKind::press
                        ? decoded.pointer.alt
                        : altDrag;
                // Capture the baseline BEFORE dispatching the add so it excludes
                // the caret this press is about to add.
                if (!doubleClickPosition &&
                    decoded.pointer.kind == ssg::app::PointerKind::press &&
                    effectiveAlt && hit.region == ssg::HitRegion::Editor &&
                    targets.document_position && snapshot) {
                    auto const& items =
                        snapshot->sections().selection.items();
                    altDragBaseline.assign(items.begin(), items.end());
                }
                auto plan =
                    doubleClickPosition
                        ? ssg::app::double_click_dispatch(*doubleClickPosition)
                        : ssg::app::route_pointer(
                              hit, decoded.pointer.button, decoded.pointer.kind,
                              effectiveAlt, dragging, dragAnchor, targets,
                              altDragBaseline);
                bool previousAccepted = true;
                for (auto const& command : plan.commands) {
                    if (command.gate_on_previous && !previousAccepted) {
                        continue;
                    }
                    auto const pointerResult =
                        runtime.dispatch(client, {command.command_id,
                                                  runtime.revision(), command.payload});
                    noteEffects(pointerResult.effects);
                    previousAccepted = pointerResult.accepted();
                }
                // A gutter gesture on a client-owned surface has no command to
                // dispatch (the picker's offset must not round-trip), so the
                // loop applies it here. The switch is exhaustive over the
                // targets a client-owned descriptor can name, so adding one
                // without handling it is a visible gap rather than a silently
                // dropped gesture.
                if (plan.client_scroll) {
                    switch (plan.client_scroll->target) {
                        case ssg::app::WheelTarget::palette:
                            scrollPaletteToFraction(
                                plan.client_scroll->numerator,
                                plan.client_scroll->denominator);
                            break;
                        case ssg::app::WheelTarget::editor:
                        case ssg::app::WheelTarget::tree:
                        case ssg::app::WheelTarget::none:
                            // Server-owned surfaces dispatch a command instead
                            // and never reach here; `none` is not scrollable.
                            break;
                    }
                }
                if (plan.begins_drag) {
                    dragging = true;
                    dragAnchor = targets.document_position;
                    altDrag = effectiveAlt;
                    if (!altDrag) altDragBaseline.clear();
                }
                // A gutter thumb drag ends on any release; the press that starts
                // it, and the grab offset it captures, are handled where `hit` is
                // classified above.
                if (decoded.pointer.kind == ssg::app::PointerKind::release) {
                    draggingGutter.reset();
                }
                if (plan.ends_drag) {
                    dragging = false;
                    dragAnchor.reset();
                    altDrag = false;
                    altDragBaseline.clear();
                }
                continue;
            }

            if (decoded.status == ssg::app::DecodeStatus::paste) {
                // Pasted bytes are CONTENT.  They go straight to the text sink
                // without touching the keymap, so a newline in the paste cannot
                // fire whatever Enter is bound to and an escape sequence in it
                // cannot be obeyed.
                if (!decoded.text.empty()) {
                    (void)routeInput(ssg::KeyStroke{}, decoded.text);
                }
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
                continue;
            }
            if (decoded.status != ssg::app::DecodeStatus::key) {
                // A recognized but unhandled byte (unknown CSI, stray control).
                continue;
            }

            auto const outcome = routeInput(decoded.stroke, decoded.text);
            if (outcome == ssg::ClientInputOutcome::Unhandled) {
                // Quit is process lifecycle, not editor behavior.
                auto const& stroke = decoded.stroke;
                if (stroke.code == ssg::KeyCode::KeyQ && stroke.alt &&
                    !stroke.control) {
                    quit = true;
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

    // Clean exit (the loop quit without an exception): best-effort final flush of
    // every dirty draft, capturing edits newer than the last autosave tick. This
    // does NOT run on SIGKILL or a crash — those rely on the last debounced tick
    runtime.flushAllAutosaveDrafts();

    return 0;
}
