#include "runtime/editor_session_internal.h"

#include <ssg/CommandCatalog.h>
#include <ssg/DraftReopenClassifier.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/GitMetadataWatcher.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Style.h>
#include <ssg/WholeScreenAssembly.h>
#include <ssg/platform_files.h>

#include <algorithm>
#include <array>
#include <span>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace ssg {
namespace {

// First-frame syntax should be ready when it's cheap: parsing a small file is
// comfortably within startup budget, while multi-MB input can exceed it and is
// deferred until primeDeferred().
constexpr std::size_t kEagerSyntaxMaxBytes = 2 * 1024 * 1024;
constexpr auto kGitDiffPollInterval = std::chrono::milliseconds{250};
constexpr auto kGitMetadataWatchPollInterval = std::chrono::milliseconds{100};
constexpr auto kGitDiffRetryDelay = std::chrono::milliseconds{1000};
// Event mode refreshes instantly on watch events; this long-interval full-refresh
// backstop bounds the staleness of anything the watcher cannot observe -- external
// git operations, a linked worktree's metadata outside the tree, dropped events on
// a network filesystem -- without re-scanning at the Poll cadence. Much larger than
// kGitDiffPollInterval so idle CPU is a small fraction of Poll's.
constexpr auto kGitDiffEventRecoveryInterval = std::chrono::minutes{1};

bool setNonBlocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

GitTreeStatus gitTreeStatusForScanFile(const GitDiffScanFile& file) {
    if (file.previousPath) return GitTreeStatus::Renamed;
    if (!file.workingContent) return GitTreeStatus::Deleted;
    if (!file.baselineContent) return GitTreeStatus::Added;
    return GitTreeStatus::Modified;
}

std::vector<GitTreeRecord> gitTreeRecordsFromScan(
    const std::vector<GitDiffScanFile>& files) {
    std::vector<GitTreeRecord> records;
    records.reserve(files.size());
    for (const auto& file : files) {
        records.push_back(
            {.workspacePath = file.path.generic_string(),
             .label = file.path.generic_string(),
             .status = gitTreeStatusForScanFile(file),
             .commands = {}});
    }
    return records;
}

std::string liveDiffTabLabelForPath(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.empty() ? path.generic_string() : filename;
}

// The user's home directory for the header path field's "~" abbreviation.
// HOME first (POSIX), then USERPROFILE (Windows); trailing separators are
// stripped so a home value like "/home/user/" still matches "/home/user/repo".
// Empty when unknown, which disables the abbreviation.
std::string resolveHomeDirectory() {
    std::string home;
    if (const char* value = std::getenv("HOME"); value != nullptr) {
        home = value;
    } else if (const char* profile = std::getenv("USERPROFILE");
               profile != nullptr) {
        home = profile;
    }
    while (!home.empty() && (home.back() == '/' || home.back() == '\\')) {
        home.pop_back();
    }
    // The path field compares against workspaceRoot.generic_string() ('/'
    // separators on every platform), so normalize a Windows USERPROFILE's
    // backslashes to match.
    for (auto& ch : home) {
        if (ch == '\\') ch = '/';
    }
    return home;
}

std::string liveDiffDocumentText(const DiffFileView& file) {
    // A deleted file's whole content is represented as Removed phantom rows
    // (see Viewport.cpp's removedBlocks/phantom-row projection), never as
    // real document text -- currentContent is already empty for a deleted
    // file (DiffModel::updateGitFile sets it from workingContent, which is
    // absent when deleted). Synthesizing baseline content as the "current"
    // text here would duplicate every removed line: once as a real row from
    // this text, and again as the phantom row the viewport already inserts
    // for the same baseline line.
    return file.currentContent;
}

// The curated terminal runtime keymap: a small set of
// argument-free bindings the TUI drives, plus the context-divergent navigation
// keys.  Only argument-free-usable commands are bound (a bare stroke dispatches
// with no payload); exhaustive reachability is the palette's job.  Every binding
// is a single stroke: global (*) actions are Alt chords, navigation differs per
// focus, and Escape is a plain cancel.
KeymapViewState defaultTerminalKeymap() {
    auto seq = [](std::initializer_list<std::string_view> strokes) {
        auto parsed = KeyCodec{}.parseSequence(strokes);
        if (!parsed) throw std::logic_error{"curated keymap has an invalid stroke"};
        return *parsed;
    };
    KeymapViewState keymap{"default", {}};
    auto bind = [&](KeySequence sequence, std::string command,
                    std::string context) {
        keymap.bindings.push_back(
            {std::move(sequence), std::move(command), std::move(context)});
    };

    // Frequent actions are single Alt+<key> chords.  In a terminal Alt+X
    // transmits as the bytes ESC X, which decode_input coalesces into one
    // alt=true stroke, so these are the same keys the user already presses -- the
    // former Escape leader is gone, and Escape is now a plain cancel key.
    bind(seq({"Alt+KeyS"}), "file.save", "*");
    bind(seq({"Alt+KeyN"}), "file.new", "*");
    bind(seq({"Alt+KeyZ"}), "edit.undo", "*");
    bind(seq({"Alt+Shift+KeyZ"}), "edit.redo", "*");
    // Alt+p opens the file picker (the frequent action) and Alt+Shift+P the
    // command palette, matching the convention users arrive with.
    bind(seq({"Alt+KeyP"}), "file_finder.open", "*");
    bind(seq({"Alt+Shift+KeyP"}), "palette.open", "*");
    bind(seq({"Alt+KeyB"}), "panel.toggle", "*");
    bind(seq({"Alt+KeyH"}), "help.open", "*");
    bind(seq({"Alt+KeyO"}), "panel.focus", "*");
    // Tab cycling: Alt+BracketRight/Left cannot be used -- ESC ] / ESC [ are the
    // OSC / CSI introducers -- so the brackets give way to Alt+Period/Comma.
    bind(seq({"Alt+Period"}), "tab.next", "*");
    bind(seq({"Alt+Comma"}), "tab.previous", "*");
    bind(seq({"Alt+KeyW"}), "tab.close", "*");
    // The Settings escape hatch (protected: a settings.open binding must always
    // exist) moves from the former three-stroke chord to a single Alt+Shift+T.
    bind(seq({"Alt+Shift+KeyT"}), "settings.open", "*");
    bind(seq({"Alt+KeyA"}), "select.all", "*");
    bind(seq({"Alt+KeyD"}), "select.add_next_occurrence", "*");
    bind(seq({"Alt+KeyI"}), "select.split_into_lines", "*");
    bind(seq({"Alt+KeyK"}), "select.add_cursor_up", "*");
    bind(seq({"Alt+KeyJ"}), "select.add_cursor_down", "*");
    bind(seq({"Alt+Slash"}), "find.open", "*");
    // Alt+8 seeds find with the word under the caret.  Editor-context: it acts on
    // the caret and document.
    bind(seq({"Alt+Digit8"}), "find.word_under_cursor", "editor");
    bind(seq({"Alt+KeyR"}), "replace.open", "*");
    // Draft recovery's "Use disk": discard unsaved edits back to the disk
    // version (the draft is archived first, so this is reversible).
    bind(seq({"Alt+Shift+KeyD"}), "draft.discard", "editor");

    // Cut/copy/paste act on the editor's selection, so they are bound in the
    // editor context; paste is additionally bound in the prompt so a prompt's
    // value can be pasted into.
    bind(seq({"Alt+KeyX"}), "clipboard.cut", "editor");
    bind(seq({"Alt+KeyC"}), "clipboard.copy", "editor");
    bind(seq({"Alt+KeyV"}), "clipboard.paste", "editor");
    // Paste also works while a prompt owns the keyboard -- find, replace, a path,
    // the palette query.  The client fulfils it against the prompt's own text
    // rather than the document, the same way typing into a prompt is routed.
    // Cut and copy are deliberately absent: a prompt's value is client-owned and
    // there is no selection within it to take.
    bind(seq({"Alt+KeyV"}), "clipboard.paste", "prompt");

    bind(seq({"ArrowDown"}), "cursor.line_down", "editor");
    bind(seq({"ArrowUp"}), "cursor.line_up", "editor");
    bind(seq({"ArrowLeft"}), "cursor.left", "editor");
    bind(seq({"ArrowRight"}), "cursor.right", "editor");
    bind(seq({"Shift+ArrowLeft"}), "select.left", "editor");
    bind(seq({"Shift+ArrowRight"}), "select.right", "editor");
    bind(seq({"Shift+ArrowUp"}), "select.line_up", "editor");
    bind(seq({"Shift+ArrowDown"}), "select.line_down", "editor");
    bind(seq({"Home"}), "cursor.line_start", "editor");
    bind(seq({"End"}), "cursor.line_end", "editor");
    bind(seq({"Shift+Home"}), "select.line_start", "editor");
    bind(seq({"Shift+End"}), "select.line_end", "editor");
    bind(seq({"Ctrl+Home"}), "cursor.document_start", "editor");
    bind(seq({"Ctrl+End"}), "cursor.document_end", "editor");
    // Alt+Home/End also jump to the document extremes: the physical Home/End keys
    // are natural for "top/bottom of file", and Alt is the modifier the rest of
    // the editor uses.
    bind(seq({"Alt+Home"}), "cursor.document_start", "editor");
    bind(seq({"Alt+End"}), "cursor.document_end", "editor");
    // Alt+Shift+G opens a prompt for a line number and jumps there (clamped).
    bind(seq({"Alt+Shift+KeyG"}), "goto.line", "editor");
    bind(seq({"Ctrl+Shift+Home"}), "select.document_start", "editor");
    bind(seq({"Ctrl+Shift+End"}), "select.document_end", "editor");
    bind(seq({"PageUp"}), "cursor.page_up", "editor");
    bind(seq({"PageDown"}), "cursor.page_down", "editor");
    bind(seq({"Shift+PageUp"}), "select.page_up", "editor");
    bind(seq({"Shift+PageDown"}), "select.page_down", "editor");
    bind(seq({"Enter"}), "text.newline", "editor");
    bind(seq({"Backspace"}), "text.delete_backward", "editor");
    bind(seq({"Delete"}), "text.delete_forward", "editor");
    // Alt+Backspace deletes the word to the left.  Alt+Backspace transmits as
    // the bytes ESC 0x7f, which decode_input coalesces into one Alt+Backspace
    // stroke.
    bind(seq({"Alt+Backspace"}), "text.delete_word_backward", "editor");
    // Word navigation: Alt+Left/Right (and Shift to extend).  Arrow keys use the
    // CSI modifier-parameter form, which decode_input parses into a single
    // alt=true stroke.
    bind(seq({"Alt+ArrowLeft"}), "cursor.word_left", "editor");
    bind(seq({"Alt+ArrowRight"}), "cursor.word_right", "editor");
    bind(seq({"Alt+Shift+ArrowLeft"}), "select.word_left", "editor");
    bind(seq({"Alt+Shift+ArrowRight"}), "select.word_right", "editor");

    bind(seq({"ArrowDown"}), "tree.select_next", "panel");
    bind(seq({"ArrowUp"}), "tree.select_previous", "panel");
    bind(seq({"Enter"}), "tree.activate", "panel");

    bind(seq({"Enter"}), "prompt.submit", "prompt");
    // A single Escape cancels a focused prompt (find, replace, path, palette).
    // With the leader gone Escape is no longer a chord prefix, so one press is
    // unambiguous.
    bind(seq({"Escape"}), "prompt.cancel", "prompt");
    bind(seq({"ArrowDown"}), "prompt.next", "prompt");
    bind(seq({"ArrowUp"}), "prompt.previous", "prompt");
    // Tab advances the keyboard among a multi-input prompt's inputs (replace's
    // query and replacement); a single-input prompt stays put.
    bind(seq({"Tab"}), "prompt.focus_next_control", "prompt");
    // The find/replace option toggles (find.toggle_case/whole_word/regex,
    // replace.all) are reachable through the command palette; they do not earn a
    // dedicated key and are left unbound.

    // The external-modification bar. Alt+E focuses it from any state (global,
    // present-gated by the command); within the external context the arrows move
    // the selection and Enter/K/D run the offered action on it, mirroring the
    // panel's navigation, and Escape returns focus without touching prompt
    // lifecycle.
    bind(seq({"Alt+KeyE"}), "external.focus", "*");
    bind(seq({"ArrowDown"}), "external.select_next", "external");
    bind(seq({"ArrowUp"}), "external.select_previous", "external");
    bind(seq({"Enter"}), "external.reload", "external");
    bind(seq({"KeyK"}), "external.keep_buffer", "external");
    bind(seq({"KeyD"}), "external.open_diff", "external");
    bind(seq({"Escape"}), "external.focus_return", "external");

    return keymap;
}

DocumentPosition zeroPosition() {
    return {ByteOffset{0}, LineIndex{0}, CellIndex{0}};
}

SelectionViewState initialSelection() {
    auto zero = zeroPosition();
    return {SelectionSet{std::vector<Selection>{Selection{zero, zero}}}, 0, 0, std::nullopt};
}

std::filesystem::path canonicalDirectory(std::filesystem::path const& path) {
    std::error_code code;
    auto canonical = std::filesystem::canonical(path, code);
    if (code || !std::filesystem::is_directory(canonical)) {
        throw std::invalid_argument{"workspace root must be an existing directory"};
    }
    return canonical;
}

std::span<const std::byte> asByteSpan(std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

LspWorkspaceFileResult asLspResult(FileIoResult result) {
    switch (result.status) {
        case FileIoStatus::Ok:
            return {};
        case FileIoStatus::NotFound:
            return {LspWorkspaceFileError::NotFound, std::move(result.message)};
        case FileIoStatus::AlreadyExists:
            return {LspWorkspaceFileError::AlreadyExists,
                    std::move(result.message)};
        case FileIoStatus::IoError:
            break;
    }
    return {LspWorkspaceFileError::IoError, std::move(result.message)};
}

// std::nullopt for a file that could not be read, so an unreadable file can
// never be mistaken for an empty one. That mistake is destructive here: these
// results feed staleness comparisons and rollback snapshots, where fake empty
// content would overwrite or restore nothing over something.
std::optional<std::string> readFileText(std::filesystem::path const& path) {
    auto result = readFile(path);
    if (!result.ok()) return std::nullopt;
    return std::string{reinterpret_cast<const char*>(result.bytes.data()),
                       result.bytes.size()};
}

std::size_t lineStartOffset(std::string_view text, std::size_t line) {
    std::size_t offset = 0;
    while (line > 0 && offset < text.size()) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) {
            return text.size();
        }
        offset = newline + 1;
        --line;
    }
    return offset;
}

std::unordered_map<std::uint64_t, Revision> documentRevisions(
    const Workspace& workspace) {
    std::unordered_map<std::uint64_t, Revision> revisions;
    for (auto const id : workspace.documents()) {
        revisions.emplace(id.value(), workspace.document(id).revision());
    }
    return revisions;
}

bool existingDocumentMutated(
    const std::unordered_map<std::uint64_t, Revision>& before,
    const Workspace& workspace) {
    for (auto const id : workspace.documents()) {
        const auto found = before.find(id.value());
        if (found == before.end()) {
            continue;
        }
        if (workspace.document(id).revision() != found->second) {
            return true;
        }
    }
    return false;
}

std::optional<std::filesystem::path> pathFromUri(std::string_view uri) {
    constexpr std::string_view prefix{"file://"};
    if (uri.rfind(prefix, 0) != 0) return std::nullopt;
    return std::filesystem::path{std::string{uri.substr(prefix.size())}};
}

std::string uriFromPath(std::filesystem::path const& path) {
    return "file://" + path.generic_string();
}

std::optional<std::string> relativeToRoot(std::filesystem::path const& root,
                                            std::filesystem::path const& path) {
    auto relative = path.lexically_relative(root);
    if (relative.empty()) return std::nullopt;
    for (auto const& part : relative) {
        if (part == "..") return std::nullopt;
    }
    return relative.generic_string();
}

bool pathContains(std::filesystem::path const& root,
                   std::filesystem::path const& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> workspaceChangePath(
    std::filesystem::path const& root, std::string_view rawPath,
    std::string& message) {
    auto supplied = std::filesystem::path{rawPath};
    if (rawPath.empty() || supplied.is_absolute()) {
        message = "workspace replacement path must be relative";
        return std::nullopt;
    }
    for (auto const& part : supplied) {
        if (part == "..") {
            message = "workspace replacement path must not traverse";
            return std::nullopt;
        }
    }
    std::error_code code;
    auto candidate = std::filesystem::weakly_canonical(root / supplied, code);
    if (code) {
        message = code.message();
        return std::nullopt;
    }
    if (!pathContains(root, candidate)) {
        message = "workspace replacement path resolves outside the workspace";
        return std::nullopt;
    }
    return candidate;
}

} // namespace

struct GitDiffRefreshWorkerState {
    explicit GitDiffRefreshWorkerState(const std::filesystem::path& rootPath)
        : repository{makePlatformGitRepository(rootPath)},
          source{sourceModel} {}

    std::unique_ptr<GitRepository> repository;
    DiffModel sourceModel;
    GitDiffSource source;
    std::unique_ptr<FilesystemWatcher> watcher;
    std::unique_ptr<GitMetadataWatcher> metadataWatcher;
    std::vector<std::filesystem::path> metadataDirectories;
    GitDiffMode mode = GitDiffMode::Poll;
    bool watcherAvailable = false;
    // Test hook: shortens the Event-mode backstop so its full refresh is
    // deterministically triggerable in a unit test. Unset uses kGitDiffBackstopInterval.
    std::optional<std::chrono::steady_clock::duration> backstopIntervalOverride;

    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool retryPending = false;
    std::chrono::steady_clock::time_point nextRetry =
        std::chrono::steady_clock::time_point::max();
    std::deque<GitDiffScan> pendingScans;
    // Normalized external-modification events queued in order for the runtime-thread
    // reconcile, beside pendingScans and woken by the same wake byte. The worker
    // never touches the flow itself; it only hands these across the thread boundary.
    std::deque<WatchEvent> pendingWatchEvents;
    // Set when the watcher reports an Overflow (event loss). The runtime-thread
    // drain consumes it and re-scans every open document against disk, because the
    // individual change events were dropped. The worker only signals; it never
    // touches the flow.
    bool pendingExternalFullReconcile = false;
    // Save expectations handed from the save primitive to the worker thread, applied
    // to the watcher on the worker thread so registration never races poll().
    std::deque<SaveExpectation> pendingSaveRegistrations;
    std::thread thread;
    std::atomic<std::uint64_t> fullRefreshCount{0};

    int wakeReadFd = -1;
    int wakeWriteFd = -1;
};

CommandHandlerResult success() { return CommandHandlerResult::success(); }
CommandHandlerResult failure(std::string message) {
    return CommandHandlerResult::failure(std::move(message));
}

