#include "runtime/editor_runtime_internal.h"

#include <ssg/CommandCatalog.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/GraphemeLayout.h>

#include <algorithm>
#include <array>
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
constexpr auto kGitDiffRetryDelay = std::chrono::milliseconds{1000};

enum class GitDiffMode { Poll, Event };

GitDiffMode gitDiffModeFromEnvironment() {
    if (const char* mode = std::getenv("SSG_GIT_DIFF_MODE");
        mode != nullptr && std::string_view{mode} == "event") {
        return GitDiffMode::Event;
    }
    return GitDiffMode::Poll;
}

bool setNonBlocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

GitTreeStatus gitTreeStatusForDiffStatus(DiffFileStatus status) {
    switch (status) {
        case DiffFileStatus::Added: return GitTreeStatus::Added;
        case DiffFileStatus::Modified: return GitTreeStatus::Modified;
        case DiffFileStatus::Deleted: return GitTreeStatus::Deleted;
        case DiffFileStatus::Renamed: return GitTreeStatus::Renamed;
    }
    throw std::logic_error("unknown diff file status");
}

std::vector<GitTreeRecord> gitTreeRecordsFromDiff(const DiffViewState& diffView) {
    std::vector<GitTreeRecord> records;
    records.reserve(diffView.files.size());
    for (const auto& file : diffView.files) {
        records.push_back(
            {.workspacePath = file.path.generic_string(),
             .label = file.path.generic_string(),
             .status = gitTreeStatusForDiffStatus(file.status),
             .commands = {}});
    }
    return records;
}