std::string wrongPayload(std::string_view commandId) {
    return std::string{commandId} + " payload has the wrong type";
}

std::string workspaceMessage(WorkspaceResult const& result) {
    return result.message.empty() ? "workspace operation failed" : result.message;
}

std::string tabMessage(TabResult const& result) {
    return result.message.empty() ? "tab operation failed" : result.message;
}

EditorSession::Impl::Impl(std::filesystem::path canonicalCwd,
                          std::filesystem::path scratchRoot,
                          std::filesystem::path recoveryRoot,
                          std::filesystem::path archiveRoot,
                          bool deferEnrichment,
                          std::shared_ptr<SyntaxParser> parser,
                          std::vector<StatusFieldProviderBinding>
                              statusFieldProviderOverrides,
                          bool enableGitDiffWorker,
                          bool enableFilesystemWatcher)
    : root{std::move(canonicalCwd)},
      scratchRoot{std::filesystem::weakly_canonical(scratchRoot)},
      recoveryRoot{std::filesystem::weakly_canonical(recoveryRoot)},
      archiveRoot{std::filesystem::weakly_canonical(archiveRoot)},
      recovery{RecoveryManager::create(recoveryRoot)},
      scratch{ScratchStore::create(scratchRoot, root)},
      workspace{Workspace::create(root, recovery, this->archiveRoot)},
      selection{initialSelection()},
      clipboard{4},
      shell{},
      tabs{*this},
      external{recovery, diff},
      syntaxParser{std::move(parser)},
      statusFieldCatalog{p0StatusFieldCatalog()},
      interaction{assembleWholeScreen(statusFieldCatalog, "help.open",
                                     StyleDimensions{}, Style{}.inputLineSigil,
                                     std::nullopt),
                  tree, 1},
      search{*this, *this},
      theme{defaultTheme()},
      deferringEnrichment{deferEnrichment} {
    for (auto& provider : defaultStatusFieldProviders()) {
        statusFieldProviders.insert_or_assign(provider.id,
                                              std::move(provider.provider));
    }
    for (auto& provider : statusFieldProviderOverrides) {
        statusFieldProviders.insert_or_assign(provider.id,
                                              std::move(provider.provider));
    }
    homeDirectory = resolveHomeDirectory();
    workspace.setSaveObserver([this](const std::filesystem::path& relativePath) {
        registerExternalSaveExpectation(relativePath);
    });
    (void)refreshTree();
    refreshSyntax();
    startGitDiffWorker(enableGitDiffWorker, enableFilesystemWatcher);
    lastPublishedWatcherAvailable =
        watcherAvailable.load(std::memory_order_relaxed);
}

EditorSession::Impl::~Impl() { stopGitDiffWorker(); }

void EditorSession::Impl::startGitDiffWorker(bool enableGit, bool enableWatcher) {
    if (!enableGit && !enableWatcher) {
        return;
    }
    auto state = std::make_unique<GitDiffRefreshWorkerState>(root);
    // The unset-default mode follows watcher availability (Event when watching is
    // enabled, Poll otherwise); an explicit env override still wins. The worker
    // thread downgrades Event->Poll if the watcher then fails to construct.
    state->mode = resolveGitDiffMode(std::getenv("SSG_GIT_DIFF_MODE"),
                                     enableWatcher);
    // Test seam (consistent with SSG_GIT_DIFF_MODE): a short backstop makes the
    // Event-mode full-refresh backstop deterministically triggerable. Not a product
    // knob; the production value lives in kGitDiffBackstopInterval.
    if (const char* ms = std::getenv("SSG_GIT_DIFF_BACKSTOP_MS");
        ms != nullptr && *ms != '\0') {
        char* end = nullptr;
        const long value = std::strtol(ms, &end, 10);
        if (end != ms && value > 0) {
            state->backstopIntervalOverride = std::chrono::milliseconds{value};
        }
    }
    const bool gitUsable =
        enableGit && state->repository && state->repository->isUsable();
    // Optimistic: the worker thread constructs the watcher off the first-frame
    // path, so startup never pays for the recursive watch setup (invariant I12).
    // The thread clears this if construction fails (Decision 1/13's degradation).
    // False when watching is disabled -- no watcher, so unavailable.
    watcherAvailable.store(enableWatcher, std::memory_order_relaxed);
    int wakePipe[2] = {-1, -1};
    if (::pipe(wakePipe) != 0 || !setNonBlocking(wakePipe[0]) ||
        !setNonBlocking(wakePipe[1])) {
        if (wakePipe[0] != -1) {
            (void)::close(wakePipe[0]);
        }
        if (wakePipe[1] != -1) {
            (void)::close(wakePipe[1]);
        }
        watcherAvailable.store(false, std::memory_order_relaxed);
        return;
    }
    state->wakeReadFd = wakePipe[0];
    state->wakeWriteFd = wakePipe[1];
    const auto watcherRoot = root;

    state->thread = std::thread([worker = state.get(), gitUsable, enableWatcher,
                                 watcherRoot, this]() {
        // The watcher is a workspace service, not a git feature: construct it on the
        // worker thread (off the first-frame path) whenever the platform can and
        // watching is enabled, so external modification is observed in a non-git
        // workspace and in poll-for-git setups alike (Decision 1). Git's Event mode
        // is impossible without it.
        if (enableWatcher) {
            try {
                worker->watcher = makePlatformFilesystemWatcher(watcherRoot);
            } catch (const std::runtime_error&) {
                worker->watcher = nullptr;
            }
        }
        if (!worker->watcher) {
            watcherAvailable.store(false, std::memory_order_relaxed);
            if (worker->mode == GitDiffMode::Event) {
                worker->mode = GitDiffMode::Poll;
            }
            // The optimistic `true` was already seeded before this thread ran, so
            // an initial construction failure is a real availability edge: write
            // the wake byte so the runtime-thread drain publishes the false and
            // advances the revision, exactly like the mid-session watcher-death
            // edge (Decision 13). Without this a client that saw the optimistic
            // `true` would never learn watching is off.
            const char byte = 'g';
            (void)::write(worker->wakeWriteFd, &byte, 1);
        }
        if (gitUsable && worker->watcher) {
            try {
                worker->metadataDirectories =
                    worker->repository->metadataDirectories();
                worker->metadataWatcher = makePlatformGitMetadataWatcher(
                    worker->metadataDirectories);
            } catch (const std::exception&) {
                worker->mode = GitDiffMode::Poll;
            }
        }
        worker->watcherAvailable = worker->watcher != nullptr;
        // Nothing to serve: no usable git repository to scan and no watcher to
        // observe. External modification is simply not observed.
        if (!gitUsable && !worker->watcher) {
            return;
        }
        const auto shouldStop = [&]() {
            std::lock_guard lock(worker->mutex);
            return worker->stop;
        };
        const auto maybeRefreshAll = [&]() -> std::optional<GitDiffRefreshResult> {
            if (shouldStop()) {
                return std::nullopt;
            }
            try {
                ++worker->fullRefreshCount;
                auto refreshed = worker->source.refresh(*worker->repository);
                const auto directories = worker->repository->metadataDirectories();
                if (worker->metadataWatcher &&
                    (directories != worker->metadataDirectories ||
                     !worker->metadataWatcher->healthy())) {
                    worker->metadataWatcher->replaceDirectories(directories);
                    worker->metadataDirectories = directories;
                }
                if (shouldStop()) {
                    return std::nullopt;
                }
                return refreshed;
            } catch (const std::system_error&) {
                return GitDiffRefreshResult{
                    .applied = false, .requestedRescan = true, .accepted = false};
            } catch (const std::exception&) {
                return GitDiffRefreshResult{
                    .applied = false, .requestedRescan = true, .accepted = false};
            }
        };
        const auto maybeRefreshPaths =
            [&](const std::vector<std::filesystem::path>& paths)
            -> std::optional<GitDiffRefreshResult> {
            if (shouldStop()) {
                return std::nullopt;
            }
            try {
                auto refreshed = worker->source.refreshPaths(*worker->repository, paths);
                if (shouldStop()) {
                    return std::nullopt;
                }
                return refreshed;
            } catch (const std::system_error&) {
                return GitDiffRefreshResult{
                    .applied = false, .requestedRescan = true, .accepted = false};
            } catch (const std::exception&) {
                return GitDiffRefreshResult{
                    .applied = false, .requestedRescan = true, .accepted = false};
            }
        };
        const auto scheduleRetry = [&]() {
            std::lock_guard lock(worker->mutex);
            worker->retryPending = true;
            worker->nextRetry = std::chrono::steady_clock::now() + kGitDiffRetryDelay;
        };
        const auto clearRetry = [&]() {
            std::lock_guard lock(worker->mutex);
            worker->retryPending = false;
            worker->nextRetry = std::chrono::steady_clock::time_point::max();
        };
        const auto queueLatestScan = [&]() {
            auto scan = worker->source.latestAppliedScan();
            if (!scan) {
                return;
            }
            bool signal = false;
            {
                std::lock_guard lock(worker->mutex);
                signal = worker->pendingScans.empty();
                worker->pendingScans.push_back(std::move(*scan));
            }
            if (signal) {
                const char byte = 'g';
                (void)::write(worker->wakeWriteFd, &byte, 1);
            }
        };
        // The branch is published independently of the diff so an incomplete
        // repository scan never hides the branch indicator.
        const auto queueBranchScan = [&]() {
            auto scan = worker->source.takeBranchOnlyScanIfChanged();
            if (!scan) {
                return;
            }
            bool signal = false;
            {
                std::lock_guard lock(worker->mutex);
                signal = worker->pendingScans.empty();
                worker->pendingScans.push_back(std::move(*scan));
            }
            if (signal) {
                const char byte = 'g';
                (void)::write(worker->wakeWriteFd, &byte, 1);
            }
        };

        std::function<void(const GitDiffRefreshResult&, bool)> handleResult;
        handleResult = [&](const GitDiffRefreshResult& refreshed, bool fullRefresh) {
            queueBranchScan();
            if (refreshed.applied) {
                queueLatestScan();
                clearRetry();
            }
            // Retry (or fall back a path scan to a full refresh) ONLY when the source
            // asked for a rescan -- a transient failure (incomplete scan, index.lock).
            if (refreshed.shouldRetry()) {
                if (!fullRefresh) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        return;
                    }
                    handleResult(*full, true);
                    return;
                }
                scheduleRetry();
            }
        };

        const auto applyPendingSaveRegistrations = [&]() {
            std::deque<SaveExpectation> registrations;
            {
                std::lock_guard lock(worker->mutex);
                registrations.swap(worker->pendingSaveRegistrations);
            }
            // Drain unconditionally so registrations never accumulate; apply only
            // when a watcher exists (accessed on this, the owning, thread).
            if (!worker->watcher) {
                return;
            }
            for (auto& expectation : registrations) {
                worker->watcher->registerSave(std::move(expectation));
            }
        };
        // Hand normalized external events to the runtime-thread reconcile, coalescing
        // the wake byte with the git-scan queue so the host drains both at once.
        const auto queueWatchEvents = [&](const std::vector<WatchEvent>& events) {
            bool signal = false;
            {
                std::lock_guard lock(worker->mutex);
                signal = worker->pendingWatchEvents.empty() &&
                         worker->pendingScans.empty();
                for (const auto& event : events) {
                    worker->pendingWatchEvents.push_back(event);
                }
            }
            if (signal) {
                const char byte = 'g';
                (void)::write(worker->wakeWriteFd, &byte, 1);
            }
        };

        if (gitUsable) {
            if (auto first = maybeRefreshAll()) {
                handleResult(*first, true);
            } else {
                return;
            }
        }
        auto nextPoll = std::chrono::steady_clock::now() + kGitDiffPollInterval;
        // Event mode has no periodic full refresh, so a long-interval backstop
        // bounds the staleness of anything the watcher cannot observe (Decision:
        // external git ops, worktree metadata outside the tree, dropped events).
        auto nextBackstop =
            std::chrono::steady_clock::now() + kGitDiffEventRecoveryInterval;
        // A test hook can shorten the backstop so it is deterministically triggerable.
        const auto backstopInterval = worker->backstopIntervalOverride
                                          ? *worker->backstopIntervalOverride
                                          : kGitDiffEventRecoveryInterval;
        nextBackstop = std::chrono::steady_clock::now() + backstopInterval;
        while (!shouldStop()) {
            applyPendingSaveRegistrations();
            const auto now = std::chrono::steady_clock::now();
            auto wakeAt = now + kGitMetadataWatchPollInterval;
            {
                std::lock_guard lock(worker->mutex);
                if (gitUsable && worker->mode == GitDiffMode::Poll) {
                    wakeAt = std::min(wakeAt, nextPoll);
                }
                if (gitUsable && worker->mode == GitDiffMode::Event) {
                    wakeAt = std::min(wakeAt, nextBackstop);
                }
                if (gitUsable && worker->retryPending) {
                    wakeAt = std::min(wakeAt, worker->nextRetry);
                }
            }
            const auto timeout =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    wakeAt > now ? wakeAt - now : std::chrono::milliseconds{0});

            if (worker->watcher) {
                std::vector<WatchEvent> events;
                try {
                    events = worker->watcher->poll(timeout);
                } catch (const std::runtime_error&) {
                    // The watcher died mid-session; drop it and fall back to git
                    // polling. External modification is no longer observed, so the
                    // durable capability flips to unavailable and a wake byte makes
                    // the runtime-thread drain observe the transition (Decision 13).
                    worker->watcher.reset();
                    worker->metadataWatcher.reset();
                    watcherAvailable.store(false, std::memory_order_relaxed);
                    worker->mode = GitDiffMode::Poll;
                    {
                        const char byte = 'g';
                        (void)::write(worker->wakeWriteFd, &byte, 1);
                    }
                    if (gitUsable) {
                        auto full = maybeRefreshAll();
                        if (!full) {
                            break;
                        }
                        handleResult(*full, true);
                    }
                    continue;
                }
                bool overflowed = false;
                for (const auto& event : events) {
                    if (event.kind == WatchEventKind::Overflow) {
                        overflowed = true;
                        break;
                    }
                }
                std::vector<WatchEvent> workspaceEvents;
                if (!overflowed && !events.empty()) {
                    workspaceEvents.reserve(events.size());
                    bool metadataDirty = false;
                    for (auto& event : events) {
                        if (!event.path.empty() &&
                            *event.path.begin() == ".git") {
                            metadataDirty = true;
                        } else {
                            workspaceEvents.push_back(std::move(event));
                        }
                    }
                    if (!workspaceEvents.empty()) {
                        queueWatchEvents(workspaceEvents);
                    }
                    if (metadataDirty && gitUsable) {
                        auto full = maybeRefreshAll();
                        if (!full) break;
                        handleResult(*full, true);
                    }
                }
                if (overflowed) {
                    // The watcher lost events: the external flow must resynchronize
                    // every open document against disk, not just refresh git. Signal
                    // the runtime-thread drain (which owns the flow) to do the full
                    // re-scan; the worker never touches the flow itself.
                    bool signal = false;
                    {
                        std::lock_guard lock(worker->mutex);
                        signal = worker->pendingScans.empty() &&
                                 worker->pendingWatchEvents.empty() &&
                                 !worker->pendingExternalFullReconcile;
                        worker->pendingExternalFullReconcile = true;
                    }
                    if (signal) {
                        const char byte = 'g';
                        (void)::write(worker->wakeWriteFd, &byte, 1);
                    }
                }
                if (gitUsable && worker->mode == GitDiffMode::Event) {
                    if (overflowed) {
                        auto full = maybeRefreshAll();
                        if (!full) {
                            break;
                        }
                        handleResult(*full, true);
                    } else if (!workspaceEvents.empty()) {
                        std::vector<std::filesystem::path> paths;
                        paths.reserve(workspaceEvents.size() * 2);
                        for (const auto& event : workspaceEvents) {
                            paths.push_back(event.path);
                            if (event.previousPath) {
                                paths.push_back(*event.previousPath);
                            }
                        }
                        std::sort(paths.begin(), paths.end());
                        paths.erase(std::unique(paths.begin(), paths.end()),
                                    paths.end());
                        auto pathRefresh = maybeRefreshPaths(paths);
                        if (!pathRefresh) {
                            break;
                        }
                        handleResult(*pathRefresh, false);
                    }
                }
            } else {
                std::unique_lock lock(worker->mutex);
                if (worker->wake.wait_until(lock, wakeAt,
                                            [&]() { return worker->stop; })) {
                    break;
                }
                lock.unlock();
            }

            const auto afterWait = std::chrono::steady_clock::now();
            if (gitUsable) {
                if (worker->mode == GitDiffMode::Event &&
                    worker->metadataWatcher) {
                    try {
                        if (worker->metadataWatcher->poll(
                                std::chrono::milliseconds{0})) {
                            auto full = maybeRefreshAll();
                            if (!full) break;
                            handleResult(*full, true);
                        }
                    } catch (const std::exception&) {
                        worker->metadataWatcher.reset();
                        worker->mode = GitDiffMode::Poll;
                    }
                }
                bool retryDue = false;
                {
                    std::lock_guard lock(worker->mutex);
                    retryDue =
                        worker->retryPending && afterWait >= worker->nextRetry;
                }
                if (retryDue) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        break;
                    }
                    handleResult(*full, true);
                }
                if (worker->mode == GitDiffMode::Poll && afterWait >= nextPoll) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        break;
                    }
                    handleResult(*full, true);
                    nextPoll = afterWait + kGitDiffPollInterval;
                }
                if (worker->mode == GitDiffMode::Event &&
                    afterWait >= nextBackstop) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        break;
                    }
                    handleResult(*full, true);
                    nextBackstop = afterWait + backstopInterval;
                }
            }
        }
    });
    gitDiffWorker = std::move(state);
}

void EditorSession::Impl::stopGitDiffWorker() {
    if (!gitDiffWorker) {
        return;
    }
    {
        std::lock_guard lock(gitDiffWorker->mutex);
        gitDiffWorker->stop = true;
    }
    gitDiffWorker->wake.notify_all();
    if (gitDiffWorker->thread.joinable()) {
        gitDiffWorker->thread.join();
    }
    if (gitDiffWorker->wakeReadFd != -1) {
        (void)::close(gitDiffWorker->wakeReadFd);
        gitDiffWorker->wakeReadFd = -1;
    }
    if (gitDiffWorker->wakeWriteFd != -1) {
        (void)::close(gitDiffWorker->wakeWriteFd);
        gitDiffWorker->wakeWriteFd = -1;
    }
    gitDiffWorker.reset();
}

bool EditorSession::Impl::drainGitDiffScans() {
    const auto drainEntryRevision = session->revision();
    auto const availabilityBefore = lastPublishedWatcherAvailable;
    drainWatcherAvailability();
    bool accepted = availabilityBefore != lastPublishedWatcherAvailable;
    if (!gitDiffWorker) {
        return accepted;
    }
    char scratch[64];
    while (true) {
        const auto count = ::read(gitDiffWorker->wakeReadFd, scratch, sizeof scratch);
        if (count <= 0) {
            if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            break;
        }
    }
    std::deque<GitDiffScan> scans;
    std::deque<WatchEvent> events;
    bool fullReconcile = false;
    {
        std::lock_guard lock(gitDiffWorker->mutex);
        scans.swap(gitDiffWorker->pendingScans);
        events.swap(gitDiffWorker->pendingWatchEvents);
        fullReconcile = gitDiffWorker->pendingExternalFullReconcile;
        gitDiffWorker->pendingExternalFullReconcile = false;
    }
    accepted = accepted || !scans.empty() || !events.empty() || fullReconcile;
    for (auto& scan : scans) {
        (void)applyGitDiffScan(std::move(scan));
    }
    // Git scans first, then the external reconcile once over the whole queue, so a
    // burst of git scans never starves external ingress and both draw revisions
    // from the one shared DiffModel in order (Decision 10).
    if (!events.empty()) {
        reconcileExternalWatchEvents(
            std::vector<WatchEvent>{events.begin(), events.end()});
        const bool inventoryChanged = std::any_of(
            events.begin(), events.end(), [](const WatchEvent& event) {
                return event.kind != WatchEventKind::Modify ||
                       event.path.filename() == ".gitignore";
            });
        if (inventoryChanged) {
            refreshTreeForPublication(drainEntryRevision);
        }
    }
    // After ordinary ingress, recover any events the watcher dropped on overflow by
    // re-scanning every open document against disk (a full external resync).
    if (fullReconcile) {
        reconcileAllOpenDocumentsAgainstDisk();
        refreshTreeForPublication(drainEntryRevision);
    }
    return accepted;
}

void EditorSession::Impl::drainWatcherAvailability() {
    const bool current = watcherAvailable.load(std::memory_order_relaxed);
    if (current == lastPublishedWatcherAvailable) {
        return;
    }
    lastPublishedWatcherAvailable = current;
    // The atomic already feeds sections(); advancing the revision is what makes a
    // delta client re-observe the flipped capability (Decision 13).
    if (session) {
        session->advanceRevision();
    }
}

int EditorSession::Impl::gitDiffWakeDescriptor() const {
    return gitDiffWorker ? gitDiffWorker->wakeReadFd : -1;
}

CommandHandlerResult EditorSession::Impl::runTransaction(
    std::function<CommandHandlerResult()> operation) {
    return operation();
}

std::any& EditorSession::Impl::featureStateValue(std::type_index) {
    throw std::logic_error{"EditorSession exposes feature state through snapshots"};
}

void EditorSession::Impl::publishStatusValue(std::type_index, std::any statusValue) {
    if (auto const* item = std::any_cast<StatusItem>(&statusValue)) {
        (void)status.enqueue(*item);
    }
}

void EditorSession::Impl::publishDeltaValue(std::type_index, std::any) {}

TabLifecycleResult EditorSession::Impl::close(
    const TabState& tab, std::chrono::milliseconds durabilityTimeout) {
    if (!tab.document) {
        if (tab.kind == TabKind::ReadOnlyOutput) {
            // A read-only output tab (help, generated content) is ephemeral and
            // regenerable: it is never journaled for reopen and never persists.
            // Drop its backing document and map entry directly, skipping the
            // recovery/scratch path entirely, and signal ephemeral so closeAt
            // accepts the missing compensation record.
            const auto mapped = readOnlyTabDocuments.find(tab.contentIdentity);
            if (mapped != readOnlyTabDocuments.end()) {
                const auto document = mapped->second;
                readOnlyTabDocuments.erase(mapped);
                documentRuntimeStates.erase(document.value());
                documentLanguageOverrides.erase(document.value());
                (void)workspace.removeDocument(document);
            }
            return {TabError::None, {}, std::nullopt, std::nullopt, std::nullopt,
                    false, true};
        }
        if (tab.kind == TabKind::LiveDiff) {
            const auto mapped = liveDiffDocuments.find(tab.contentIdentity);
            if (mapped != liveDiffDocuments.end()) {
                const auto document = mapped->second;
                const bool stillReferenced = std::any_of(
                    tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
                    [&](const TabState& candidate) {
                        return candidate.id != tab.id &&
                               candidate.document == document;
                    });
                if (!stillReferenced) {
                    auto state = workspace.state(document);
                    if (!state) {
                        return {TabError::NotFound,
                                "live diff document state does not exist",
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    std::optional<JournalDocument> journal;
                    if (auto const* current = workspace.tryDocument(document);
                        current != nullptr) {
                        journal = JournalDocument{state->key, current->mode(),
                                                  state->dirty,
                                                  current->snapshot().text,
                                                  workspace.baselineFor(document)};
                    }
                    auto closed =
                        recovery.closeDocument(journal, scratch, durabilityTimeout);
                    if (!closed.accepted()) {
                        return {TabError::LifecycleFailed, closed.error->message,
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    if (journal) scratch.removeDocument(state->key);
                    auto removed = workspace.removeDocument(document);
                    if (!removed.accepted()) {
                        return {TabError::LifecycleFailed, workspaceMessage(removed),
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    documentRuntimeStates.erase(document.value());
                    liveDiffDocuments.erase(mapped);
                    return {TabError::None, {}, closed.compensation, std::nullopt,
                            std::nullopt,
                            scratch.waitUntilDurable(durabilityTimeout)};
                }
                liveDiffDocuments.erase(mapped);
            }
        }
        return {};
    }
    const bool sharedByDocumentTab = std::any_of(
        tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
        [&](const TabState& candidate) {
            return candidate.id != tab.id &&
                   candidate.document == tab.document;
        });
    const bool sharedByLiveDiffTab = std::any_of(
        tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
        [&](const TabState& candidate) {
            if (candidate.id == tab.id || candidate.kind != TabKind::LiveDiff) {
                return false;
            }
            const auto mapped = liveDiffDocuments.find(candidate.contentIdentity);
            return mapped != liveDiffDocuments.end() &&
                   mapped->second == *tab.document;
        });
    if (sharedByDocumentTab || sharedByLiveDiffTab) {
        return {};
    }
    auto state = workspace.state(*tab.document);
    if (!state) return {TabError::NotFound, "tab document does not exist",
                        std::nullopt, std::nullopt, std::nullopt, false};
    std::optional<JournalDocument> document;
    if (auto const* current = workspace.tryDocument(*tab.document); current != nullptr) {
        document = JournalDocument{state->key, current->mode(), state->dirty,
                                   current->snapshot().text,
                                   workspace.baselineFor(*tab.document)};
    }
    auto closed = recovery.closeDocument(document, scratch, durabilityTimeout);
    if (!closed.accepted()) {
        return {TabError::LifecycleFailed, closed.error->message, std::nullopt,
                std::nullopt, std::nullopt, false};
    }
    if (document) scratch.removeDocument(state->key);
    auto removed = workspace.removeDocument(*tab.document);
    if (!removed.accepted()) {
        return {TabError::LifecycleFailed, workspaceMessage(removed),
                std::nullopt, std::nullopt, std::nullopt, false};
    }
    documentRuntimeStates.erase(tab.document->value());
    return {TabError::None, {}, closed.compensation, std::nullopt, std::nullopt,
            scratch.waitUntilDurable(durabilityTimeout)};
}

TabLifecycleResult EditorSession::Impl::reopen(
    const TabState& tab, const RecoveryRecordId& compensation) {
    std::optional<JournalDocument> restoredDocument;
    auto restored = recovery.restoreDocument(compensation, restoredDocument);
    if (!restored.accepted()) {
        return {TabError::LifecycleFailed, restored.error->message, std::nullopt,
                std::nullopt, std::nullopt, false};
    }
    if (!restoredDocument) {
        return {TabError::LifecycleFailed, "recovery record had no document",
                std::nullopt, std::nullopt, std::nullopt, false};
    }

    WorkspaceResult opened;
    if (tab.kind == TabKind::LiveDiff) {
        opened = workspace.openVirtualDocument(
            tab.label, restoredDocument->utf8Content, restoredDocument->mode);
    } else if (restoredDocument->key.kind() == JournalDocumentKeyKind::Saved) {
        opened = workspace.openFile(restoredDocument->key.savedPath());
    } else {
        opened = workspace.newDocument();
    }
    if (!opened.accepted() || !opened.document) {
        return {TabError::LifecycleFailed, workspaceMessage(opened), std::nullopt,
                std::nullopt, std::nullopt, false};
    }
    auto* reopenedDocument = const_cast<Document*>(workspace.tryDocument(*opened.document));
    if (!reopenedDocument) {
        return {TabError::LifecycleFailed,
                "reopened document was not available in workspace",
                std::nullopt, std::nullopt, std::nullopt, false};
    }
    const auto current = reopenedDocument->snapshot();
    if (current.text != restoredDocument->utf8Content) {
        auto replace = workspace.apply(
            *opened.document,
            {current.revision,
             {{ByteOffset{0}, current.text.size(), restoredDocument->utf8Content}}});
        if (!replace.accepted()) {
            return {TabError::LifecycleFailed, replace.message, std::nullopt,
                    std::nullopt, std::nullopt, false};
        }
    }
    ensureDocumentRuntimeState(*opened.document);
    auto reopenedState = workspace.state(*opened.document);
    if (!reopenedState) {
        return {TabError::LifecycleFailed,
                "reopened document state was not available in workspace",
                std::nullopt, std::nullopt, std::nullopt, false};
    }
    if (tab.kind == TabKind::LiveDiff) {
        liveDiffDocuments[tab.contentIdentity] = *opened.document;
    }
    return {TabError::None, {}, std::nullopt, *opened.document,
            reopenedState->key, true};
}

WorkspaceSnapshot EditorSession::Impl::snapshot(Revision revision) const {
    WorkspaceSnapshot result;
    result.revision = revision;
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) continue;
        result.files.push_back({state->key.savedPath(), workspace.document(id).snapshot().text});
    }
    std::filesystem::recursive_directory_iterator it{root};
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; ++it) {
        auto const& entry = *it;
        if (entry.is_directory() &&
            (pathContains(scratchRoot, entry.path()) ||
             pathContains(recoveryRoot, entry.path()) ||
             pathContains(archiveRoot, entry.path()))) {
            it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file()) continue;
        auto relative = relativeToRoot(root, entry.path());
        if (!relative) continue;
        if (std::find_if(result.files.begin(), result.files.end(), [&](WorkspaceFile const& file) {
                return file.path == *relative;
            }) != result.files.end()) {
            continue;
        }
        auto content = readFileText(entry.path());
        if (!content) continue;
        result.files.push_back({*relative, std::move(*content)});
    }
    return result;
}

std::vector<SearchCommandDescriptor> EditorSession::Impl::descriptors() const {
    std::vector<SearchCommandDescriptor> result;
    for (auto const* command : catalog->commands()) {
        result.push_back({command->id, command->id});
    }
    return result;
}

PaletteExecutionResult EditorSession::Impl::execute(std::string_view commandId) {
    return {catalog->find(commandId) != nullptr, {}};
}

WorkspaceApplyResult EditorSession::Impl::apply(
    const WorkspaceReplacePreview& preview, WorkspaceRecoverySink& recoverySink) {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> normalizedPaths;
    paths.reserve(preview.changes.size());
    normalizedPaths.reserve(preview.changes.size());
    for (auto const& change : preview.changes) {
        std::string message;
        auto path = workspaceChangePath(root, change.path, message);
        if (!path) {
            return {FindReplaceError::WorkspaceRejected,
                    preview.sourceRevision, std::move(message)};
        }
        if (pathContains(scratchRoot, *path) ||
            pathContains(recoveryRoot, *path) ||
            pathContains(archiveRoot, *path)) {
            return {FindReplaceError::WorkspaceRejected,
                    preview.sourceRevision,
                    "workspace replacement path targets runtime state"};
        }
        auto normalized =
            std::filesystem::path{change.path}.lexically_normal().generic_string();
        std::string current;
        bool foundOpenDocument = false;
        for (auto const id : workspace.documents()) {
            auto state = workspace.state(id);
            if (!state || state->key.kind() != JournalDocumentKeyKind::Saved ||
                state->key.savedPath() != normalized) {
                continue;
            }
            current = workspace.document(id).snapshot().text;
            foundOpenDocument = true;
            break;
        }
        if (!foundOpenDocument) {
            auto content = readFileText(*path);
            // Unreadable is not "unchanged": refusing here is what stops a
            // replacement being applied to a file whose current state is
            // unknown.
            if (!content) {
                return {FindReplaceError::StaleRevision, preview.sourceRevision,
                        "workspace replacement target cannot be read"};
            }
            current = std::move(*content);
        }
        if (current != change.before) {
            return {FindReplaceError::StaleRevision, preview.sourceRevision,
                    "workspace replacement preview is stale"};
        }
        paths.push_back(std::move(*path));
        normalizedPaths.push_back(std::move(normalized));
    }
    WorkspaceRecoveryRecord record{preview.sourceRevision, Revision{preview.sourceRevision.value() + 1}, preview.changes};
    if (!recoverySink.store(record)) {
        return {FindReplaceError::RecoveryRejected, preview.sourceRevision, "workspace replacement recovery rejected"};
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        auto const& change = preview.changes[index];
        // Atomic replace, not truncate-then-stream: a replace-across-files run
        // interrupted part way through must leave each file either wholly old
        // or wholly new. A truncating write turns an interruption into a
        // truncated source file.
        try {
            replaceFileAtomically(paths[index], asByteSpan(change.after));
        } catch (const std::exception&) {
            return {FindReplaceError::WorkspaceRejected, preview.sourceRevision, "failed to write workspace file"};
        }
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        auto const& change = preview.changes[index];
        for (auto const id : workspace.documents()) {
            auto state = workspace.state(id);
            if (!state || state->key.kind() != JournalDocumentKeyKind::Saved ||
                state->key.savedPath() != normalizedPaths[index]) {
                continue;
            }
            auto reloaded = workspace.reload(id);
            if (!reloaded.accepted()) {
                return {FindReplaceError::WorkspaceRejected,
                        preview.sourceRevision, workspaceMessage(reloaded)};
            }
            (void)updateTabsFor(id);
        }
    }
    return {FindReplaceError::None, record.appliedRevision, {}};
}

WorkspaceApplyResult EditorSession::Impl::recover(const WorkspaceRecoveryRecord& record) {
    for (auto const& change : record.changes) {
        // This is the rollback path, so an interrupted write here would leave a
        // file that is neither the edited version nor the original.
        try {
            replaceFileAtomically(root / change.path, asByteSpan(change.before));
        } catch (const std::exception&) {
            return {FindReplaceError::WorkspaceRejected, record.appliedRevision, "failed to recover workspace file"};
        }
    }
    return {FindReplaceError::None, record.appliedRevision, {}};
}

bool EditorSession::Impl::store(const WorkspaceRecoveryRecord&) { return true; }

std::optional<LspDocumentSnapshot> EditorSession::Impl::snapshot(std::string_view uri) const {
    auto path = pathFromUri(uri);
    if (!path) return std::nullopt;
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) continue;
        if (uriFromPath(root / state->key.savedPath()) == uri) {
            return LspDocumentSnapshot{std::string{uri}, workspace.document(id).revision(), 1,
                                       workspace.document(id).snapshot().text};
        }
    }
    return std::nullopt;
}

LspWorkspaceDocumentWriteResult EditorSession::Impl::apply(
    std::string uri, Revision expectedRevision, std::string text) {
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) continue;
        if (uriFromPath(root / state->key.savedPath()) != uri) continue;
        auto& document = const_cast<Document&>(workspace.document(id));
        if (document.revision() != expectedRevision) {
            return {document.revision(), LspWorkspaceDocumentError::StaleRevision, "document revision is stale"};
        }
        auto snapshot = document.snapshot();
        auto result = document.apply({snapshot.revision, {{ByteOffset{0}, snapshot.text.size(), std::move(text)}}});
        if (!result.accepted()) return {document.revision(), LspWorkspaceDocumentError::WriteFailed, result.message};
        return {result.revision, LspWorkspaceDocumentError::None, {}};
    }
    return {Revision{0}, LspWorkspaceDocumentError::UnknownDocument, "document URI is not open"};
}

LspWorkspaceFileResult EditorSession::Impl::snapshot(std::string_view uri, LspWorkspaceFileNode& node) const {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::NotFound, "URI is not a file URI"};
    if (!std::filesystem::exists(*path)) {
        node.kind = LspWorkspaceFileNodeKind::Missing;
    } else if (std::filesystem::is_directory(*path)) {
        node.kind = LspWorkspaceFileNodeKind::Directory;
    } else {
        node.kind = LspWorkspaceFileNodeKind::File;
        auto content = readFileText(*path);
        // This snapshot is what a rollback restores from. Recording empty
        // content for a file that merely could not be read would turn a failed
        // edit into a truncation.
        if (!content) {
            return {LspWorkspaceFileError::IoError,
                    "failed to read file for snapshot"};
        }
        node.content = std::move(*content);
    }
    return {};
}


LspWorkspaceFileResult EditorSession::Impl::createFile(std::string uri, bool overwrite) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    if (overwrite) {
        try {
            replaceFileAtomically(*path, {});
        } catch (const std::exception& exception) {
            return {LspWorkspaceFileError::IoError, exception.what()};
        }
        return {};
    }
    // Exclusive create rather than exists()-then-truncate: the previous form
    // could report success after another process won the race, and its truncate
    // would then have destroyed that file's contents.
    return asLspResult(createFileExclusively(*path, {}));
}

LspWorkspaceFileResult EditorSession::Impl::writeFile(std::string uri, std::string content) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    // Atomic replace rather than truncate-then-stream: an LSP edit interrupted
    // part way through must leave the user's file whole, not half written.
    try {
        replaceFileAtomically(*path, asByteSpan(content));
    } catch (const std::exception& exception) {
        return {LspWorkspaceFileError::IoError, exception.what()};
    }
    return {};
}

LspWorkspaceFileResult EditorSession::Impl::renamePath(std::string oldUri, std::string newUri, bool overwrite) {
    auto oldPath = pathFromUri(oldUri);
    auto newPath = pathFromUri(newUri);
    if (!oldPath || !newPath) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    if (!overwrite) {
        return asLspResult(renameFileNoClobber(*oldPath, *newPath));
    }
    std::error_code code;
    // seam-exempt: the LSP protocol asked for overwrite explicitly
    std::filesystem::rename(*oldPath, *newPath, code);
    return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
}