std::string liveDiffTabLabelForPath(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.empty() ? path.generic_string() : filename;
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

// The curated terminal runtime keymap (doc/spec-keymap.md K2): a small set of
// argument-free bindings the TUI drives, plus the context-divergent navigation
// keys.  Only argument-free-usable commands are bound (a bare chord dispatches
// with no payload); exhaustive reachability is the palette's job.  Global (*)
// chords are Escape-led and prefix-free; single strokes differ per focus.
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

    bind(seq({"Escape", "KeyS"}), "file.save", "*");
    bind(seq({"Escape", "KeyN"}), "file.new", "*");
    bind(seq({"Escape", "KeyZ"}), "edit.undo", "*");
    bind(seq({"Escape", "Shift+KeyZ"}), "edit.redo", "*");
    // leader+p opens the file picker (the frequent action) and leader+Shift+P
    // the command palette, matching the convention users arrive with.
    bind(seq({"Escape", "KeyP"}), "file_finder.open", "*");
    bind(seq({"Escape", "Shift+KeyP"}), "palette.open", "*");
    bind(seq({"Escape", "KeyB"}), "panel.toggle", "*");
    bind(seq({"Escape", "KeyO"}), "panel.focus", "*");
    bind(seq({"Escape", "BracketRight"}), "tab.next", "*");
    bind(seq({"Escape", "BracketLeft"}), "tab.previous", "*");
    bind(seq({"Escape", "KeyW"}), "tab.close", "*");
    bind(seq({"Escape", "KeyF", "KeyT"}), "settings.open", "*");
    bind(seq({"Escape", "KeyA"}), "select.all", "*");
    bind(seq({"Escape", "KeyD"}), "select.add_next_occurrence", "*");
    bind(seq({"Escape", "KeyI"}), "select.split_into_lines", "*");
    bind(seq({"Escape", "KeyK"}), "select.add_cursor_up", "*");
    bind(seq({"Escape", "KeyJ"}), "select.add_cursor_down", "*");
    bind(seq({"Escape", "Slash"}), "find.open", "*");
    bind(seq({"Escape", "KeyR"}), "replace.open", "*");

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
    bind(seq({"Ctrl+Shift+Home"}), "select.document_start", "editor");
    bind(seq({"Ctrl+Shift+End"}), "select.document_end", "editor");
    bind(seq({"PageUp"}), "cursor.page_up", "editor");
    bind(seq({"PageDown"}), "cursor.page_down", "editor");
    bind(seq({"Shift+PageUp"}), "select.page_up", "editor");
    bind(seq({"Shift+PageDown"}), "select.page_down", "editor");
    bind(seq({"Enter"}), "text.newline", "editor");
    bind(seq({"Backspace"}), "text.delete_backward", "editor");
    bind(seq({"Delete"}), "text.delete_forward", "editor");
    // Word-left/right: Ctrl+Left/Right is the common editor convention, but
    // Ctrl is not reliably interceptable in every host (browsers capture
    // several Ctrl+key combos at the chrome layer; see doc/spec-mod-keys.md).
    // The Escape leader works in both hosts, so word navigation rides it
    // instead, with the Shift variant extending the selection.
    bind(seq({"Escape", "ArrowLeft"}), "cursor.word_left", "editor");
    bind(seq({"Escape", "ArrowRight"}), "cursor.word_right", "editor");
    bind(seq({"Escape", "Shift+ArrowLeft"}), "select.word_left", "editor");
    bind(seq({"Escape", "Shift+ArrowRight"}), "select.word_right", "editor");
    // Alt+Left/Right is the conventional word-nav shortcut, but unlike
    // Alt+<letter> it does NOT arrive "for free" via the Escape/Alt byte
    // collision (see doc/spec-mod-keys.md): arrow keys use the CSI
    // modifier-parameter form, which decode_input already parses into a
    // single alt=true stroke, so this is an explicit, deliberate second
    // binding to the same commands as the Escape-led chords above.
    bind(seq({"Alt+ArrowLeft"}), "cursor.word_left", "editor");
    bind(seq({"Alt+ArrowRight"}), "cursor.word_right", "editor");
    bind(seq({"Alt+Shift+ArrowLeft"}), "select.word_left", "editor");
    bind(seq({"Alt+Shift+ArrowRight"}), "select.word_right", "editor");

    bind(seq({"ArrowDown"}), "tree.select_next", "panel");
    bind(seq({"ArrowUp"}), "tree.select_previous", "panel");
    bind(seq({"Enter"}), "tree.activate", "panel");

    bind(seq({"Enter"}), "prompt.submit", "prompt");
    bind(seq({"Escape", "Escape"}), "prompt.cancel", "prompt");
    bind(seq({"ArrowDown"}), "prompt.next", "prompt");
    bind(seq({"ArrowUp"}), "prompt.previous", "prompt");
    // Find/replace option toggles and replace-all, reachable while a find or
    // replace prompt is focused.  KeyC/KeyG/KeyE/KeyL are not in the `*` chord
    // set, so these Escape-prefixed chords stay prefix-free.  The handlers are
    // benign no-ops unless a find/replace prompt is active.
    bind(seq({"Escape", "KeyC"}), "find.toggle_case", "prompt");
    bind(seq({"Escape", "KeyG"}), "find.toggle_whole_word", "prompt");
    bind(seq({"Escape", "KeyE"}), "find.toggle_regex", "prompt");
    bind(seq({"Escape", "KeyL"}), "replace.all", "prompt");

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
          source{sourceModel},
          mode{gitDiffModeFromEnvironment()} {}

    std::unique_ptr<GitRepository> repository;
    DiffModel sourceModel;
    GitDiffSource source;
    std::unique_ptr<FilesystemWatcher> watcher;
    GitDiffMode mode = GitDiffMode::Poll;

    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool retryPending = false;
    std::chrono::steady_clock::time_point nextRetry =
        std::chrono::steady_clock::time_point::max();
    std::deque<GitDiffScan> pendingScans;
    std::thread thread;

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

EditorRuntime::Impl::Impl(std::filesystem::path canonicalCwd,
                          std::filesystem::path scratchRoot,
                          std::filesystem::path recoveryRoot,
                          std::filesystem::path archiveRoot,
                          bool deferEnrichment,
                          std::shared_ptr<SyntaxParser> parser,
                          std::vector<StatusFieldProviderBinding>
                              statusFieldProviderOverrides,
                          bool enableGitDiffWorker)
    : root{std::move(canonicalCwd)},
      scratchRoot{std::filesystem::weakly_canonical(scratchRoot)},
      recoveryRoot{std::filesystem::weakly_canonical(recoveryRoot)},
      archiveRoot{std::filesystem::weakly_canonical(archiveRoot)},
      recovery{RecoveryActions::create(recoveryRoot)},
      scratch{ScratchStore::create(scratchRoot, root)},
      workspace{Workspace::create(root, recovery, this->archiveRoot)},
      selection{initialSelection()},
      clipboard{4},
      shell{{"Files", "Git", "Symbols"}},
      tabs{*this},
      external{recovery, diff},
      syntaxParser{std::move(parser)},
      statusFieldCatalog{p0StatusFieldCatalog()},
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
    refreshTree();
    refreshSyntax();
    startGitDiffWorker(enableGitDiffWorker);
}