LspWorkspaceFileResult EditorSession::Impl::deletePath(std::string uri, bool recursive) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    std::error_code code;
    if (recursive) std::filesystem::remove_all(*path, code);
    // seam-exempt: LSP delete has no archive contract; file.delete is the archived path
    else std::filesystem::remove(*path, code);
    return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
}

LspWorkspaceFileResult EditorSession::Impl::restorePath(std::string uri, const LspWorkspaceFileNode& node) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    if (node.kind == LspWorkspaceFileNodeKind::Missing) {
        std::error_code code;
        std::filesystem::remove_all(*path, code);
        return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
    }
    if (node.kind == LspWorkspaceFileNodeKind::Directory) {
        std::error_code code;
        std::filesystem::create_directories(*path, code);
        return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
    }
    return writeFile(std::move(uri), node.content);
}

std::optional<FileDocumentId> EditorSession::Impl::activeDocumentId() const {
    // The active tab is the single source of truth for the active editor
    // document.  With no active tab (e.g. the last tab was closed) there is no
    // active document and the shell renders its empty state; the editor view
    // never shows a document that has no tab.
    auto const& view = tabs.viewState();
    if (!view.active) return std::nullopt;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(), [&](TabState const& tab) {
        return tab.id == *view.active;
    });
    if (found == view.tabs.end()) return std::nullopt;
    if (found->document) return found->document;
    if (found->kind == TabKind::LiveDiff) {
        const auto mapped = liveDiffDocuments.find(found->contentIdentity);
        if (mapped != liveDiffDocuments.end()) {
            return mapped->second;
        }
    }
    if (found->kind == TabKind::ReadOnlyOutput) {
        const auto mapped = readOnlyTabDocuments.find(found->contentIdentity);
        if (mapped != readOnlyTabDocuments.end()) {
            return mapped->second;
        }
    }
    return std::nullopt;
}

const TabState* EditorSession::Impl::activeTabState() const {
    auto const& view = tabs.viewState();
    if (!view.active) return nullptr;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(),
                              [&](const TabState& tab) {
                                  return tab.id == *view.active;
                              });
    if (found == view.tabs.end()) return nullptr;
    return &*found;
}

CommandHandlerResult EditorSession::Impl::openOrFocusLiveDiffTab(
    const DiffFileView& file, NavigationClass classification,
    std::optional<ClientId> userClient, std::optional<ViewId> userView) {
    const auto target = diffOpenFile(file);
    const auto diffText = liveDiffDocumentText(file);
    std::optional<FileDocumentId> document;
    auto mapped = liveDiffDocuments.find(target.id.value());
    if (mapped != liveDiffDocuments.end()) {
        if (const auto* opened = workspace.tryDocument(mapped->second);
            opened != nullptr &&
            opened->snapshot().text == diffText) {
            document = mapped->second;
        } else {
            documentRuntimeStates.erase(mapped->second.value());
            auto removed = workspace.removeDocument(mapped->second);
            liveDiffDocuments.erase(mapped);
            if (!removed.accepted()) {
                return failure(workspaceMessage(removed));
            }
        }
    }
    if (!document) {
        auto created = workspace.openVirtualDocument(
            liveDiffTabLabelForPath(target.path), diffText,
            DocumentMode::Diff);
        if (!created.accepted() || !created.document) {
            return failure(workspaceMessage(created));
        }
        document = *created.document;
    }

    ensureDocumentRuntimeState(*document);
    liveDiffDocuments[target.id.value()] = *document;
    auto opened = tabs.openContent(TabKind::LiveDiff, target.id.value(),
                                   liveDiffTabLabelForPath(target.path),
                                   DocumentMode::Diff);
    if (!opened.accepted()) {
        return failure(tabMessage(opened));
    }
    if (userClient && userView) {
        recordNavigation(*userClient, *userView, classification);
    }
    interaction.focusEditor();
    return success();
}

CommandHandlerResult EditorSession::Impl::openReadOnlyTab(
    TabKind kind, std::string contentIdentity, std::string label,
    std::string text, LanguageId language) {
    // Build the replacement document FIRST, then swap: a ReadOnly document
    // rejects Document::apply, so content is refreshed by remove+recreate (never
    // an in-place edit) -- and creating before removing keeps a refresh failure
    // non-destructive, so a failed rebuild leaves the existing tab intact.
    auto created =
        workspace.openVirtualDocument(label, text, DocumentMode::ReadOnly);
    if (!created.accepted() || !created.document) {
        return failure(workspaceMessage(created));
    }
    ensureDocumentRuntimeState(*created.document);
    documentLanguageOverrides.insert_or_assign(created.document->value(),
                                                std::move(language));
    auto mapped = readOnlyTabDocuments.find(contentIdentity);
    if (mapped != readOnlyTabDocuments.end()) {
        const auto previous = mapped->second;
        documentRuntimeStates.erase(previous.value());
        documentLanguageOverrides.erase(previous.value());
        (void)workspace.removeDocument(previous);
    }
    readOnlyTabDocuments[contentIdentity] = *created.document;
    auto opened =
        tabs.openContent(kind, contentIdentity, label, DocumentMode::ReadOnly);
    if (!opened.accepted()) {
        return failure(tabMessage(opened));
    }
    interaction.focusEditor();
    // Untitled documents get no language from a path, so highlight the override
    // language (e.g. Markdown) now that this tab is active.
    refreshSyntax();
    return success();
}

CommandHandlerResult EditorSession::Impl::openDraftDiff() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto state = workspace.state(*id);
    if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
        // A live diff tab's document (and an untitled buffer) is not a saved
        // file, so it has no on-disk side to diff the draft against.
        return failure("draft.diff needs a saved file");
    }
    const auto* opened = workspace.tryDocument(*id);
    if (opened == nullptr) return failure("no active document");
    const std::string draft = opened->snapshot().text;

    // The baseline is the file's CURRENT disk content, read now (not the
    // open-time bytes) so the diff reflects any external change. A missing or
    // unreadable file diffs the draft against empty, matching a deleted-file
    // conflict where the draft would recreate the file on save.
    const auto absolute =
        workspace.root() / std::filesystem::path{state->key.savedPath()};
    std::string disk;
    if (const auto read = readFile(absolute); read.ok()) {
        disk.assign(reinterpret_cast<const char*>(read.bytes.data()),
                    read.bytes.size());
    }

    const DiffFileId diffId{"draft:" + state->key.savedPath()};
    // Non-git entries share the DiffModel's monotonic revision line; one past
    // the current revision is always fresh. Create both seeds and updates the
    // entry (an existing non-git entry is updated in place), so re-running
    // draft.diff on the same file refreshes its tab.
    const Revision revision{diff.viewState().revision.value() + 1};
    const auto applied = diff.applyNonGitEvent(
        NonGitDiffEvent{NonGitDiffEventKind::Create, diffId,
                        std::filesystem::path{state->key.savedPath()},
                        std::nullopt, disk, draft},
        revision);
    if (!applied.accepted()) return failure("draft diff could not be computed");

    const auto file = diff.file(diffId);
    if (!file.has_value()) return failure("draft diff is unavailable");
    return openOrFocusLiveDiffTab(file->get(), NavigationClass::Programmatic,
                                  std::nullopt);
}

namespace {

// The one runtime routine that captures a file's watch state, used both to record
// a save expectation and to correlate an event whose state the injecting caller
// did not supply. Consistency between the two is what makes an SSG save match.
std::optional<WatchFileState> observeWatchState(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    std::uint64_t size = 0;
    if (std::filesystem::is_regular_file(status)) {
        size = std::filesystem::file_size(path, error);
        if (error) return std::nullopt;
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return std::nullopt;
    const auto nanos = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            modified.time_since_epoch())
            .count());
    try {
        return WatchFileState{fileIdentity(path), size, nanos};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

DiffFileId EditorSession::Impl::externalDiffFileId(std::string_view savedPath) {
    return DiffFileId{"external:" + std::string{savedPath}};
}

std::optional<std::string> EditorSession::Impl::savedPathFromExternalDiffId(
    const DiffFileId& id) {
    static constexpr std::string_view prefix{"external:"};
    const auto& value = id.value();
    if (std::string_view{value}.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    return value.substr(prefix.size());
}

std::optional<FileDocumentId> EditorSession::Impl::resolveExternalDocument(
    const DiffFileId& id) const {
    const auto savedPath = savedPathFromExternalDiffId(id);
    if (!savedPath) return std::nullopt;
    for (const auto documentId : workspace.documents()) {
        const auto state = workspace.state(documentId);
        if (state && state->key.kind() == JournalDocumentKeyKind::Saved &&
            state->key.savedPath() == *savedPath) {
            return documentId;
        }
    }
    return std::nullopt;
}

std::optional<FileDocumentId>
EditorSession::Impl::resolveOpenSavedDocumentByPath(
    const std::filesystem::path& relativePath) const {
    const auto normalized = relativePath.generic_string();
    for (const auto documentId : workspace.documents()) {
        const auto state = workspace.state(documentId);
        if (state && state->key.kind() == JournalDocumentKeyKind::Saved &&
            state->key.savedPath() == normalized) {
            return documentId;
        }
    }
    return std::nullopt;
}

void EditorSession::Impl::registerExternalSaveExpectation(
    const std::filesystem::path& relativePath) {
    const auto observed = observeWatchState(workspace.root() / relativePath);
    if (!observed) return;
    SaveExpectation expectation{relativePath, *observed};
    {
        std::lock_guard lock(externalSaveMutex);
        pendingSaveExpectations.push_back(expectation);
        // Bound the deque: a save the watcher never reports back must not
        // accumulate forever.
        constexpr std::size_t kMaxSaveExpectations = 256;
        while (pendingSaveExpectations.size() > kMaxSaveExpectations) {
            pendingSaveExpectations.pop_front();
        }
    }
    // Also register with the real watcher's normalizer so a genuine save is stamped
    // at source (Decision 9); handed to the worker thread, which owns the watcher,
    // so the main thread never touches it. Bounded so a save the worker never drains
    // cannot grow without limit.
    if (gitDiffWorker) {
        std::lock_guard lock(gitDiffWorker->mutex);
        auto& queue = gitDiffWorker->pendingSaveRegistrations;
        queue.push_back(std::move(expectation));
        constexpr std::size_t kMaxSaveRegistrations = 256;
        while (queue.size() > kMaxSaveRegistrations) {
            queue.pop_front();
        }
    }
}

void EditorSession::Impl::reconcileExternalWatchEvents(
    std::vector<WatchEvent> events, bool resync) {
    const auto flowRevisionBefore = external.viewState().revision;
    const auto diffRevisionBefore = diff.viewState().revision;
    for (auto& event : events) {
        if (event.kind == WatchEventKind::Overflow) {
            continue;
        }
        // Correlate SSG's own writes so a save never reads as an external change.
        // A self-save resolves the external state: consume exactly its expectation
        // and clear any pending conflict for the file, so a stale expectation can
        // never accumulate to suppress a later genuine external edit.
        {
            std::optional<WatchFileState> observed;
            if (event.identity && event.size && event.modificationTime) {
                observed = WatchFileState{*event.identity, *event.size,
                                          *event.modificationTime};
            } else {
                observed = observeWatchState(workspace.root() / event.path);
            }
            bool selfSave = false;
            if (observed) {
                std::lock_guard lock(externalSaveMutex);
                const auto found = std::find_if(
                    pendingSaveExpectations.begin(),
                    pendingSaveExpectations.end(),
                    [&](const SaveExpectation& expectation) {
                        return expectation.path == event.path &&
                               expectation.state == *observed;
                    });
                if (found != pendingSaveExpectations.end()) {
                    pendingSaveExpectations.erase(found);
                    selfSave = true;
                }
            }
            if (selfSave) {
                // The save already advanced the workspace baseline to the written
                // bytes; clearing any pending conflict needs no further cross-store
                // commit, so the dismissal commit is a no-op.
                (void)external.keepBuffer(
                    externalDiffFileId(event.path.generic_string()),
                    [](bool, const std::optional<std::string>&) { return true; });
                continue;
            }
        }

        const std::filesystem::path& lookupPath =
            (event.kind == WatchEventKind::Rename && event.previousPath)
                ? *event.previousPath
                : event.path;
        const auto documentId = resolveOpenSavedDocumentByPath(lookupPath);
        if (!documentId) {
            // A change to a file no open document corresponds to is ignored by this
            // flow; the tree/git refresh already covers it.
            continue;
        }
        const auto state = workspace.state(*documentId);
        const auto* document = workspace.tryDocument(*documentId);
        if (!state || document == nullptr) {
            continue;
        }

        const std::string savedPath = event.path.generic_string();
        const DiffFileId id = externalDiffFileId(savedPath);
        const std::string baseline = document->snapshot().text;

        std::optional<std::string> diskContent;
        bool unknownObservation = false;
        if (event.kind != WatchEventKind::Remove) {
            diskContent = readFileText(workspace.root() / event.path);
            if (!diskContent) {
                // Decision 2a: an unreadable/non-regular path where a file was is
                // Unknown. Never silently skip it -- raise it as a removal conflict,
                // so the buffer now orphaned from any regular file surfaces.
                unknownObservation = true;
                event.kind = WatchEventKind::Remove;
            }
        } else {
            // A queued ordinary Remove carries only the fact "removed" and is never
            // re-observed by the watcher. Between the emit and this processing the
            // path may have reappeared as a directory, an unreadable file, or a
            // regular file. Re-observe before trusting the Remove so a stale one
            // cannot match a Missing baseline and be silently skipped: a status
            // error or a present-but-non-regular/unreadable entry is Unknown (raise
            // through the same chokepoint), a present regular file is a real change
            // (raise as Modify), and only a still-genuine absence stays a Remove that
            // a Missing baseline suppresses. Runtime thread only.
            std::error_code linkCode;
            const auto linkStatus =
                std::filesystem::symlink_status(workspace.root() / event.path,
                                                linkCode);
            const bool statusError =
                linkStatus.type() == std::filesystem::file_type::none;
            if (statusError) {
                unknownObservation = true;
            } else if (std::filesystem::exists(linkStatus)) {
                auto reobserved = readFileText(workspace.root() / event.path);
                if (reobserved) {
                    diskContent = std::move(reobserved);
                    event.kind = WatchEventKind::Modify;
                } else {
                    unknownObservation = true;
                }
            }
        }

        // Decision 4: an event whose observed disk state equals the document's
        // external baseline is a change already adopted or dismissed (keep_buffer);
        // skip it so a duplicate/coalesced ordinary event does not re-raise a
        // dismissed conflict. A rename changes identity (handled by adoption) and an
        // Unknown observation never matches, so both fall through to processing. The
        // diff CONTENT stays buffer-vs-disk; only this raise/skip decision consults
        // the baseline.
        if (!unknownObservation && event.kind != WatchEventKind::Rename &&
            workspace.matchesExternalBaseline(*documentId, diskContent)) {
            continue;
        }

        // Seed the non-git entry the first time this file is observed, so openDiff
        // finds a file and applyNonGitEvent is not rejected (Decision 5). Its
        // revision, like the event's, is allocated from the shared DiffModel.
        bool seededHere = false;
        if (!diff.file(id).has_value()) {
            const Revision seedRevision{diff.viewState().revision.value() + 1};
            (void)diff.seedNonGit({{id, event.path, baseline}}, seedRevision);
            seededHere = true;
        }
        const Revision diffRevision{diff.viewState().revision.value() + 1};

        std::optional<JournalDocument> journal{JournalDocument{
            state->key, document->mode(), state->dirty, document->snapshot().text}};

        // The clean auto-reload and rename-adoption paths commit to the workspace
        // BEFORE the flow publishes the cleared/updated state (Decision 11): the
        // flow calls this and only adopts when it succeeds, so a failed workspace
        // commit leaves the prior published conflict rather than a stale buffer.
        // The commit decodes the RAW disk bytes through the document's encoding.
        bool committed = false;
        std::function<bool()> commitClean;
        std::function<bool()> commitConflictRename;
        if (event.kind == WatchEventKind::Rename) {
            commitClean = [&]() {
                const bool ok =
                    workspace
                        .adoptExternalRename(*documentId, savedPath,
                                             diskContent.value_or(std::string{}),
                                             /*replaceBuffer=*/true)
                        .accepted();
                committed = ok;
                return ok;
            };
            // A dirty rename keeps its buffer, but the document must still adopt
            // the new path's disk identity and baseline BEFORE the flow publishes
            // the conflict under the new-path id. The flow calls this and refuses
            // to publish when it fails, so a failed adoption never leaves an action
            // referencing a path no document owns.
            commitConflictRename = [&]() {
                const bool ok =
                    workspace
                        .adoptExternalRename(*documentId, savedPath,
                                             diskContent.value_or(std::string{}),
                                             /*replaceBuffer=*/false)
                        .accepted();
                committed = ok;
                return ok;
            };
        } else if (event.kind != WatchEventKind::Remove) {
            commitClean = [&]() {
                const bool ok =
                    workspace
                        .reloadWithContent(*documentId,
                                           diskContent.value_or(std::string{}))
                        .accepted();
                committed = ok;
                return ok;
            };
        }

        ExternalEventInput input{event, id, baseline, diskContent};
        if (event.kind == WatchEventKind::Rename && event.previousPath) {
            input.previousId =
                externalDiffFileId(event.previousPath->generic_string());
        }
        const auto result =
            resync ? external.processResyncEvent(std::move(input), diffRevision,
                                                 journal, commitClean,
                                                 commitConflictRename)
                   : external.processEvent(std::move(input), diffRevision,
                                           journal, commitClean,
                                           commitConflictRename);
        if (!result.accepted()) {
            // A rejected event must leave no diff entry behind. When this iteration
            // seeded the new-path entry (so openDiff would have a file), roll it back
            // so a failed rename-adoption -- or any rejected event -- never orphans a
            // diff entry keyed to a path no pending action owns.
            if (seededHere && diff.file(id).has_value()) {
                const Revision removalRevision{diff.viewState().revision.value() +
                                               1};
                (void)diff.removeFile(id, removalRevision);
            }
            continue;
        }

        // A rename changes the namespaced id; retire the stale diff entry keyed by
        // the old path so a second, orphaned entry is not left behind (Decision 10).
        if (event.kind == WatchEventKind::Rename && event.previousPath) {
            const DiffFileId previousId =
                externalDiffFileId(event.previousPath->generic_string());
            if (previousId != id && diff.file(previousId).has_value()) {
                const Revision removalRevision{diff.viewState().revision.value() +
                                               1};
                (void)diff.removeFile(previousId, removalRevision);
            }
        }

        if (committed) {
            (void)updateTabsFor(*documentId);
        }
    }
    // The session revision must advance whenever the drain moved the published
    // state a delta client observes -- not only when the flow's own view changed.
    // A rejected event that seeded then rolled back a diff entry leaves the shared
    // DiffModel revision net-advanced with the flow's view unchanged; the diff
    // section a client sees is keyed to that revision, so the session revision must
    // track it or a delta client could miss or mis-order the change.
    if (session && (external.viewState().revision != flowRevisionBefore ||
                    diff.viewState().revision != diffRevisionBefore)) {
        session->advanceRevision();
    }
    // External state changes here in the watcher drain, not only on a command
    // dispatch (Decision 4): reconcile the section's presence into the interaction
    // authority whenever the flow's view advanced, so the node appears/updates
    // without waiting for an unrelated command.
    if (external.viewState().revision != flowRevisionBefore) {
        interaction.refreshExternalModificationPresence(
            externalModificationPresent());
    }
}

bool EditorSession::Impl::commitExternalDismissal(
    FileDocumentId document, bool removed,
    const std::optional<std::string>& dismissedContent) {
    return workspace.commitExternalDismissal(
        document, removed, dismissedContent,
        [&](const std::optional<DraftBaseline>& newBaseline) -> bool {
            // Decision 5: an already-persisted draft record still carries the
            // pre-dismissal baseline; refresh it to the dismissed state so a
            // crash-reopen classifies Unchanged instead of resurrecting the
            // conflict via draft recovery. No persisted record: nothing to do.
            const auto state = workspace.state(document);
            if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
                return true;
            }
            const auto drafts = scratch.recovery().documents;
            const auto draft = std::find_if(
                drafts.begin(), drafts.end(),
                [&](const JournalDocument& candidate) {
                    return candidate.dirty && candidate.key == state->key;
                });
            if (draft == drafts.end()) return true;
            scratch.updateDocument(JournalDocument{draft->key, draft->mode,
                                                   draft->dirty,
                                                   draft->utf8Content,
                                                   newBaseline});
            return true;
        });
}

void EditorSession::Impl::reconcileAllOpenDocumentsAgainstDisk() {
    std::vector<WatchEvent> synthesized;
    std::uint64_t sequence = 0;
    for (const auto documentId : workspace.documents()) {
        const auto state = workspace.state(documentId);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
            continue;
        }
        const std::filesystem::path relative{state->key.savedPath()};
        const auto absolute = workspace.root() / relative;
        std::error_code linkCode;
        const auto linkStatus = std::filesystem::symlink_status(absolute, linkCode);
        WatchEvent event;
        event.path = relative;
        event.origin = WatchEventOrigin::External;
        event.sequence = ++sequence;
        // file_type::none is an indeterminate status (permission denied, I/O error);
        // file_type::not_found is a determinate clean absence. Only the former is a
        // status error.
        if (linkStatus.type() == std::filesystem::file_type::none) {
            // Decision 2a: a status/stat error is Unknown, not a clean absence -- it
            // must never become a Missing-matchable Remove the resync could suppress.
            // Synthesize a Modify so the shared reconcile re-reads, fails, and marks
            // the observation Unknown, which always raises. This converges on the one
            // Unknown-detection chokepoint.
            event.kind = WatchEventKind::Modify;
            synthesized.push_back(std::move(event));
            continue;
        }
        // The path entry itself is present (a regular file, a directory, or even a
        // broken symlink) -- distinct from exists(), which follows the link and is
        // false for a broken symlink, indistinguishable there from a true absence.
        const bool entryPresent = std::filesystem::exists(linkStatus);
        std::error_code code;
        const bool exists = std::filesystem::exists(absolute, code) && !code;
        if (!entryPresent) {
            // A genuine absence. A dismissed removal (keep_buffer set the baseline
            // Missing) must not be re-raised by the resync; only a still-differing
            // absence raises.
            if (workspace.matchesExternalBaseline(documentId, std::nullopt)) {
                continue;
            }
            event.kind = WatchEventKind::Remove;
            synthesized.push_back(std::move(event));
            continue;
        }
        // The entry is present. If it is a readable regular file whose content
        // matches the baseline, it did not change during the overflow window and
        // needs no event (and a dirty document must not be told its unchanged disk
        // file was modified). Otherwise -- a changed file OR an Unknown observation
        // (unreadable, non-regular, or broken symlink) -- synthesize a Modify. The
        // shared reconcile re-reads it; on a failed read it marks the observation
        // Unknown, which ALWAYS raises and never matches a Missing baseline, so an
        // Unknown never collapses into a Remove the ordinary reconcile could
        // suppress. Both the ordinary and overflow paths thus converge on the one
        // Unknown-detection chokepoint (Decision 2a).
        const auto disk = exists ? readFileText(absolute) : std::nullopt;
        if (disk && workspace.matchesExternalBaseline(documentId, *disk)) {
            continue;
        }
        event.kind = WatchEventKind::Modify;
        synthesized.push_back(std::move(event));
    }
    if (!synthesized.empty()) {
        reconcileExternalWatchEvents(std::move(synthesized), /*resync=*/true);
    }
}

bool EditorSession::Impl::archiveDiscardedDraft(std::string_view savedPath,
                                                std::string_view content) {
    // Beside the scratch store (not the workspace deleted-file archive, which
    // the housekeeping pruner owns), so a discarded draft is never pruned as a
    // stale deleted file.
    const auto archiveDir = scratchRoot.parent_path() / "draft-archive";
    std::error_code code;
    std::filesystem::create_directories(archiveDir, code);
    if (code) return false;

    // Name by a HASH of the workspace-relative path rather than the flattened
    // path itself: a legal deep path can exceed a filesystem's per-component
    // name limit (255 bytes on Linux), which would make discard fail for a valid
    // file. A short basename prefix stays for humans browsing the archive; the
    // hash disambiguates two files sharing a basename, and the nanosecond stamp
    // keeps repeated discards of one file distinct.
    std::string basename =
        std::filesystem::path{std::string{savedPath}}.filename().string();
    if (basename.empty()) basename = "draft";
    if (basename.size() > 64) basename.resize(64);
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto target =
        archiveDir / (basename + "." + std::to_string(fastContentHash(savedPath)) +
                      "." + std::to_string(stamp) + ".draft");
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(content.data()), content.size()};
    return createFileExclusively(target, bytes).ok();
}

CommandHandlerResult EditorSession::Impl::discardDraft() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto state = workspace.state(*id);
    if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure("draft.discard needs a saved file");
    }
    if (!state->dirty) return failure("no unsaved edits to discard");
    const auto* opened = workspace.tryDocument(*id);
    if (opened == nullptr) return failure("no active document");
    const std::string draftText = opened->snapshot().text;

    // Archive the discarded edits FIRST, before anything is removed or the
    // buffer is reloaded: even if the reload fails, the draft survives here and
    // in the scratch store, so a mis-click is always recoverable.
    if (!archiveDiscardedDraft(state->key.savedPath(), draftText)) {
        return failure("could not archive the draft before discarding");
    }

    // Reload the on-disk content, which refreshes the baseline and clears
    // dirty. A missing or unreadable file leaves the draft untouched.
    const auto reloaded = workspace.reload(*id);
    if (!reloaded.accepted()) return failure("could not load the file from disk");

    scratch.removeDocument(state->key);
    // "Use disk" is meant to be final. removeDocument is queued to the async
    // durability thread, so wait briefly (as tab close does) to shrink the
    // window where a crash could replay the just-discarded draft on next launch.
    // Best-effort: the archived copy already makes a lost race recoverable.
    (void)scratch.waitUntilDurable(std::chrono::milliseconds{100});
    if (const auto found = documentRuntimeStates.find(id->value());
        found != documentRuntimeStates.end()) {
        found->second.reopen = DraftReopenOutcome::None;
    }
    resetSelectionForActiveDocument();
    refreshSyntax();
    return updateTabsFor(*id);
}

CommandHandlerResult EditorSession::Impl::dismissDraftNotice() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto found = documentRuntimeStates.find(id->value());
    if (found == documentRuntimeStates.end() ||
        found->second.reopen != DraftReopenOutcome::Conflict) {
        // Only a Conflict raises the notice; Restored/None show nothing to
        // dismiss, so dismissing them would silently mutate non-notice state.
        return failure("no draft notice to dismiss");
    }
    found->second.reopen = DraftReopenOutcome::None;
    return success();
}

void EditorSession::Impl::refreshLiveDiffDocuments(const DiffViewState& diffView) {
    for (auto it = liveDiffDocuments.begin(); it != liveDiffDocuments.end();) {
        const auto id = DiffFileId{it->first};
        auto file = std::find_if(
            diffView.files.begin(), diffView.files.end(),
            [&](const DiffFileView& candidate) { return candidate.id == id; });
        const auto desired =
            file == diffView.files.end() ? std::string{}
                                         : liveDiffDocumentText(*file);
        const auto document = it->second;
        const auto* opened = workspace.tryDocument(document);
        if (opened == nullptr) {
            it = liveDiffDocuments.erase(it);
            continue;
        }
        if (opened->snapshot().text == desired) {
            ++it;
            continue;
        }
        auto state = workspace.state(document);
        const auto label =
            state ? state->displayLabel : std::string{"LiveDiff"};
        auto replacement = workspace.openVirtualDocument(
            label, desired, DocumentMode::Diff);
        if (!replacement.accepted() || !replacement.document) {
            continue;
        }
        it->second = *replacement.document;
        ensureDocumentRuntimeState(*replacement.document);
        auto removed = workspace.removeDocument(document);
        if (removed.accepted()) {
            documentRuntimeStates.erase(document.value());
        }
        ++it;
    }
}

bool EditorSession::Impl::openOrRevealFollowTargetProgrammatic(
    const FollowTarget& target) {
    const auto file = diff.file(target.id);
    if (!file.has_value()) {
        return false;
    }
    if (!openOrFocusLiveDiffTab(file->get(), NavigationClass::Programmatic,
                                std::nullopt)
             .accepted) {
        return false;
    }
    if (target.deleted) {
        return true;
    }
    return revealCurrentDiffTarget(target, NavigationClass::Programmatic);
}

Document const* EditorSession::Impl::activeDocument() const {
    auto id = activeDocumentId();
    return id ? workspace.tryDocument(*id) : nullptr;
}

Document* EditorSession::Impl::activeDocument() {
    auto id = activeDocumentId();
    return id ? const_cast<Document*>(workspace.tryDocument(*id)) : nullptr;
}

void EditorSession::Impl::ensureDocumentRuntimeState(FileDocumentId document) {
    documentRuntimeStates.try_emplace(
        document.value(),
        DocumentRuntimeState{HistoryConfig::defaults(), syntaxParser});
}

void EditorSession::Impl::discardDocumentRuntimeState(FileDocumentId document) {
    documentRuntimeStates.erase(document.value());
    documentLanguageOverrides.erase(document.value());
    autosave.forget(document);
    // A find that was scoped to this document no longer has a subject.
    if (findDocumentId == document) findDocumentId.reset();
    // Any live diff tab mapped to it is equally orphaned.
    for (auto it = liveDiffDocuments.begin(); it != liveDiffDocuments.end();) {
        it = it->second == document ? liveDiffDocuments.erase(it)
                                    : std::next(it);
    }
    // Same for a read-only output (help) tab mapped to it.
    for (auto it = readOnlyTabDocuments.begin();
         it != readOnlyTabDocuments.end();) {
        it = it->second == document ? readOnlyTabDocuments.erase(it)
                                    : std::next(it);
    }
}

DocumentHistory& EditorSession::Impl::historyFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{
            "document history was requested before document runtime state existed"};
    }
    return it->second.history;
}

SyntaxModel& EditorSession::Impl::syntaxFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{
            "document syntax was requested before document runtime state existed"};
    }
    return it->second.syntax;
}

SyntaxViewState EditorSession::Impl::activeSyntaxView() const {
    if (auto id = activeDocumentId()) {
        if (auto it = documentRuntimeStates.find(id->value());
            it != documentRuntimeStates.end()) {
            return it->second.syntax.viewState();
        }
    }
    const auto* document = activeDocument();
    const auto text = document ? document->snapshot().text : std::string{};
    const auto revision = document ? document->revision() : Revision{0};
    return SyntaxViewState::plainText(revision, LanguageId::plainText(), text, 4);
}

std::optional<WorkspaceDocumentState> EditorSession::Impl::activeWorkspaceState() const {
    auto id = activeDocumentId();
    return id ? workspace.state(*id) : std::nullopt;
}

std::optional<DiffFileView> EditorSession::Impl::activeDiffFile() const {
    const auto diffState = diff.viewState();
    const auto file = diffState.fileForDocument(documentView());
    return file ? std::optional<DiffFileView>{file->get()} : std::nullopt;
}

std::string const& EditorSession::Impl::activeText() const {
    static const std::string empty;
    const auto documentId = activeDocumentId();
    auto const* document = activeDocument();
    if (!documentId || document == nullptr) return empty;
    const auto revision = document->revision();
    if (activeTextDocument != documentId || activeTextRevision != revision) {
        activeTextCache = document->snapshot().text;
        activeTextDocument = documentId;
        activeTextRevision = revision;
    }
    return activeTextCache;
}

int EditorSession::Impl::lineNumberGutterWidth() const {
    if (!lineNumbers) return 0;
    auto const* document = activeDocument();
    if (document == nullptr) return 0;
    auto const revision = document->revision();
    auto const documentId = activeDocumentId();
    if (!lineCountRevision || *lineCountRevision != revision ||
        lineCountDocument != documentId) {
        auto const text = document->snapshot().text;
        std::uint32_t lines = 1;
        for (char c : text) {
            if (c == '\n') ++lines;
        }
        lineCountCache = lines;
        lineCountRevision = revision;
        lineCountDocument = documentId;
    }
    return static_cast<int>(std::to_string(lineCountCache).size()) + 1;
}

void EditorSession::Impl::resetSelectionForActiveDocument() {
    selection = initialSelection();
    for (auto& [_, view] : viewPresentations) {
        view.requestedFirstVisualRow = 0;
        view.requestedFirstVisualColumn = 0;
        view.desiredCell.reset();
        view.viewportLineCache = LineLayoutCache{};
    }
}

void EditorSession::Impl::clampSelectionToActiveDocument() {
    auto const& text = activeText();
    auto offset = selection.selections.primary().active.byteOffset.value();
    if (offset > text.size()) offset = text.size();
    auto position = ssg::SelectionNavigator::resolvePosition(text, ByteOffset{offset}).value_or(zeroPosition());
    selection.selections = SelectionSet{std::vector<Selection>{Selection{position, position}}};
}

void EditorSession::Impl::clampSelectionsToActiveDocument() {
    auto const& text = activeText();
    auto clampPosition = [&](DocumentPosition const& p) {
        auto offset = p.byteOffset.value();
        if (offset > text.size()) offset = text.size();
        // Prefer the exact offset; if it is not a grapheme boundary (only
        // possible for a selection carried from a differently-shaped document,
        // not for the edit paths this serves), snap DOWN to the nearest boundary
        // at or below it rather than teleporting to the document end.
        for (;;) {
            if (auto at = ssg::SelectionNavigator::resolvePosition(
                    text, ByteOffset{offset})) {
                return *at;
            }
            if (offset == 0) break;
            --offset;
        }
        return zeroPosition();
    };
    std::vector<Selection> clamped;
    clamped.reserve(selection.selections.items().size());
    for (auto const& sel : selection.selections.items()) {
        clamped.push_back(
            Selection{clampPosition(sel.anchor), clampPosition(sel.active)});
    }
    if (clamped.empty()) {
        clamped.push_back(Selection{zeroPosition(), zeroPosition()});
    }
    selection.selections = SelectionSet{std::move(clamped)};
}

const std::vector<CellRun>& EditorSession::Impl::activeCellRuns() const {
    auto const* document = activeDocument();
    auto const documentId = activeDocumentId();
    auto const revision = document ? document->revision() : Revision{0};
    if (cellRunsRevision && *cellRunsRevision == revision &&
        cellRunsDocument == documentId) {
        return cellRunsCache;
    }
    std::string const text = document ? document->snapshot().text : std::string{};
    std::vector<CellRun> runs;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? end : end - start);
        runs.push_back(GraphemeLayout{}.computeRun(line, 4));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (runs.empty()) runs.push_back(GraphemeLayout{}.computeRun("", 4));
    cellRunsCache = std::move(runs);
    cellRunsRevision = revision;
    cellRunsDocument = documentId;
    return cellRunsCache;
}

ViewportViewState EditorSession::Impl::computeEditorViewport(
    ViewPresentationState& presentation, std::uint32_t firstRow,
    std::uint32_t firstColumn) const {
    auto const dimensions = presentation.dimensions;
    const auto diffFile = activeDiffFile();
    // Scroll against the region the editor actually PAINTS, not the terminal's
    // full surface.  The shell spends rows on the header, the tab bar, the
    // footer and any reserved prompt, and columns on the sidebar; a viewport
    // sized to the whole terminal overshoots by exactly that much.  Vertically
    // its maximum scroll offset leaves the last few lines permanently
    // unreachable and it reports no scrollbar for a document that is in fact
    // clipped; horizontally it breaks wrapped lines past the right edge of the
    // pane, so the tail is painted nowhere. The retained pane content size is
    // computed -- the same numbers page-up/page-down already scroll by.
    //
    // Clamped to the client surface because a terminal too small to lay out at
    // all leaves that cache holding the last good layout's value, which would
    // otherwise size the viewport larger than the screen.
    ViewportDimensions const content{
        std::max<std::uint32_t>(
            1, std::min<std::uint32_t>(presentation.paneContentColumns,
                                       dimensions.columns)),
        std::max<std::uint32_t>(
            1, std::min<std::uint32_t>(presentation.paneContentRows,
                                       dimensions.rows))};
    auto const view =
        wordWrap
            ? Viewport{}.compute(activeCellRuns(), content, firstRow,
                                 diffFile ? &*diffFile : nullptr, dimensions)
            // Word wrap off (default): one logical line is one visual row; only
            // the visible lines are segmented, so this is O(visible rows), not
            // O(document).
            : Viewport{}.computeUnwrapped(activeText(), content, firstRow,
                                          firstColumn, 4,
                                          diffFile ? &*diffFile : nullptr,
                                          dimensions,
                                          &presentation.viewportLineCache);
    return view;
}

ViewportViewState EditorSession::Impl::viewport(
    ViewPresentationState& presentation) const {
    return computeEditorViewport(
        presentation, presentation.requestedFirstVisualRow,
        presentation.requestedFirstVisualColumn);
}

bool EditorSession::Impl::refreshTree() {
    if (deferringEnrichment) {
        pendingTreeRefresh = true;
        return false;
    }
    ++treeScanCount;
    tree.replaceProvider(TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"filesystem"}, root, interaction.allocateTreeRevision()));
    rebuildFileCandidates();
    return true;
}

void EditorSession::Impl::refreshTreeForPublication(
    Revision drainEntryRevision) {
    if (refreshTree() && session &&
        session->revision() == drainEntryRevision) {
        session->advanceRevision();
    }
}