EditorRuntime::Impl::~Impl() { stopGitDiffWorker(); }

void EditorRuntime::Impl::startGitDiffWorker(bool enable) {
    if (!enable) {
        return;
    }
    auto state = std::make_unique<GitDiffRefreshWorkerState>(root);
    if (!state->repository || !state->repository->isUsable()) {
        return;
    }
    int wakePipe[2] = {-1, -1};
    if (::pipe(wakePipe) != 0 || !setNonBlocking(wakePipe[0]) ||
        !setNonBlocking(wakePipe[1])) {
        if (wakePipe[0] != -1) {
            (void)::close(wakePipe[0]);
        }
        if (wakePipe[1] != -1) {
            (void)::close(wakePipe[1]);
        }
        return;
    }
    state->wakeReadFd = wakePipe[0];
    state->wakeWriteFd = wakePipe[1];
    if (state->mode == GitDiffMode::Event) {
        try {
            state->watcher = makePlatformFilesystemWatcher(root);
        } catch (const std::runtime_error&) {
            state->mode = GitDiffMode::Poll;
        }
        if (!state->watcher) {
            state->mode = GitDiffMode::Poll;
        }
    }

    state->thread = std::thread([worker = state.get()]() {
        const auto shouldStop = [&]() {
            std::lock_guard lock(worker->mutex);
            return worker->stop;
        };
        const auto maybeRefreshAll = [&]() -> std::optional<GitDiffRefreshResult> {
            if (shouldStop()) {
                return std::nullopt;
            }
            try {
                auto refreshed = worker->source.refresh(*worker->repository);
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

        std::function<void(const GitDiffRefreshResult&, bool)> handleResult;
        handleResult = [&](const GitDiffRefreshResult& refreshed, bool fullRefresh) {
            if (refreshed.applied) {
                queueLatestScan();
                clearRetry();
            }
            if (!refreshed.accepted || refreshed.requestedRescan) {
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

        if (auto first = maybeRefreshAll()) {
            handleResult(*first, true);
        } else {
            return;
        }
        auto nextPoll = std::chrono::steady_clock::now() + kGitDiffPollInterval;
        while (!shouldStop()) {
            if (worker->mode == GitDiffMode::Poll) {
                auto wakeAt = nextPoll;
                {
                    std::lock_guard lock(worker->mutex);
                    if (worker->retryPending && worker->nextRetry < wakeAt) {
                        wakeAt = worker->nextRetry;
                    }
                }
                std::unique_lock lock(worker->mutex);
                if (worker->wake.wait_until(lock, wakeAt,
                                            [&]() { return worker->stop; })) {
                    break;
                }
                lock.unlock();
            } else {
                auto timeout = kGitDiffPollInterval;
                {
                    std::lock_guard lock(worker->mutex);
                    if (worker->retryPending) {
                        const auto now = std::chrono::steady_clock::now();
                        const auto remaining =
                            worker->nextRetry > now ? worker->nextRetry - now
                                                    : std::chrono::milliseconds{0};
                        timeout = std::min(
                            timeout,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                remaining));
                    }
                }
                std::vector<WatchEvent> events;
                try {
                    events = worker->watcher->poll(timeout);
                } catch (const std::runtime_error&) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        break;
                    }
                    handleResult(*full, true);
                    continue;
                }
                bool overflowed = false;
                std::vector<std::filesystem::path> paths;
                paths.reserve(events.size() * 2);
                for (const auto& event : events) {
                    if (event.kind == WatchEventKind::Overflow) {
                        overflowed = true;
                        break;
                    }
                    paths.push_back(event.path);
                    if (event.previousPath) {
                        paths.push_back(*event.previousPath);
                    }
                }
                if (overflowed) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        break;
                    }
                    handleResult(*full, true);
                } else if (!paths.empty()) {
                    std::sort(paths.begin(), paths.end());
                    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
                    auto pathRefresh = maybeRefreshPaths(paths);
                    if (!pathRefresh) {
                        break;
                    }
                    handleResult(*pathRefresh, false);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            bool retryDue = false;
            {
                std::lock_guard lock(worker->mutex);
                retryDue = worker->retryPending && now >= worker->nextRetry;
            }
            if (retryDue) {
                auto full = maybeRefreshAll();
                if (!full) {
                    break;
                }
                handleResult(*full, true);
            }
            if (worker->mode == GitDiffMode::Poll && now >= nextPoll) {
                auto full = maybeRefreshAll();
                if (!full) {
                    break;
                }
                handleResult(*full, true);
                nextPoll = now + kGitDiffPollInterval;
            }
        }
    });
    gitDiffWorker = std::move(state);
}

void EditorRuntime::Impl::stopGitDiffWorker() {
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

void EditorRuntime::Impl::drainGitDiffScans() {
    if (!gitDiffWorker) {
        return;
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
    {
        std::lock_guard lock(gitDiffWorker->mutex);
        scans.swap(gitDiffWorker->pendingScans);
    }
    for (auto& scan : scans) {
        (void)applyGitDiffScan(std::move(scan));
    }
}

int EditorRuntime::Impl::gitDiffWakeDescriptor() const {
    return gitDiffWorker ? gitDiffWorker->wakeReadFd : -1;
}

CommandHandlerResult EditorRuntime::Impl::runTransaction(
    std::function<CommandHandlerResult()> operation) {
    return operation();
}

std::any& EditorRuntime::Impl::featureStateValue(std::type_index) {
    throw std::logic_error{"EditorRuntime exposes feature state through snapshots"};
}

void EditorRuntime::Impl::publishStatusValue(std::type_index, std::any statusValue) {
    if (auto const* item = std::any_cast<StatusItem>(&statusValue)) {
        (void)status.enqueue(*item);
    }
}

void EditorRuntime::Impl::publishDeltaValue(std::type_index, std::any) {}

TabLifecycleResult EditorRuntime::Impl::close(
    const TabState& tab, std::chrono::milliseconds durabilityTimeout) {
    if (!tab.document) {
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
                                                  current->snapshot().text};
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
                                   current->snapshot().text};
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

TabLifecycleResult EditorRuntime::Impl::reopen(
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

WorkspaceSnapshot EditorRuntime::Impl::snapshot(Revision revision) const {
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

std::vector<SearchCommandDescriptor> EditorRuntime::Impl::descriptors() const {
    std::vector<SearchCommandDescriptor> result;
    for (auto const* command : session->catalog()->commands()) {
        result.push_back({command->id, command->id});
    }
    return result;
}

PaletteExecutionResult EditorRuntime::Impl::execute(std::string_view commandId) {
    return {session->catalog()->find(commandId) != nullptr, {}};
}

WorkspaceApplyResult EditorRuntime::Impl::apply(
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

WorkspaceApplyResult EditorRuntime::Impl::recover(const WorkspaceRecoveryRecord& record) {
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

bool EditorRuntime::Impl::store(const WorkspaceRecoveryRecord&) { return true; }

std::optional<LspDocumentSnapshot> EditorRuntime::Impl::snapshot(std::string_view uri) const {
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

LspWorkspaceDocumentWriteResult EditorRuntime::Impl::apply(
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

LspWorkspaceFileResult EditorRuntime::Impl::snapshot(std::string_view uri, LspWorkspaceFileNode& node) const {
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


LspWorkspaceFileResult EditorRuntime::Impl::createFile(std::string uri, bool overwrite) {
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

LspWorkspaceFileResult EditorRuntime::Impl::writeFile(std::string uri, std::string content) {
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

LspWorkspaceFileResult EditorRuntime::Impl::renamePath(std::string oldUri, std::string newUri, bool overwrite) {
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

LspWorkspaceFileResult EditorRuntime::Impl::deletePath(std::string uri, bool recursive) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    std::error_code code;
    if (recursive) std::filesystem::remove_all(*path, code);
    // seam-exempt: LSP delete has no archive contract; file.delete is the archived path
    else std::filesystem::remove(*path, code);
    return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
}

LspWorkspaceFileResult EditorRuntime::Impl::restorePath(std::string uri, const LspWorkspaceFileNode& node) {
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

std::optional<FileDocumentId> EditorRuntime::Impl::activeDocumentId() const {
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
    return std::nullopt;
}

const TabState* EditorRuntime::Impl::activeTabState() const {
    auto const& view = tabs.viewState();
    if (!view.active) return nullptr;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(),
                              [&](const TabState& tab) {
                                  return tab.id == *view.active;
                              });
    if (found == view.tabs.end()) return nullptr;
    return &*found;
}

CommandHandlerResult EditorRuntime::Impl::openOrFocusLiveDiffTab(
    const DiffFileView& file, NavigationClass classification,
    std::optional<ClientId> userClient) {
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
    if (userClient.has_value()) {
        recordNavigation(*userClient, classification);
    }
    shell.focusEditor();
    return success();
}

void EditorRuntime::Impl::refreshLiveDiffDocuments(const DiffViewState& diffView) {
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

bool EditorRuntime::Impl::openOrRevealFollowTargetProgrammatic(
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

Document const* EditorRuntime::Impl::activeDocument() const {
    auto id = activeDocumentId();
    return id ? workspace.tryDocument(*id) : nullptr;
}

Document* EditorRuntime::Impl::activeDocument() {
    auto id = activeDocumentId();
    return id ? const_cast<Document*>(workspace.tryDocument(*id)) : nullptr;
}

void EditorRuntime::Impl::ensureDocumentRuntimeState(FileDocumentId document) {
    documentRuntimeStates.try_emplace(
        document.value(),
        DocumentRuntimeState{HistoryConfig::defaults(), syntaxParser});
}

void EditorRuntime::Impl::discardDocumentRuntimeState(FileDocumentId document) {
    documentRuntimeStates.erase(document.value());
    // A find that was scoped to this document no longer has a subject.
    if (findDocumentId == document) findDocumentId.reset();
    // Any live diff tab mapped to it is equally orphaned.
    for (auto it = liveDiffDocuments.begin(); it != liveDiffDocuments.end();) {
        it = it->second == document ? liveDiffDocuments.erase(it)
                                    : std::next(it);
    }
}

DocumentHistory& EditorRuntime::Impl::historyFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{
            "document history was requested before document runtime state existed"};
    }
    return it->second.history;
}

SyntaxModel& EditorRuntime::Impl::syntaxFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{
            "document syntax was requested before document runtime state existed"};
    }
    return it->second.syntax;
}

SyntaxViewState EditorRuntime::Impl::activeSyntaxView() const {
    if (auto id = activeDocumentId()) {
        if (auto it = documentRuntimeStates.find(id->value());
            it != documentRuntimeStates.end()) {
            return it->second.syntax.viewState();
        }
    }
    const auto* document = activeDocument();
    const auto text = document ? document->snapshot().text : std::string{};
    const auto revision = document ? document->revision() : Revision{0};
    return plainTextSyntaxViewState(revision, LanguageId::plainText(), text, 4);
}

std::optional<WorkspaceDocumentState> EditorRuntime::Impl::activeWorkspaceState() const {
    auto id = activeDocumentId();
    return id ? workspace.state(*id) : std::nullopt;
}

std::optional<DiffFileView> EditorRuntime::Impl::activeDiffFile() const {
    const auto diffState = diff.viewState();
    const auto file = diffState.fileForDocument(documentView());
    return file ? std::optional<DiffFileView>{file->get()} : std::nullopt;
}

std::string EditorRuntime::Impl::activeText() const {
    auto const* document = activeDocument();
    return document ? document->snapshot().text : std::string{};
}

void EditorRuntime::Impl::resetSelectionForActiveDocument() {
    selection = initialSelection();
    requestedFirstVisualRow = 0;
}

void EditorRuntime::Impl::clampSelectionToActiveDocument() {
    auto text = activeText();
    auto offset = selection.selections.primary().active.byteOffset.value();
    if (offset > text.size()) offset = text.size();
    auto position = ssg::SelectionNavigator::resolvePosition(text, ByteOffset{offset}).value_or(zeroPosition());
    selection.selections = SelectionSet{std::vector<Selection>{Selection{position, position}}};
}

std::vector<CellRun> EditorRuntime::Impl::activeCellRuns() const {
    std::vector<CellRun> runs;
    std::string const text = activeText();
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? end : end - start);
        runs.push_back(GraphemeLayout{}.computeRun(line, 4));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (runs.empty()) runs.push_back(GraphemeLayout{}.computeRun("", 4));
    return runs;
}

ViewportViewState EditorRuntime::Impl::computeEditorViewport(
    ViewportDimensions dimensions, std::uint32_t firstRow,
    std::uint32_t firstColumn) const {
    const auto diffFile = activeDiffFile();
    if (wordWrap) {
        auto runs = activeCellRuns();
        return Viewport{}.compute(
            runs, dimensions, firstRow, diffFile ? &*diffFile : nullptr);
    }
    // Word wrap off (default): one logical line is one visual row; only the
    // visible lines are segmented, so this is O(visible rows), not O(document).
    return Viewport{}.computeUnwrapped(activeText(), dimensions, firstRow,
                                       firstColumn, 4,
                                       diffFile ? &*diffFile : nullptr);
}

ViewportViewState EditorRuntime::Impl::viewport(ViewportDimensions dimensions) const {
    return computeEditorViewport(dimensions, requestedFirstVisualRow,
                                   requestedFirstVisualColumn);
}

void EditorRuntime::Impl::refreshTree() {
    if (deferringEnrichment) {
        pendingTreeRefresh = true;
        return;
    }
    ++treeScanCount;
    tree.replaceProvider(TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"filesystem"}, root, TreeRevision{nextTreeRevision++}));
}

void EditorRuntime::Impl::reconcilePromptFocus() {
    if (prompt.active() && shell.focus() != FocusTarget::Prompt) {
        shell.enterPromptFocus();
    } else if (!prompt.active() && shell.focus() == FocusTarget::Prompt) {
        shell.exitPromptFocus();
    }
}

// Opens a picker's prompt and records its kind in one step.  Neither half is
// meaningful alone: a Palette prompt with no kind publishes an empty candidate
// list, and a kind with no prompt is a leak.  Pairing them here is why
// reconcileOpenPicker() below only has to handle closing.
bool EditorRuntime::Impl::openPickerPrompt(PickerKind kind) {
    auto const* picker = pickerCatalog().find(kind);
    if (picker == nullptr) return false;
    auto opened = prompt.open(PromptRequest{
        PromptKind::Palette, std::string{picker->promptTitle},
        {{"query", "Command palette query", ""}}, {}, std::nullopt});
    if (!opened.accepted()) return false;
    openPicker = kind;
    if (kind == PickerKind::File) rebuildFileCandidates();
    return true;
}

// The index opens its OWN repository handle rather than sharing the git-diff
// worker's: that one is owned by its thread, and libgit2 handles are not safe to
// use from two threads.
void EditorRuntime::Impl::rebuildFileCandidates() {
    auto matcher = makePlatformGitIgnoreMatcher(root);
    WorkspaceFileIndexOptions options;
    options.respectGitignore =
        boolSetting(settings, SettingKey::FileFinderRespectGitignore, true);
    fileCandidates =
        std::move(WorkspaceFileIndex{}.build(root, *matcher, options).candidates);
}

// Re-derives `openPicker` from the prompt after every dispatch.  A picker can be
// closed by palette.close, by prompt.cancel, by a successful palette.execute, or
// by the find-document reconcile dismissing the prompt; deriving the field here
// covers all of them at once, so adding a fifth close path cannot leave the next
// open publishing the previous picker's candidates.  The converse (a Palette
// prompt without a kind) is not reconcilable here -- nothing in the prompt says
// WHICH picker it is -- and is instead made unrepresentable by openPickerPrompt()
// being the only opener.
void EditorRuntime::Impl::reconcileOpenPicker() {
    bool const inputLineActive = prompt.active() && prompt.request() &&
                               prompt.request()->kind == PromptKind::Palette;
    if (!inputLineActive) {
        openPicker.reset();
        // Discard the walk's results with the picker that owned them.
        fileCandidates.clear();
    }
}

void EditorRuntime::Impl::reconcileFindDocument() {
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
    if (auto const& request = prompt.request();
        request && (request->kind == PromptKind::Find ||
                    request->kind == PromptKind::Replace)) {
        (void)prompt.cancel();
    }
    findDocumentId.reset();
}

void EditorRuntime::Impl::refreshSyntax() {
    auto id = activeDocumentId();
    if (!id) return;
    auto& model = syntaxFor(*id);
    auto const* document = activeDocument();
    auto text = document ? document->snapshot().text : std::string{};
    auto revision = document ? document->revision() : Revision{0};
    auto language = LanguageId::plainText();
    if (auto state = activeWorkspaceState();
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
    auto request = model.request(revision, std::move(language), std::move(text));
    if (request.accepted()) {
        auto output = model.run(*request.request);
        (void)model.accept(request.request, output);
    }
}

void EditorRuntime::Impl::primeDeferred() {
    if (!deferringEnrichment) return;
    deferringEnrichment = false;
    // Run whichever scans were requested while deferring, now that the first
    // frame is drawn.  Order: tree then syntax (independent; both publish through
    // the normal snapshot channel on the next snapshot).
    bool ran = false;
    if (pendingTreeRefresh) {
        pendingTreeRefresh = false;
        refreshTree();
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

void EditorRuntime::Impl::enqueueStatus(StatusPriority priority, std::string text) {
    auto value = nextStatusId++;
    (void)status.enqueue(StatusItem{StatusId{value}, priority, std::move(text), {}});
}

ExternalDiffBurstResult EditorRuntime::Impl::applyExternalDiffBurst(
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

GitDiffScanResult EditorRuntime::Impl::applyGitDiffScan(GitDiffScan scan) {
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
    auto stagedDiff = diff;
    auto stagedFollow = follow;
    std::vector<FollowDiffChange> followChanges;
    followChanges.reserve(scan.files.size() + stagedDiff.viewState().files.size());
    bool mutated = false;

    Revision nextRevision = Revision{stagedDiff.viewState().revision.value() + 1};
    const auto nextMutationRevision = [&nextRevision]() {
        auto current = nextRevision;
        nextRevision = Revision{nextRevision.value() + 1};
        return current;
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
            return {GitDiffScanError::DiffRejected};
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
        const auto prior = stagedDiff.file(file.id);
        if (!prior) {
            continue;
        }
        auto removedFile = prior->get();
        auto priorHunks = removedFile.hunks;
        const auto revision = nextMutationRevision();
        const auto removed = stagedDiff.removeFile(file.id, revision);
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
        refreshLiveDiffDocuments(diff.viewState());
        const auto next = follow.viewState();
        if (next.mode == FollowMode::Following && next.activeTarget &&
            next.activeTarget != previousTarget) {
            (void)openOrRevealFollowTargetProgrammatic(*next.activeTarget);
        }
    }
    tree.replaceProvider(TreeProviderSnapshot::fromGit(
        TreeProviderId{"git"}, TreeRevision{nextTreeRevision++},
        gitTreeRecordsFromDiff(mutated ? diff.viewState() : stagedDiff.viewState())));
    lastGitScanRevision = scan.revision;
    if (session) {
        session->advanceRevision();
    }
    return {};
}

bool EditorRuntime::Impl::revealCurrentDiffTarget(
    const FollowTarget& target, NavigationClass classification) {
    const auto text = activeText();
    const auto offset = lineStartOffset(text, target.newestHunkLine);
    const auto position =
        SelectionNavigator::resolvePosition(text, ByteOffset{offset});
    if (!position) {
        return false;
    }
    selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*position, *position}}};
    if (const auto file = diff.file(target.id)) {
        const auto projection =
            Viewport{}.rowProjectionUnwrapped(text, file->get());
        requestedFirstVisualRow = projection.visualRowForBufferLine(
            static_cast<std::uint32_t>(std::min<std::size_t>(
                target.newestHunkLine,
                std::numeric_limits<std::uint32_t>::max())));
        selection.firstVisualRow = requestedFirstVisualRow;
    }
    revealPrimaryCaret();
    for (const auto& client : follow.viewState().clients) {
        recordNavigation(client.client, classification);
    }
    shell.focusEditor();
    return true;
}

bool EditorRuntime::Impl::revealDiffTarget(
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

void EditorRuntime::Impl::recordNavigation(
    ClientId client, NavigationClass classification) {
    (void)follow.applyNavigation(
        {.client = client,
         .classification = classification,
         .offset = FollowScrollOffset{requestedFirstVisualRow,
                                      requestedFirstVisualColumn}});
}

EditorRuntime::EditorRuntime(std::unique_ptr<Impl> implementation) noexcept
    : impl_{std::move(implementation)} {}
EditorRuntime::~EditorRuntime() = default;

void EditorRuntime::resetKeymapToDefault() {
    // defaultTerminalKeymap() is a fixed, already-construction-time-
    // validated value (see create() above), so no re-validation is needed
    // here -- resetting to it can never fail.
    impl_->keymap = defaultTerminalKeymap();
}

void EditorRuntime::focusEditor() { impl_->shell.focusEditor(); }

EditorRuntimeCreateResult EditorRuntime::create(EditorRuntimeConfig config) {
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
                                           config.enableGitDiffWorker);
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
        EditorSessionBuilder builder;
        builder.services(*impl);
        bindRuntimeEditing(builder, *impl);
        bindRuntimeFiles(builder, *impl);
        bindRuntimePresentation(builder, *impl);
        bindRuntimeNavigation(builder, *impl);
        bindRuntimeLanguageServices(builder, *impl);
        impl->session = builder.build();
        return {std::unique_ptr<EditorRuntime>{new EditorRuntime{std::move(impl)}}, {}};
    } catch (std::exception const& exception) {
        return {nullptr, exception.what()};
    }
}

AttachResult EditorRuntime::attach(InvocationPrincipal principal, ViewId viewId) {
    auto clientId = principal.clientId();
    auto result = impl_->session->attach(std::move(principal), viewId);
    if (result.accepted()) {
        (void)impl_->follow.attachClient(clientId, ViewportDimensions{80, 24});
    }
    return result;
}

bool EditorRuntime::detach(ClientId clientId) {
    (void)impl_->follow.detachClient(clientId);
    return impl_->session->detach(clientId);
}

void EditorRuntime::primeDeferred() { impl_->primeDeferred(); }

EditorRuntime::DeferredWorkCounts EditorRuntime::deferredWorkCounts() const {
    return {impl_->syntaxRunCount, impl_->treeScanCount};
}

std::uint64_t EditorRuntime::liveDocumentRuntimeStateCountForTests() {
    return DocumentRuntimeState::liveInstances();
}

bool EditorRuntime::dispatchInProgress() const noexcept {
    return impl_->session->activeDispatchRevision().has_value();
}

bool EditorRuntime::Impl::defer(std::optional<ClientId> as,
                                ClientCommand command) {
    if (!session->activeDispatchRevision()) return false;
    return deferredCommands.enqueue({as, std::move(command)});
}

bool EditorRuntime::deferDispatch(ClientId clientId, ClientCommand command) {
    return impl_->defer(clientId, std::move(command));
}

CommandResult EditorRuntime::dispatch(ClientId clientId, ClientCommand const& command) {
    // The session refuses this too, but it has to be caught HERE as well:
    // everything below touches the session first (the attachment lookup), and
    // would block on the lock the handler's own call is holding before the
    // session ever got the chance to refuse.  One message, defined on
    // EditorSession, so the two guards cannot drift apart.
    if (const auto nested = impl_->session->activeDispatchRevision()) {
        return {CommandError::HandlerFailed, *nested,
                std::string{EditorSession::kNestedDispatchRefusal}};
    }
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
        impl_->reconcilePromptFocus();
        impl_->reconcileOpenPicker();
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
                return CommandResult{deferredResult.error,
                                     deferredResult.revision,
                                     std::string{deferred.command.id.name()} +
                                         ": " + deferredResult.message};
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
    if (result.accepted() && impl_->openPicker == PickerKind::File &&
        command.id == "file.open") {
        (void)impl_->prompt.cancel();
        impl_->reconcilePromptFocus();
        impl_->reconcileOpenPicker();
    }
    return result;
}

std::shared_ptr<CommandCatalog> EditorRuntime::commandCatalog() const {
    return impl_->session->catalog();
}

Revision EditorRuntime::revision() const { return impl_->session->revision(); }
std::filesystem::path const& EditorRuntime::workspaceRoot() const noexcept { return impl_->root; }
ExternalDiffBurstResult EditorRuntime::applyExternalDiffBurst(
    std::vector<ExternalDiffRevision> changes) {
    return impl_->applyExternalDiffBurst(std::move(changes));
}
GitDiffScanResult EditorRuntime::applyGitDiffScan(GitDiffScan scan) {
    return impl_->applyGitDiffScan(std::move(scan));
}
std::optional<SessionSnapshot> EditorRuntime::snapshot(ClientId clientId, ViewportDimensions dimensions,
                                                       KeySequence leaderPending,
                                                       PaletteReport paletteReport) const {
    const_cast<EditorRuntime::Impl*>(impl_.get())->drainGitDiffScans();
    auto client = impl_->session->attachedClient(clientId);
    if (!client) return std::nullopt;
    return SessionSnapshotCodec{}.assemble(impl_->session->revision(), impl_->session->topology(),
                                     client->principal, client->viewId,
                                     impl_->viewport(dimensions),
                                     impl_->sections(dimensions, leaderPending, paletteReport));
}

int EditorRuntime::gitDiffWakeDescriptor() const {
    return impl_->gitDiffWakeDescriptor();
}

std::string EditorRuntime::activeDocumentText() const { return impl_->activeText(); }

} // namespace ssg