void EditorSession::Impl::rebuildInteractionSchema(
    const StyleDimensions& dimensions,
    std::string_view promptSigil,
    const std::optional<ValidatedComposition>& composed) {
    (void)interaction.updateComposition(
        assembleWholeScreen(statusFieldCatalog, "help.open", dimensions,
                            promptSigil, composed));
}

bool EditorSession::Impl::openPickerPrompt(PickerKind kind) {
    return interaction.apply(OpenFinder{kind});
}

// The index opens its OWN repository handle rather than sharing the git-diff
// worker's: that one is owned by its thread, and libgit2 handles are not safe to
// use from two threads.
void EditorSession::Impl::rebuildFileCandidates() {
    auto matcher = makePlatformGitIgnoreMatcher(root);
    WorkspaceFileIndexOptions options;
    options.respectGitignore =
        boolSetting(settings, SettingKey::FileFinderRespectGitignore, true);
    fileCandidates =
        std::move(WorkspaceFileIndex{}.build(root, *matcher, options).candidates);
}

void EditorSession::Impl::reconcileFindDocument() {
    if (!findReplace.viewState().open) {
        findDocumentId.reset();
        return;
    }
    auto const active = activeDocumentId();
    auto const* document = activeDocument();
    bool const stale =
        !active || active != findDocumentId || document == nullptr ||
        document->snapshot().revision != findReplace.viewState().sourceRevision;
    if (!stale) return;
    // The document the find evaluated against is gone, changed, or was edited:
    // close the controller and dismiss its prompt so no stale match is navigable.
    findReplace.close();
    if (auto const& request = interaction.prompt().request();
        request && (request->kind == PromptKind::Find ||
                    request->kind == PromptKind::Replace)) {
        (void)interaction.cancelPrompt();
    }
    findDocumentId.reset();
}

void EditorSession::Impl::refreshSyntax(std::vector<SyntaxEdit> edits) {
    auto id = activeDocumentId();
    if (!id) return;
    auto& model = syntaxFor(*id);
    auto const* document = activeDocument();
    auto text = document ? document->snapshot().text : std::string{};
    auto revision = document ? document->revision() : Revision{0};
    auto language = LanguageId::plainText();
    if (auto const override = documentLanguageOverrides.find(id->value());
        override != documentLanguageOverrides.end()) {
        language = override->second;
    } else if (auto state = activeWorkspaceState();
               state && state->key.kind() == JournalDocumentKeyKind::Saved) {
        language = LanguageId::fromPath(state->key.savedPath());
    }
    if (deferringEnrichment) {
        const bool canEagerlyParse =
            document != nullptr && model.hasGrammar(language) &&
            text.size() <= kEagerSyntaxMaxBytes;
        if (!canEagerlyParse) {
            pendingSyntaxRefresh = true;
            return;
        }
    }
    ++syntaxRunCount;
    if (!model.canIncrementallyParse(language)) edits.clear();
    (void)model.parse(revision, std::move(language), std::move(text),
                      std::move(edits));
}

void EditorSession::Impl::primeDeferred() {
    if (!deferringEnrichment) return;
    deferringEnrichment = false;
    // Run whichever scans were requested while deferring, now that the first
    // frame is drawn.  Order: tree then syntax (independent; both publish through
    // the normal snapshot channel on the next snapshot).
    bool ran = false;
    if (pendingTreeRefresh) {
        pendingTreeRefresh = false;
        (void)refreshTree();
        ran = true;
    }
    if (pendingSyntaxRefresh) {
        pendingSyntaxRefresh = false;
        refreshSyntax();
        ran = true;
    }
    // Advance the session revision so delta-based clients observe the primed
    // enrichment; a same-revision snapshot pair yields no delta (derive_session_
    // delta rejects it), so without this a WebSocket client would miss it.
    if (ran && session) session->advanceRevision();
}

void EditorSession::Impl::enqueueStatus(StatusPriority priority, std::string text) {
    auto value = nextStatusId++;
    (void)status.enqueue(StatusItem{StatusId{value}, priority, std::move(text), {}});
}

namespace {

std::vector<AutosaveCandidate> autosaveCandidates(const Workspace& workspace) {
    std::vector<AutosaveCandidate> candidates;
    for (const auto id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state) continue;
        auto const* current = workspace.tryDocument(id);
        if (current == nullptr) continue;
        // Only an editable document can hold unsaved user edits worth a draft. A
        // live-diff tab's virtual document (DocumentMode::Diff) is a derived view
        // that is untitled and non-empty, so it would otherwise read as a dirty
        // untitled buffer and be persisted as a spurious scratch draft.
        if (current->mode() != DocumentMode::Edit) continue;
        // Only a dirty document is a flush candidate, so only a dirty document
        // pays for a text snapshot + hash. A clean one still appears (hash 0) so
        // the scheduler can drop any debounce state it held — cheap, no copy.
        std::uint64_t contentHash = 0;
        if (state->dirty) {
            contentHash = fastContentHash(current->snapshot().text);
        }
        candidates.push_back(AutosaveCandidate{id, state->dirty, contentHash});
    }
    return candidates;
}

} // namespace

std::size_t EditorSession::Impl::persistAutosaveDraft(FileDocumentId document) {
    auto state = workspace.state(document);
    auto const* current = workspace.tryDocument(document);
    if (!state || current == nullptr) return 0;

    // A buffer too large to draft gets NO draft (and thus no crash-safety),
    // reported rather than silently written: a giant draft would blow the
    // scratch quota and stall fsync, and a stale partial draft would be false
    // reassurance. Remove any earlier draft for the key so the on-disk state is
    // honestly "no draft", and warn once.
    const auto& text = current->snapshot().text;
    if (text.size() > autosaveDraftByteCap) {
        // Report and drop any prior draft exactly ONCE per over-cap episode:
        // the reported flag gates the whole block so a long oversized edit
        // session does not enqueue a no-op journal remove on every flush tick.
        if (const auto found = documentRuntimeStates.find(document.value());
            found != documentRuntimeStates.end() &&
            !found->second.autosaveOversizeReported) {
            found->second.autosaveOversizeReported = true;
            scratch.removeDocument(state->key);
            enqueueStatus(StatusPriority::Warning,
                          "file is too large to autosave a draft; unsaved edits "
                          "are not crash-protected until saved");
        }
        return 0;
    }
    if (const auto found = documentRuntimeStates.find(document.value());
        found != documentRuntimeStates.end()) {
        found->second.autosaveOversizeReported = false;
    }

    // Identical JournalDocument to the tab-close path, minus the blocking
    // durability wait: autosave leaves fsync to the background thread so a tick
    // never stalls the UI (the recovery badge still reports pending/durable).
    scratch.updateDocument(JournalDocument{state->key, current->mode(),
                                           state->dirty, text,
                                           workspace.baselineFor(document)});
    return 1;
}

void EditorSession::Impl::reconcileDraftOnOpen(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) return;

    const auto drafts = scratch.recovery().documents;
    const auto draft = std::find_if(
        drafts.begin(), drafts.end(), [&](const JournalDocument& candidate) {
            return candidate.dirty && candidate.key == state->key;
        });
    if (draft == drafts.end()) return;

    auto const* opened = workspace.tryDocument(document);
    auto rawDisk = workspace.rawDiskContent(document);
    if (opened == nullptr || !rawDisk) return;
    const DraftDiskState disk{std::move(*rawDisk), opened->snapshot().text};

    auto& runtimeState = documentRuntimeStates.at(document.value());
    switch (DraftReopenClassifier{}.classify(draft->baseline,
                                             draft->utf8Content, disk)) {
        case DraftReopenClass::Converged:
            // The edits equal disk (or were undone): nothing to recover. Drop the
            // draft and keep the clean disk buffer already open.
            scratch.removeDocument(draft->key);
            runtimeState.reopen = DraftReopenOutcome::None;
            return;
        case DraftReopenClass::Unchanged:
            // Disk is unchanged since the edits branched: load the draft dirty.
            // If it cannot be loaded (disk is now binary/undecodable, so the
            // buffer is read-only), do NOT drop it silently -- keep it in scratch
            // and raise the conflict notice so the user is warned, never falsely
            // reassured that nothing needs attention.
            if (workspace.restoreDraft(document, draft->utf8Content)) {
                runtimeState.reopen = DraftReopenOutcome::Restored;
            } else {
                runtimeState.reopen = DraftReopenOutcome::Conflict;
            }
            return;
        case DraftReopenClass::Conflict:
            // Best-effort load; the draft stays in scratch whether or not the
            // buffer can hold it (a binary/undecodable disk file yields a
            // read-only buffer). Either way the conflict notice is raised, so the
            // draft is never silently lost.
            (void)workspace.restoreDraft(document, draft->utf8Content);
            runtimeState.reopen = DraftReopenOutcome::Conflict;
            return;
        case DraftReopenClass::Missing:
            // Unreachable via file.open (the file was just read from disk), so a
            // missing disk file here means the classifier's contract changed;
            // leave the clean buffer rather than guess.
            return;
    }
}

std::size_t EditorSession::Impl::flushDueAutosaveDrafts() {
    autosave.setInterval(std::chrono::milliseconds{
        uint32Setting(settings, SettingKey::AutosaveDebounceMs, 10000)});
    const auto candidates = autosaveCandidates(workspace);
    std::size_t flushed = 0;
    for (const auto id :
         autosave.due(std::chrono::steady_clock::now(), candidates)) {
        flushed += persistAutosaveDraft(id);
    }
    return flushed;
}

std::size_t EditorSession::Impl::flushAllAutosaveDrafts() {
    const auto candidates = autosaveCandidates(workspace);
    std::size_t flushed = 0;
    for (const auto id :
         autosave.flushAll(std::chrono::steady_clock::now(), candidates)) {
        flushed += persistAutosaveDraft(id);
    }
    return flushed;
}

ExternalDiffBurstResult EditorSession::Impl::applyExternalDiffBurst(
    std::vector<ExternalDiffRevision> changes) {
    if (changes.empty()) {
        return {ExternalDiffBurstError::EmptyBurst};
    }

    auto stagedDiff = diff;
    std::vector<FollowDiffChange> followChanges;
    followChanges.reserve(changes.size());
    for (auto& change : changes) {
        const auto id = change.event.id;
        std::vector<DiffHunk> priorHunks;
        if (const auto prior = stagedDiff.file(id)) {
            priorHunks = prior->get().hunks;
        }
        const auto applied =
            stagedDiff.applyNonGitEvent(std::move(change.event), change.revision);
        if (!applied.accepted()) {
            return {ExternalDiffBurstError::DiffRejected};
        }
        const auto changedFile = stagedDiff.file(id);
        if (!changedFile) {
            return {ExternalDiffBurstError::DiffRejected};
        }
        followChanges.push_back(
            {changedFile->get(), std::move(priorHunks), change.revision});
    }

    auto stagedFollow = follow;
    const auto followed =
        stagedFollow.acceptExternalChanges(std::move(followChanges));
    if (!followed.accepted()) {
        return {ExternalDiffBurstError::FollowRejected};
    }

    const auto previousTarget = follow.viewState().activeTarget;
    diff = std::move(stagedDiff);
    follow = std::move(stagedFollow);
    const auto next = follow.viewState();
    if (next.mode == FollowMode::Following && next.activeTarget &&
        next.activeTarget != previousTarget) {
        (void)openOrRevealFollowTargetProgrammatic(*next.activeTarget);
    }
    if (session) {
        session->advanceRevision();
    }
    return {};
}

GitDiffScanResult EditorSession::Impl::applyGitDiffScan(GitDiffScan scan) {
    if (scan.revision.value() == 0) {
        auto const previousBranch = currentGitBranch;
        currentGitBranch = scan.currentBranch;
        if (currentGitBranch != previousBranch && session) {
            session->advanceRevision();
        }
        return {};
    }
    if (scan.revision <= lastGitScanRevision) {
        return {GitDiffScanError::DiffRejected};
    }
    currentGitBranch = scan.currentBranch;
    auto gitRecords = gitTreeRecordsFromScan(scan.files);
    auto stagedDiff = diff;
    auto stagedFollow = follow;
    std::vector<FollowDiffChange> followChanges;
    followChanges.reserve(scan.files.size() + stagedDiff.viewState().files.size());
    bool mutated = false;
    std::vector<DiffFileId> statusOnlyIds;

    Revision nextRevision = Revision{stagedDiff.viewState().revision.value() + 1};
    const auto nextMutationRevision = [&nextRevision]() {
        auto current = nextRevision;
        nextRevision = Revision{nextRevision.value() + 1};
        return current;
    };
    const auto removeDetailedFile =
        [&](const DiffFileId& id) -> GitDiffScanResult {
        const auto prior = stagedDiff.file(id);
        if (!prior || !stagedDiff.isGitFile(id)) {
            return {};
        }
        auto removedFile = prior->get();
        auto priorHunks = removedFile.hunks;
        const auto revision = nextMutationRevision();
        const auto removed = stagedDiff.removeFile(id, revision);
        if (!removed.accepted()) {
            return {GitDiffScanError::DiffRejected};
        }
        mutated = true;
        removedFile.deleted = true;
        removedFile.currentContent.clear();
        removedFile.hunks.clear();
        removedFile.changedLines.clear();
        followChanges.push_back(
            {std::move(removedFile), std::move(priorHunks), revision});
        return {};
    };

    std::vector<DiffFileId> scannedIds;
    scannedIds.reserve(scan.files.size());
    for (auto& file : scan.files) {
        scannedIds.push_back(file.id);
        std::vector<DiffHunk> priorHunks;
        if (const auto prior = stagedDiff.file(file.id)) {
            priorHunks = prior->get().hunks;
        }
        const auto revision = nextMutationRevision();
        const auto applied = stagedDiff.updateGitFile(
            {.id = file.id,
             .path = file.path,
             .previousPath = file.previousPath,
             .baselineContent = std::move(file.baselineContent),
             .workingContent = std::move(file.workingContent),
             .baselineIdentity = scan.baselineIdentity},
            revision);
        if (!applied.accepted()) {
            if (applied.error != DiffError::WorkLimitExceeded) {
                return {GitDiffScanError::DiffRejected};
            }
            statusOnlyIds.push_back(file.id);
            if (auto removed = removeDetailedFile(file.id);
                !removed.accepted()) {
                return removed;
            }
            continue;
        }
        const auto changedFile = stagedDiff.file(file.id);
        if (!changedFile) {
            return {GitDiffScanError::DiffRejected};
        }
        mutated = true;
        followChanges.push_back(
            {changedFile->get(), std::move(priorHunks), revision});
    }

    const auto stagedView = stagedDiff.viewState();
    for (const auto& file : stagedView.files) {
        if (std::find(scannedIds.begin(), scannedIds.end(), file.id) !=
            scannedIds.end()) {
            continue;
        }
        // A git rescan reconciles only git-source entries. A non-git entry (a
        // draft-vs-disk diff, or an external-modification view) is owned by a
        // different flow and must survive a scan that simply does not mention
        // it, rather than being evicted as "no longer changed".
        if (!stagedDiff.isGitFile(file.id)) {
            continue;
        }
        if (auto removed = removeDetailedFile(file.id); !removed.accepted()) {
            return removed;
        }
    }

    if (mutated) {
        const auto followed =
            stagedFollow.acceptExternalChanges(std::move(followChanges));
        if (!followed.accepted()) {
            return {GitDiffScanError::FollowRejected};
        }

        const auto previousTarget = follow.viewState().activeTarget;
        diff = std::move(stagedDiff);
        follow = std::move(stagedFollow);
        for (const auto& id : statusOnlyIds) {
            std::optional<TabId> liveTab;
            for (const auto& tab : tabs.viewState().tabs) {
                if (tab.kind == TabKind::LiveDiff &&
                    tab.contentIdentity == id.value()) {
                    liveTab = tab.id;
                    break;
                }
            }
            if (liveTab) {
                (void)tabs.close(*liveTab, std::chrono::milliseconds{100});
            }
        }
        refreshLiveDiffDocuments(diff.viewState());
        const auto next = follow.viewState();
        if (next.mode == FollowMode::Following && next.activeTarget &&
            next.activeTarget != previousTarget) {
            (void)openOrRevealFollowTargetProgrammatic(*next.activeTarget);
        }
    }
    tree.replaceProvider(TreeProviderSnapshot::fromGit(
        TreeProviderId{"git"}, interaction.allocateTreeRevision(),
        std::move(gitRecords)));
    lastGitScanRevision = scan.revision;
    if (session) {
        session->advanceRevision();
    }
    return {};
}

bool EditorSession::Impl::revealCurrentDiffTarget(
    const FollowTarget& target, NavigationClass classification) {
    const auto& text = activeText();
    const auto offset = lineStartOffset(text, target.newestHunkLine);
    const auto position =
        SelectionNavigator::resolvePosition(text, ByteOffset{offset});
    if (!position) {
        return false;
    }
    selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*position, *position}}};
    std::optional<std::uint32_t> targetRow;
    if (const auto file = diff.file(target.id)) {
        const auto projection =
            Viewport{}.rowProjectionUnwrapped(text, file->get());
        targetRow = projection.visualRowForBufferLine(
            static_cast<std::uint32_t>(std::min<std::size_t>(
                target.newestHunkLine,
                std::numeric_limits<std::uint32_t>::max())));
    }
    for (const auto& client : follow.viewState().clients) {
        if (auto attached = clientViews.find(client.client);
            attached != clientViews.end()) {
            auto& view = presentation(attached->second);
            if (targetRow) {
                view.requestedFirstVisualRow = *targetRow;
            }
            revealPrimaryCaret(attached->second);
            recordNavigation(client.client, attached->second, classification);
        }
    }
    interaction.focusEditor();
    return true;
}

bool EditorSession::Impl::revealDiffTarget(
    const FollowTarget& target, NavigationClass classification) {
    if (target.deleted) {
        return false;
    }
    const auto opened = workspace.openFile(target.path.generic_string());
    if (!opened.accepted() || !opened.document ||
        !activateDocument(*opened.document).accepted) {
        return false;
    }
    return revealCurrentDiffTarget(target, classification);
}

void EditorSession::Impl::recordNavigation(
    ClientId client, ViewId viewId, NavigationClass classification) {
    auto const& view = presentation(viewId);
    (void)follow.applyNavigation(
        {.client = client,
         .classification = classification,
         .offset = FollowScrollOffset{view.requestedFirstVisualRow,
                                      view.requestedFirstVisualColumn}});
}

EditorSession::EditorSession(std::unique_ptr<Impl> implementation) noexcept
    : impl_{std::move(implementation)} {}
EditorSession::~EditorSession() = default;

EditorSession::Impl::ViewPresentationState&
EditorSession::Impl::presentation(ViewId viewId) {
    return viewPresentations.at(viewId);
}

EditorSession::Impl::ViewPresentationState const&
EditorSession::Impl::presentation(
    ViewId viewId) const {
    return viewPresentations.at(viewId);
}

void EditorSession::resetKeymapToDefault() {
    std::lock_guard operationLock{impl_->operationMutex};
    // defaultTerminalKeymap() is a fixed, already-construction-time-
    // validated value (see create() above), so no re-validation is needed
    // here -- resetting to it can never fail.
    impl_->keymap = defaultTerminalKeymap();
    ++impl_->keymapGeneration;
}

void EditorSession::focusEditor() {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->interaction.focusEditor();
}

void EditorSession::setComposedUi(std::optional<ValidatedComposition> composition) {
    std::lock_guard operationLock{impl_->operationMutex};
    // A composition-only reload (a script that just calls ssg.chrome, or one
    // that drops the call) runs outside command dispatch, so nothing else
    // advances the session revision. Delta-gated clients derive a frame only
    // from a revision change (derive_session_delta rejects a same-revision
    // pair), so bump on a real CHANGE -- and only then, to avoid a redundant
    // repaint when the host re-pushes an identical composition on every reload.
    if (impl_->composedUi == composition) return;
    // Migrate the schema over the new composition FIRST; adopt chrome truth only if it
    // succeeds, so a failure cannot leave composedUi/chromeGeneration ahead of the schema.
    impl_->rebuildInteractionSchema(impl_->style.dimensions,
                                    impl_->style.inputLineSigil, composition);
    impl_->composedUi = std::move(composition);
    ++impl_->chromeGeneration;
    if (impl_->session) impl_->session->advanceRevision();
}

EditorSessionCreateResult EditorSession::create(EditorSessionConfig config) {
    try {
        auto cwd = canonicalDirectory(config.cwd);
        if (config.scratchRoot.empty()) config.scratchRoot = cwd / ".ssg" / "scratch";
        if (config.recoveryRoot.empty()) config.recoveryRoot = cwd / ".ssg" / "recovery";
        if (config.archiveRoot.empty()) config.archiveRoot = cwd / ".ssg" / "archive";
        std::filesystem::create_directories(config.scratchRoot);
        std::filesystem::create_directories(config.recoveryRoot);
        // The archive root is deliberately NOT created here. Creating it eagerly
        // would materialise a `.ssg/` directory inside every workspace merely
        // for being opened -- visible in the file tree, and pointless for a
        // session that never deletes anything. FileArchive creates it on the
        // first delete instead.
        auto impl = std::make_unique<Impl>(cwd, config.scratchRoot,
                                           config.recoveryRoot,
                                           config.archiveRoot,
                                           config.deferEnrichment,
                                           std::move(config.syntaxParser),
                                           std::move(config.statusFieldProviders),
                                           config.enableGitDiffWorker,
                                           config.enableFilesystemWatcher);
        // Housekeeping at workspace open rather than on a timer, so it is
        // deterministic and testable. Its result is deliberately ignored: a
        // corrupt archive entry must never stop a user opening their workspace.
        (void)impl->workspace.pruneArchive();
        impl->keymap = defaultTerminalKeymap();        if (auto errors = KeymapMatcher{impl->keymap}.validate({}); !errors.empty()) {
            return {nullptr, "default keymap is invalid: " + errors.front().message};
        }
        if (!KeymapMatcher{impl->keymap}.hasGlobalBinding("settings.open", {})) {
            return {nullptr,
                    "default keymap lacks a global settings.open escape hatch"};
        }
        bindRuntimeEditing(*impl->catalog, *impl);
        bindRuntimeFiles(*impl->catalog, *impl);
        bindRuntimePresentation(*impl->catalog, *impl);
        bindRuntimeNavigation(*impl->catalog, *impl);
        bindRuntimeLanguageServices(*impl->catalog, *impl);
        bindRuntimeHelp(*impl->catalog, *impl);
        impl->session =
            std::make_unique<CommandExecutor>(impl->catalog, impl.get());
        return {std::unique_ptr<EditorSession>{new EditorSession{std::move(impl)}}, {}};
    } catch (std::exception const& exception) {
        return {nullptr, exception.what()};
    }
}

AttachResult EditorSession::attach(InvocationPrincipal principal, ViewId viewId) {
    std::lock_guard operationLock{impl_->operationMutex};
    auto clientId = principal.clientId();
    auto result = impl_->session->attach(std::move(principal), viewId);
    if (result.accepted()) {
        auto& references = impl_->viewReferences[viewId];
        if (references++ == 0) {
            impl_->viewPresentations.try_emplace(viewId);
        }
        impl_->clientViews.emplace(clientId, viewId);
        (void)impl_->follow.attachClient(
            clientId, impl_->presentation(viewId).dimensions);
    }
    return result;
}

bool EditorSession::detach(ClientId clientId) {
    std::lock_guard operationLock{impl_->operationMutex};
    auto attached = impl_->clientViews.find(clientId);
    (void)impl_->follow.detachClient(clientId);
    auto const detached = impl_->session->detach(clientId);
    if (detached && attached != impl_->clientViews.end()) {
        impl_->documentPointerGestures.erase(clientId);
        auto const viewId = attached->second;
        impl_->clientViews.erase(attached);
        auto references = impl_->viewReferences.find(viewId);
        if (references != impl_->viewReferences.end() &&
            --references->second == 0) {
            impl_->viewReferences.erase(references);
            impl_->viewPresentations.erase(viewId);
        }
    }
    return detached;
}

PumpResult EditorSession::pump() {
    if (impl_->session->activeDispatchRevision()) {
        throw std::logic_error{"worker results cannot be pumped during dispatch"};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    auto const before = impl_->session->revision();
    (void)impl_->drainGitDiffScans();
    auto const after = impl_->session->revision();
    return {after != before, after};
}

void EditorSession::primeDeferred() {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->primeDeferred();
}
std::size_t EditorSession::flushDueAutosaveDrafts() {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->flushDueAutosaveDrafts();
}
std::size_t EditorSession::flushAllAutosaveDrafts() {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->flushAllAutosaveDrafts();
}

EditorSession::DeferredWorkCounts EditorSession::deferredWorkCounts() const {
    std::lock_guard operationLock{impl_->operationMutex};
    return {impl_->syntaxRunCount, impl_->treeScanCount};
}

std::uint64_t EditorSession::liveDocumentRuntimeStateCountForTests() {
    return DocumentRuntimeState::liveInstances();
}

void EditorSession::setAutosaveDraftByteCapForTests(std::uint64_t cap) {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->autosaveDraftByteCap = cap;
}

void EditorSession::reconcileExternalWatchEventsForTest(
    std::vector<WatchEvent> events) {
    std::lock_guard operationLock{impl_->operationMutex};
    // Mirror the runtime drain: an Overflow in the batch triggers the full
    // open-document-vs-disk resync (the worker would signal it out of band), the
    // ordinary events reconcile normally.
    bool overflowed = false;
    std::vector<WatchEvent> ordinary;
    for (auto& event : events) {
        if (event.kind == WatchEventKind::Overflow) {
            overflowed = true;
        } else {
            ordinary.push_back(std::move(event));
        }
    }
    if (!ordinary.empty()) {
        impl_->reconcileExternalWatchEvents(std::move(ordinary));
    }
    if (overflowed) {
        impl_->reconcileAllOpenDocumentsAgainstDisk();
    }
}

bool EditorSession::diffModelHasFileForTest(const DiffFileId& id) const {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->diff.file(id).has_value();
}

void EditorSession::reportWatcherAvailabilityForTest(bool available) {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->watcherAvailable.store(available, std::memory_order_relaxed);
    impl_->drainWatcherAvailability();
}

void EditorSession::refreshFilesystemForTest() {
    std::lock_guard operationLock{impl_->operationMutex};
    const auto before = impl_->session->revision();
    impl_->refreshTreeForPublication(before);
}

bool EditorSession::dispatchInProgress() const noexcept {
    return impl_->session->activeDispatchRevision().has_value();
}

bool EditorSession::Impl::defer(std::optional<ClientId> as,
                                ClientCommand command) {
    if (!session->activeDispatchRevision()) return false;
    return deferredCommands.enqueue({as, std::move(command)});
}

bool EditorSession::deferDispatch(ClientId clientId, ClientCommand command) {
    return impl_->defer(clientId, std::move(command));
}

namespace {

CommandResult dispatchLocked(EditorSession::Impl* impl_, ClientId clientId,
                             ClientCommand const& command);

ClientInputResult inputKeyLocked(EditorSession::Impl* impl_, ClientId clientId,
                                 ClientKeyInput const& input) {
    if (!impl_->session->attachedClient(clientId)) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::UnknownClient,
                              impl_->session->revision(),
                              "client ID is not attached", {}}};
    }

    auto dispatchInput = [&](CommandName command,
                             std::any payload = {}) -> ClientInputResult {
        auto result = dispatchLocked(
            impl_, clientId,
            {std::move(command), impl_->session->revision(),
             std::move(payload)});
        const auto activation = result.accepted()
                                    ? impl_->interaction.openPickerActivation()
                                    : std::nullopt;
        return {ClientInputOutcome::Dispatched, std::nullopt,
                std::move(result), activation};
    };
    auto clientOwned = [](ClientOwnedInputKind kind,
                          std::string text = {}) -> ClientInputResult {
        return {ClientInputOutcome::ClientOwned,
                ClientOwnedInput{kind, std::move(text)}, std::nullopt};
    };

    PromptRoutingState routing;
    routing.focus = impl_->interaction.effectiveFocus();
    auto const promptStatus = impl_->promptStatusView();
    if (promptStatus.activeKind == PromptKind::Palette) {
        routing.prompt = ActivePrompt::Palette;
    } else if (auto const view = impl_->promptView()) {
        switch (view->kind) {
        case PromptKind::Find:
            routing.prompt = ActivePrompt::Find;
            break;
        case PromptKind::Replace:
            routing.prompt = ActivePrompt::Replace;
            break;
        case PromptKind::Path:
        case PromptKind::Settings:
        case PromptKind::CommandArgument:
            routing.prompt = ActivePrompt::TextPrompt;
            break;
        case PromptKind::Palette:
            break;
        }
        routing.activeInput = view->activeInput;
        std::size_t inputIndex = 0;
        for (auto const& control : view->controls) {
            if (control.kind != PromptControlKind::Input) continue;
            if (inputIndex++ == view->activeInput) {
                routing.currentValue = control.value;
                break;
            }
        }
    }

    auto routeTextEdit = [&](PromptTextEdit edit) -> ClientInputResult {
        auto const route = PromptTextRouter{}.edit(routing, edit);
        if (route.kind == PromptTextRoute::Kind::Dispatch) {
            return dispatchInput(route.command, route.payload);
        }
        if (routing.prompt == ActivePrompt::Palette) {
            switch (edit.kind) {
            case PromptTextEdit::Kind::Append:
                return clientOwned(ClientOwnedInputKind::AppendText,
                                   route.appendText);
            case PromptTextEdit::Kind::DeleteGraphemeBack:
                return clientOwned(
                    ClientOwnedInputKind::DeleteGraphemeBackward);
            case PromptTextEdit::Kind::DeleteWordBack:
                return clientOwned(ClientOwnedInputKind::DeleteWordBackward);
            }
        }
        return {ClientInputOutcome::Unhandled, std::nullopt, std::nullopt};
    };

    auto const catalogRevision = impl_->catalog->revision();
    if (!impl_->inputKeymap ||
        impl_->inputKeymapGeneration != impl_->keymapGeneration ||
        impl_->inputCatalogRevision != catalogRevision) {
        impl_->inputKeymap =
            std::make_unique<CompiledKeymap>(impl_->keymap, *impl_->catalog);
        impl_->inputKeymapGeneration = impl_->keymapGeneration;
        impl_->inputCatalogRevision = catalogRevision;
    }

    if (input.stroke.code != KeyCode::None) {
        auto const resolved = impl_->inputKeymap->resolve(
            std::array{CompiledKeymap::compile(input.stroke)}, routing.focus);
        if (resolved.kind == KeymapMatchKind::Resolved) {
            auto const& command = resolved.command;
            if (routing.prompt == ActivePrompt::Palette) {
                if (command == "prompt.submit") {
                    return clientOwned(ClientOwnedInputKind::Submit);
                }
                if (command == "prompt.next" || command == "palette.next") {
                    return clientOwned(ClientOwnedInputKind::SelectNext);
                }
                if (command == "prompt.previous" ||
                    command == "palette.previous") {
                    return clientOwned(ClientOwnedInputKind::SelectPrevious);
                }
                if (command == "prompt.cancel") {
                    return dispatchInput("palette.close");
                }
            }
            if (routing.focus == FocusTarget::Prompt &&
                command == "clipboard.paste") {
                auto const text = impl_->clipboard.viewState().plainText;
                if (text.empty()) {
                    return {ClientInputOutcome::Unhandled, std::nullopt,
                            std::nullopt};
                }
                return routeTextEdit(
                    {PromptTextEdit::Kind::Append, std::move(text)});
            }
            return dispatchInput(command);
        }
        if (input.stroke.code == KeyCode::Backspace) {
            return routeTextEdit(
                {input.stroke.alt ? PromptTextEdit::Kind::DeleteWordBack
                                  : PromptTextEdit::Kind::DeleteGraphemeBack,
                 {}});
        }
    }
    if (!input.committedText.empty()) {
        return routeTextEdit(
            {PromptTextEdit::Kind::Append, input.committedText});
    }
    return {ClientInputOutcome::Unhandled, std::nullopt, std::nullopt};
}

ClientInputResult inputLocked(EditorSession::Impl* impl_, ClientId clientId,
                              ClientInput const& input) {
    return std::visit(
        [&](auto const& semantic) -> ClientInputResult {
            using Input = std::decay_t<decltype(semantic)>;
            if constexpr (std::same_as<Input, ClientKeyInput>) {
                return inputKeyLocked(impl_, clientId, semantic);
            } else {
                if (!impl_->session->attachedClient(clientId)) {
                    return {ClientInputOutcome::Rejected, std::nullopt,
                            CommandResult{CommandError::UnknownClient,
                                          impl_->session->revision(),
                                          "client ID is not attached", {}}};
                }
                const auto unhandled = [] {
                    return ClientInputResult{ClientInputOutcome::Unhandled,
                                             std::nullopt, std::nullopt};
                };
                const auto rejectTarget = [&](std::string message) {
                    return ClientInputResult{
                        ClientInputOutcome::Rejected, std::nullopt,
                        CommandResult{CommandError::HandlerFailed,
                                      impl_->session->revision(),
                                      std::move(message), {}}};
                };
                if constexpr (!std::same_as<Input, DocumentPointerInput> &&
                              !std::same_as<Input, ScrollLinesInput> &&
                              !std::same_as<Input, ScrollFractionInput>) {
                    if (semantic.phase != InputPointerPhase::Press) {
                        return unhandled();
                    }
                }
                if constexpr (!std::same_as<Input, ScrollLinesInput> &&
                              !std::same_as<Input, ScrollFractionInput>) {
                    if (semantic.button != InputPointerButton::Primary &&
                        !std::same_as<Input, TabPointerInput>) {
                        return unhandled();
                    }
                }
                auto dispatch = [&](CommandName command,
                                    std::any payload) -> ClientInputResult {
                    auto result = dispatchLocked(
                        impl_, clientId,
                        {std::move(command), impl_->session->revision(),
                         std::move(payload)});
                    const auto activation =
                        result.accepted()
                            ? impl_->interaction.openPickerActivation()
                            : std::nullopt;
                    return {ClientInputOutcome::Dispatched, std::nullopt,
                            std::move(result), activation};
                };
                if constexpr (std::same_as<Input, DocumentPointerInput>) {
                    if (semantic.phase == InputPointerPhase::Press ||
                        semantic.phase == InputPointerPhase::Cancel) {
                        impl_->documentPointerGestures.erase(clientId);
                    }
                }
                if constexpr (!std::same_as<Input, PickerPointerInput>) {
                    if (semantic.basis.observedRevision !=
                        impl_->session->revision()) {
                        return {
                            ClientInputOutcome::Rejected, std::nullopt,
                            CommandResult{CommandError::StaleRevision,
                                          impl_->session->revision(),
                                          "semantic input basis is stale", {}}};
                    }
                }
                if constexpr (std::same_as<Input, ScrollLinesInput>) {
                    if (semantic.rows == 0) {
                        return rejectTarget(
                            "line-scroll input must move at least one row");
                    }
                    switch (semantic.target) {
                        case SemanticScrollTarget::Document:
                            return dispatch(
                                "view.scroll_lines",
                                ScrollLinesArguments{semantic.rows});
                        case SemanticScrollTarget::Tree:
                            return dispatch(
                                "tree.scroll",
                                ScrollLinesArguments{semantic.rows});
                    }
                    return rejectTarget("line-scroll target is invalid");
                } else if constexpr (std::same_as<Input,
                                                  ScrollFractionInput>) {
                    if (semantic.denominator == 0 ||
                        semantic.numerator > semantic.denominator) {
                        return rejectTarget(
                            "fraction-scroll input is invalid");
                    }
                    const auto fraction = ScrollFractionArguments{
                        semantic.numerator, semantic.denominator};
                    switch (semantic.target) {
                        case SemanticScrollTarget::Document:
                            return dispatch("view.scroll_to_fraction",
                                            fraction);
                        case SemanticScrollTarget::Tree:
                            return dispatch("tree.scroll_to_fraction",
                                            fraction);
                    }
                    return rejectTarget("fraction-scroll target is invalid");
                } else if constexpr (std::same_as<Input,
                                                  DocumentPointerInput>) {
                    const auto handled = [&] {
                        return ClientInputResult{
                            ClientInputOutcome::Dispatched, std::nullopt,
                            CommandResult{CommandError::None,
                                          impl_->session->revision(), {}, {}}};
                    };
                    if (semantic.phase == InputPointerPhase::Cancel) {
                        impl_->documentPointerGestures.erase(clientId);
                        return handled();
                    }
                    const auto resolvePosition = [&]()
                        -> std::optional<DocumentPosition> {
                        if (!semantic.position) return std::nullopt;
                        return SelectionNavigator::resolvePosition(
                            impl_->activeText(), *semantic.position);
                    };
                    if (semantic.phase == InputPointerPhase::Press) {
                        auto position = resolvePosition();
                        auto documentId = impl_->activeDocumentId();
                        if (!position || !documentId) {
                            return rejectTarget(
                                "document pointer target is not actionable");
                        }
                        if (semantic.selectWord) {
                            return dispatch(
                                "select.word_at_position",
                                SelectionCommandArguments{*position,
                                                          std::nullopt});
                        }
                        auto const& items = impl_->selection.selections.items();
                        std::vector<Selection> baseline{
                            items.begin(), items.end()};
                        if (semantic.additive && baseline.size() > 1) {
                            auto const hit = std::find_if(
                                baseline.begin(), baseline.end(),
                                [&](Selection const& selection) {
                                    auto const offset =
                                        position->byteOffset.value();
                                    auto const lower =
                                        selection.lower().byteOffset.value();
                                    auto const upper =
                                        selection.upper().byteOffset.value();
                                    return lower == upper ? offset == lower
                                                          : lower <= offset &&
                                                                offset < upper;
                                });
                            if (hit != baseline.end()) {
                                baseline.erase(hit);
                                return dispatch(
                                    "select.set_ranges",
                                    SelectionCommandArguments{
                                        std::nullopt, std::nullopt,
                                        std::move(baseline)});
                            }
                        }
                        impl_->documentPointerGestures.insert_or_assign(
                            clientId,
                            EditorSession::Impl::DocumentPointerGesture{
                                *documentId,
                                impl_->activeDocument()->revision(),
                                *position, *position, semantic.additive,
                                baseline});
                        auto result =
                            semantic.additive
                                ? dispatch(
                                      "select.add_range",
                                      SelectionCommandArguments{
                                          std::nullopt,
                                          Selection{*position, *position}})
                                : dispatch(
                                      "cursor.set_position",
                                      SelectionCommandArguments{*position,
                                                                std::nullopt});
                        if (!result.command || !result.command->accepted()) {
                            impl_->documentPointerGestures.erase(clientId);
                        }
                        return result;
                    }
                    auto gesture =
                        impl_->documentPointerGestures.find(clientId);
                    if (gesture == impl_->documentPointerGestures.end()) {
                        return unhandled();
                    }
                    if (impl_->activeDocumentId() !=
                        std::optional<FileDocumentId>{
                            gesture->second.documentId}) {
                        impl_->documentPointerGestures.erase(gesture);
                        return rejectTarget(
                            "document pointer gesture target changed");
                    }
                    if (impl_->activeDocument()->revision() !=
                        gesture->second.documentRevision) {
                        impl_->documentPointerGestures.erase(gesture);
                        return rejectTarget(
                            "document changed during pointer gesture");
                    }
                    auto position = resolvePosition();
                    if (semantic.edge != DocumentPointerEdge::None) {
                        const auto client =
                            impl_->session->attachedClient(clientId);
                        if (!client) {
                            return rejectTarget(
                                "document edge gesture client is detached");
                        }
                        auto& presentation =
                            impl_->presentation(client->viewId);
                        SelectionViewState edgeState{
                            SelectionSet{{Selection{
                                gesture->second.anchor,
                                gesture->second.active}}},
                            presentation.requestedFirstVisualRow,
                            presentation.requestedFirstVisualColumn,
                            presentation.desiredCell};
                        const ViewportDimensions viewport{
                            std::max<std::uint32_t>(
                                presentation.paneContentColumns, 1),
                            std::max<std::uint32_t>(
                                presentation.paneContentRows, 1)};
                        const auto diff = impl_->activeDiffFile();
                        auto advanced = SelectionNavigator{}.apply(
                            impl_->activeText(), edgeState,
                            semantic.edge == DocumentPointerEdge::Before
                                ? SelectionCommand::SelectLineUp
                                : SelectionCommand::SelectLineDown,
                            viewport, {}, {}, 4, impl_->wordWrap,
                            diff ? &*diff : nullptr);
                        if (!advanced.accepted()) {
                            return rejectTarget(advanced.message);
                        }
                        if (advanced.delta.replacement) {
                            position = advanced.delta.replacement->selections
                                           .primary()
                                           .active;
                        } else {
                            position = gesture->second.active;
                        }
                    }
                    if (!position &&
                        semantic.phase == InputPointerPhase::Move) {
                        return rejectTarget(
                            "document pointer target is not actionable");
                    }
                    ClientInputResult result = handled();
                    if (position) {
                        if (gesture->second.additive) {
                            auto ranges = gesture->second.baseline;
                            ranges.push_back(Selection{
                                gesture->second.anchor, *position});
                            result = dispatch(
                                "select.set_ranges",
                                SelectionCommandArguments{
                                    std::nullopt, std::nullopt,
                                    std::move(ranges)});
                        } else {
                            result = dispatch(
                                "select.set_range",
                                SelectionCommandArguments{
                                    std::nullopt,
                                    Selection{gesture->second.anchor,
                                              *position}});
                        }
                        if (result.command && result.command->accepted() &&
                            position) {
                            gesture->second.active = *position;
                        }
                    }
                    if (semantic.phase == InputPointerPhase::Release) {
                        impl_->documentPointerGestures.erase(clientId);
                    }
                    return result;
                } else if constexpr (std::same_as<Input, TabPointerInput>) {
                    if (semantic.button == InputPointerButton::Primary) {
                        return dispatch("tab.activate", semantic.tabId);
                    }
                    if (semantic.button == InputPointerButton::Auxiliary) {
                        return dispatch("tab.close", semantic.tabId);
                    }
                    return unhandled();
                } else if constexpr (std::same_as<Input, TreePointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch("tree.activate_node",
                                    TreeSelectArguments{semantic.nodeId});
                } else if constexpr (std::same_as<Input,
                                                  PickerPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch(
                        "picker.submit",
                        PickerSubmitArguments{semantic.activation,
                                              semantic.candidateId});
                } else if constexpr (std::same_as<
                                         Input, PromptControlPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch(
                        "prompt.focus_control",
                        PromptFocusArguments{semantic.controlId});
                } else if constexpr (std::same_as<
                                         Input, ExternalActionPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch("external.invoke_action",
                                    semantic.invocation);
                } else if constexpr (std::same_as<
                                         Input, StatusActionPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch("status.invoke_action",
                                    semantic.invocation);
                } else if constexpr (std::same_as<
                                         Input,
                                         PublishedUiActionPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    auto const sections = impl_->sections();
                    if (sections.uiState.generation !=
                            semantic.schemaGeneration ||
                        sections.uiPresence.generation !=
                            semantic.schemaGeneration) {
                        return rejectTarget("UI action schema is stale");
                    }
                    bool present = false;
                    for (auto const& node : sections.uiPresence.nodes) {
                        if (node.id == semantic.nodeId) {
                            present = node.present;
                            break;
                        }
                    }
                    if (!present) {
                        return rejectTarget("UI action target is not present");
                    }
                    for (auto const& node : sections.uiState.nodes) {
                        if (node.id == semantic.nodeId && node.leaf &&
                            node.leaf->command &&
                            !node.leaf->command->empty()) {
                            return dispatch(*node.leaf->command, std::any{});
                        }
                    }
                    return rejectTarget("UI action target is not actionable");
                } else if constexpr (std::same_as<
                                         Input, NoticeActionPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    const auto notice = impl_->noticeView();
                    if (!notice) {
                        return rejectTarget("notice action target is not present");
                    }
                    for (auto const& action : notice->actions) {
                        if (action.id == semantic.actionId) {
                            return dispatch(action.command, std::any{});
                        }
                    }
                    return rejectTarget("notice action target is not actionable");
                }
            }
        },
        input);
}

CommandResult dispatchLocked(EditorSession::Impl* impl_, ClientId clientId,
                             ClientCommand const& command) {
    // The routing signature: every runtime-owned input a host reads to interpret
    // the NEXT key. Compared before/after the whole dispatch (which drains nested
    // and deferred commands), so the effects union every route without annotating
    // any handler. Focus, prompt kind/value, and picker are subsumed by the
    // interaction routing generation; keymap, catalog, and clipboard each carry
    // their own authoritative counter.
    const auto routingSignature = [&] {
        return std::tuple{impl_->interaction.routingGeneration(),
                          impl_->keymapGeneration,
                          impl_->catalog->revision(),
                          impl_->clipboard.writeGeneration()};
    };
    const auto routingBefore = routingSignature();
    const auto revisionBefore = impl_->session->revision();
    const auto withEffects = [&](ExecutorResult outcome) {
        CommandResult result{outcome.error, outcome.revision,
                             std::move(outcome.message), {}};
        // routingChanged is precise; geometryChanged is the conservative gate a
        // pointer/wheel hit-test consumes. A routing change (prompt/focus/picker)
        // also reshapes presentation geometry, and a command that fails after a
        // partial mutation may move routing without advancing the session
        // revision -- so geometry is the union of "revision advanced" and "routing
        // changed", never a subset.
        const bool routingChanged = routingSignature() != routingBefore;
        result.effects.routingChanged = routingChanged;
        result.effects.geometryChanged =
            routingChanged || impl_->session->revision() != revisionBefore;
        return result;
    };
    // Parameterised by client because a deferred command runs as the client
    // that queued it, whose origin -- and so whether an edit counts as local --
    // may differ from the client whose dispatch is draining the queue.
    const auto dispatchAs = [&](ClientId as, const ClientCommand& dispatched) {
        const auto attached = impl_->session->attachedClient(as);
        const auto origin =
            attached ? attached->principal.origin() : InvocationOrigin::System;
        const auto shouldPauseForLocalEdit =
            origin != InvocationOrigin::Lua &&
            origin != InvocationOrigin::System;
        const auto revisionsBefore = documentRevisions(impl_->workspace);
        auto result = impl_->session->dispatch(as, dispatched);
        impl_->reconcileFindDocument();
        // The draft-conflict notice's presence lives in per-document runtime state,
        // outside the prompt/panel transitions, so reconcile it into the interaction
        // authority here where every state change (open, reopen, tab switch, discard,
        // dismiss) has settled -- the notice region then shows/hides in the presence
        // section this dispatch publishes.
        impl_->interaction.refreshNoticePresence(impl_->noticePresent());
        impl_->interaction.refreshExternalModificationPresence(
            impl_->externalModificationPresent());
        if (result.accepted() && shouldPauseForLocalEdit &&
            existingDocumentMutated(revisionsBefore, impl_->workspace)) {
            (void)impl_->follow.notifyLocalEdit();
        }
        return result;
    };
    // Dispatches a command AND runs whatever its handler asked to invoke,
    // before returning.  The drain lives here rather than at the end of this
    // function so that no path can reach a `return` with requests still
    // queued: the palette and prompt paths below return early, and a command
    // run through either of them may queue just as any other can.
    //
    // Requests run once the session lock has released, in the order asked for,
    // each rebased on the revision the previous one left.
    const auto dispatchAndDrain = [&](ClientId as,
                                      const ClientCommand& dispatched) {
        auto outcome = dispatchAs(as, dispatched);
        // A handler that FAILED does not get its requests performed: it may
        // have queued half a sequence before giving up, and running that half
        // is worse than running none of it.  Its success would also overwrite
        // the failure being reported.
        if (!outcome.accepted()) {
            impl_->deferredCommands.clear();
            return outcome;
        }
        while (!impl_->deferredCommands.empty()) {
            auto deferred = impl_->deferredCommands.takeFront();
            deferred.command.baseRevision = impl_->session->revision();
            auto const deferredResult = dispatchAs(
                deferred.client.value_or(as), deferred.command);
            // The first failure is reported, naming the command that failed,
            // and the rest are abandoned: continuing would run the remainder of
            // a sequence whose earlier step did not happen.
            if (!deferredResult.accepted()) {
                impl_->deferredCommands.clear();
                return ExecutorResult{
                    deferredResult.error, deferredResult.revision,
                    std::string{deferred.command.id.name()} + ": " +
                        deferredResult.message};
            }
            outcome = deferredResult;
        }
        return outcome;
    };
    const auto dispatchWithFollowEditPause =
        [&](const ClientCommand& dispatched) {
            return dispatchAndDrain(clientId, dispatched);
        };
    auto result = dispatchWithFollowEditPause(command);
    // The file picker's submit is file.open, which (unlike palette.execute) has
    // no prompt side effects of its own.  Closing it here rather than in the
    // client keeps close-on-success semantics identical for keyboard and
    // pointer submits: a rejected open -- the file was removed between the walk
    // and the submit -- leaves the picker open with its query intact.
    if (result.accepted() && impl_->interaction.openPicker() == PickerKind::File &&
        command.id == "file.open") {
        (void)impl_->interaction.cancelPrompt();
    }
    return withEffects(std::move(result));
}

}  // namespace

ClientInputResult EditorSession::input(ClientId clientId,
                                       ClientInput const& input) {
    if (const auto nested = impl_->session->activeDispatchRevision()) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed, *nested,
                              std::string{
                                  kNestedDispatchRefusal},
                              {}}};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return inputLocked(impl_.get(), clientId, input);
}

CommandResult EditorSession::dispatch(ClientId clientId,
                                      ClientCommand const& command) {
    // A handler must be refused before taking the non-recursive aggregate lock.
    if (const auto nested = impl_->session->activeDispatchRevision()) {
        return {CommandError::HandlerFailed, *nested,
                std::string{kNestedDispatchRefusal}};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return dispatchLocked(impl_.get(), clientId, command);
}

std::shared_ptr<CommandCatalog const> EditorSession::commandCatalog() const {
    return impl_->catalog;
}

CommandHandle EditorSession::registerCommand(CommandSpecBuilder command) {
    if (dispatchInProgress()) {
        throw std::logic_error{"commands cannot be registered during dispatch"};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    if (impl_->session->revision().value() ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"session revision exhausted"};
    }
    auto const handle = impl_->catalog->add(std::move(command));
    impl_->session->advanceRevision();
    return handle;
}

std::vector<CommandHandle> EditorSession::replaceCommandGeneration(
    std::span<CommandHandle const> retire,
    std::vector<CommandSpecBuilder> commands) {
    if (dispatchInProgress()) {
        throw std::logic_error{
            "command generations cannot be replaced during dispatch"};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    if (impl_->session->revision().value() ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"session revision exhausted"};
    }
    auto const catalogRevision = impl_->catalog->revision();
    auto handles = impl_->catalog->replaceGeneration(
        retire, std::move(commands));
    if (impl_->catalog->revision() != catalogRevision) {
        impl_->session->advanceRevision();
    }
    return handles;
}

Revision EditorSession::revision() const {
    if (const auto active = impl_->session->activeDispatchRevision()) {
        return *active;
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->session->revision();
}

std::uint64_t EditorSession::gitFullRefreshCountForTest() const {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->gitDiffWorker
               ? impl_->gitDiffWorker->fullRefreshCount.load(
                     std::memory_order_relaxed)
               : 0;
}

std::filesystem::path const& EditorSession::workspaceRoot() const noexcept { return impl_->root; }
ExternalDiffBurstResult EditorSession::applyExternalDiffBurst(
    std::vector<ExternalDiffRevision> changes) {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->applyExternalDiffBurst(std::move(changes));
}
GitDiffScanResult EditorSession::applyGitDiffScan(GitDiffScan scan) {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->applyGitDiffScan(std::move(scan));
}
std::optional<SessionSnapshot> EditorSession::present(
    ClientId clientId, ViewportDimensions dimensions,
    PaletteReport paletteReport) {
    if (impl_->session->activeDispatchRevision()) {
        throw std::logic_error{"a view cannot be presented during dispatch"};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    auto client = impl_->session->attachedClient(clientId);
    if (!client) return std::nullopt;
    auto& presentation = impl_->presentation(client->viewId);
    presentation.dimensions = dimensions;
    // Sections FIRST, then the viewport: computing the shell layout is what
    // caches the pane content height the viewport scrolls against.  As
    // arguments to one call their evaluation order would be unspecified, so the
    // viewport could be built against the PREVIOUS frame's pane height -- which
    // is wrong on the frame a resize or a prompt changes it.
    // The shell layout is computed FIRST: it caches the panel content height that
    // treeView() (inside sections) and viewport() resolve their scroll against.
    // Its geometry is the presentation's shell projection; its focus is semantic.
    auto shell = impl_->shellView(dimensions, paletteReport);
    if (!shell.panes.empty()) {
        auto const& content = shell.panes.front().content;
        presentation.paneContentRows =
            static_cast<std::uint32_t>(std::max(content.height, 1));
        presentation.paneContentColumns =
            static_cast<std::uint32_t>(std::max(content.width, 1));
    }
    presentation.reservedPromptRows =
        impl_->interaction.prompt().active() &&
                impl_->interaction.prompt().request()
            ? promptRowCount(impl_->interaction.prompt().request()->kind)
            : 0;
    presentation.panelContentRows =
        shell.panel ? static_cast<std::uint32_t>(
                          std::max(shell.panel->height - 1, 0))
                    : 0;
    auto sections = impl_->sections(paletteReport);
    auto viewport = impl_->viewport(presentation);
    auto promptView = impl_->promptProjection(dimensions, shell.prompt);
    ssg::SelectionNavigation selectionNav{
        presentation.requestedFirstVisualRow,
        presentation.requestedFirstVisualColumn, presentation.desiredCell};
    auto treeWindows = impl_->treeWindows(presentation);
    return SessionSnapshotCodec{}.assemble(impl_->session->revision(), impl_->session->topology(),
                                     client->principal, client->viewId,
                                     std::move(viewport), std::move(sections),
                                     impl_->style, std::move(promptView),
                                     std::move(shell), selectionNav,
                                     std::move(treeWindows));
}

std::optional<SessionSnapshot> EditorSession::snapshot(ClientId clientId,
                                                       PaletteReport paletteReport) const {
    std::lock_guard operationLock{impl_->operationMutex};
    auto client = impl_->session->attachedClient(clientId);
    if (!client) return std::nullopt;
    // Semantic-only: no ViewportDimensions, so no shell layout, viewport, prompt
    // projection, or selection scroll is computed, and the result carries no
    // PresentationSnapshot. A native-layout client that lays out the semantic
    // model itself uses this overload.
    auto sections = impl_->sections(paletteReport);
    return SessionSnapshot{impl_->session->revision(), impl_->session->topology(),
                           {client->principal.clientId(), client->viewId,
                            client->principal.capabilities()},
                           std::move(sections)};
}

int EditorSession::gitDiffWakeDescriptor() const {
    return impl_->gitDiffWakeDescriptor();
}

std::string EditorSession::activeDocumentText() const {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->activeText();
}

EditorSession::DraftReopenNotice EditorSession::activeDraftReopenNotice() const {
    std::lock_guard operationLock{impl_->operationMutex};
    const auto id = impl_->activeDocumentId();
    if (!id) return DraftReopenNotice::None;
    const auto found = impl_->documentRuntimeStates.find(id->value());
    if (found == impl_->documentRuntimeStates.end()) return DraftReopenNotice::None;
    switch (found->second.reopen) {
        case DraftReopenOutcome::None: return DraftReopenNotice::None;
        case DraftReopenOutcome::Restored: return DraftReopenNotice::Restored;
        case DraftReopenOutcome::Conflict: return DraftReopenNotice::Conflict;
    }
    return DraftReopenNotice::None;
}

} // namespace ssg
