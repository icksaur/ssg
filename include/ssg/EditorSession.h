#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/DiffModel.h>
#include <ssg/ClientInput.h>
#include <ssg/CommandCatalog.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/GitDiffSource.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PaneTopology.h>
#include <ssg/PromptSurface.h>
#include <ssg/Selection.h>
#include <ssg/Style.h>
#include <ssg/StatusBar.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TabManager.h>
#include <ssg/Theme.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/Viewport.h>
#include <ssg/LspSyncClient.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ssg {

class CommandCatalog;
class GridPresenter;

struct NoticeView {
    std::string text;
    std::vector<UiAction> actions;
    friend bool operator==(const NoticeView&, const NoticeView&) = default;
};

struct EditorSessionConfig {
    std::filesystem::path cwd;
    std::filesystem::path scratchRoot;
    std::filesystem::path recoveryRoot;
    // Where deleted files are kept. Defaults beside scratch and recovery under
    // the workspace's `.ssg/`.
    std::filesystem::path archiveRoot;
    // When true, deferrable enrichment (workspace tree scan,
    // syntax highlighting) is NOT run during construction or the initial
    // file.open; it runs when the client calls prime_deferred() after drawing its
    // first frame.  Default false preserves the eager, fully-populated behavior
    // every non-startup caller (tests, in-process embedders) already relies on.
    bool deferEnrichment = false;
    // The syntax parser the runtime drives for highlighting. Injected here (not
    // hard-constructed inside the runtime) so an app supplies tree-sitter via
    // TreeSitterParserFactory::createDefault(), a future LSP semantic-tokens
    // source substitutes another implementation, and tests inject a
    // deterministic double. Null = plain-text highlighting.
    std::shared_ptr<SyntaxParser> syntaxParser;
    // When false, disables the internal git-diff refresh worker. Tests can use
    // this for deterministic control; default true keeps git-diff wiring library-owned.
    // This controls git diff computation only, never file watching: the filesystem
    // watcher is a workspace service that observes external modification whether or
    // not git diffs are being computed (Decision 1).
    bool enableGitDiffWorker = true;
    // When false, disables the filesystem watcher (and thus external-modification
    // ingress). Independent of enableGitDiffWorker so a non-git session still
    // watches files, and a test driving the reconcile hook can suppress the real
    // inotify watcher to stay deterministic. Default true keeps watching on.
    bool enableFilesystemWatcher = true;
};

class EditorSession;

struct EditorSessionCreateResult {
    std::unique_ptr<EditorSession> session;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept { return session != nullptr; }
};

struct ExternalDiffRevision {
    NonGitDiffEvent event;
    std::uint64_t revision{0};
};

enum class DiffIngressError {
    None,
    EmptyBurst,
    DiffRejected,
    FollowRejected,
};

struct DiffIngressResult {
    DiffIngressError error = DiffIngressError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == DiffIngressError::None;
    }
};

struct PumpResult {
    bool advanced;
};

// CONTRACT
// EditorSession: resetKeymapToDefault, focusEditor, primeDeferred, and the
//   autosave-flush methods are host-only orchestration
//   seams, called on the session thread. They deliberately bypass the command
//   registry and are not user-visible actions, so they are never registered or
//   exposed through the Lua API; that omission is intentional, not a gap.
class EditorSession {
public:
    [[nodiscard]] static EditorSessionCreateResult create(
        EditorSessionConfig config);

    ~EditorSession();
    EditorSession(EditorSession const&) = delete;
    EditorSession& operator=(EditorSession const&) = delete;
    EditorSession(EditorSession&&) = delete;
    EditorSession& operator=(EditorSession&&) = delete;

    // CONTRACT
    // EditorSession is the serialized aggregate boundary. Each public state
    // operation completes before another state operation can observe it; command
    // dispatch includes handler requests, reconciliation, and public result
    // construction. A handler must request a follow-up through
    // deferDispatch rather than re-entering a state operation.
    [[nodiscard]] PumpResult pump();
    [[nodiscard]] CommandResult dispatch(ClientCommand const& command);
    [[nodiscard]] ClientInputResult input(ClientInput const& input);

    // Asks for `command` to be dispatched once the dispatch in progress
    // finishes, and reports whether the request was taken.
    //
    // For a handler that needs to invoke another command.  A handler runs with
    // the session locked, so it cannot dispatch directly -- `dispatch` refuses
    // it rather than deadlocking. Queued commands run in order, and a failure
    // among them becomes the result of the dispatch that queued them.
    //
    // Returns false when called outside a dispatch (where the caller should
    // simply dispatch) or when the queue is full, which means a handler is
    // queueing without bound.
    [[nodiscard]] bool deferDispatch(ClientCommand command);

    // Whether a dispatch is in progress on this thread, and so whether
    // `deferDispatch` is the way to reach another command.
    [[nodiscard]] bool dispatchInProgress() const noexcept;

    // The commands this runtime offers.  Held, not copied: a command
    // registered later is visible through the same pointer.
    [[nodiscard]] std::shared_ptr<CommandCatalog const> commandCatalog() const;
    // CONTRACT
    // Command registration is host orchestration on the session thread. It must
    // not run concurrently with input, dispatch, or another registration, and a
    // command handler must defer orchestration rather than register reentrantly.
    [[nodiscard]] CommandHandle registerCommand(CommandSpec command);
    [[nodiscard]] std::vector<CommandHandle> replaceCommandGeneration(
        std::span<CommandHandle const> retire,
        std::vector<CommandSpec> commands);

    [[nodiscard]] std::filesystem::path const& workspaceRoot() const noexcept;
    [[nodiscard]] DiffIngressResult applyExternalDiffBurst(
        std::vector<ExternalDiffRevision> changes);
    [[nodiscard]] DiffIngressResult applyGitDiffScan(GitDiffScan scan);
    // Resets the live keymap to defaultTerminalKeymap() -- the same
    // hand-reviewed keymap installed at EditorSession::create. Called by
    // the host (src/application.cpp) immediately before every init.lua
    // evaluation (startup AND auto-reload), so keymap.bind/keymap.unbind
    // always start from a clean slate: init.lua's current content is the
    // WHOLE keymap customization, never additive across reloads. A dedicated
    // method rather
    // than application.cpp reaching into Impl fields directly.
    void resetKeymapToDefault();
    // Moves keyboard focus to the editor (e.g. after tab.activate succeeds),
    // exposed as a dedicated method
    // for the host (src/application.cpp) to call after opening a
    // command-line file argument at startup, so focus lands on the
    // editor rather than wherever panel.show_files left it. Not a
    // Lua/keymap/palette command -- an app/runtime seam only, like
    // resetKeymapToDefault() above.
    void focusEditor();
    // Run the enrichment work that was deferred when the
    // runtime was created with defer_enrichment=true (the workspace tree scan and
    // syntax highlighting). Idempotent and a no-op when nothing was deferred; the client
    // calls it once after drawing its first frame.
    void primeDeferred();
    // Autosave open dirty documents' drafts (single-file draft recovery, M15).
    // `flushDueAutosaveDrafts` applies the debounce policy (eager first flush,
    // then at most once per AutosaveDebounceMs) and is called on the app's
    // periodic tick; `flushAllAutosaveDrafts` forces every dirty draft for a
    // clean process exit. Both return the number of drafts written and are
    // non-blocking (durability is the background fsync thread's job). Call on the
    // app thread only, like the other runtime seams here.
    std::size_t flushDueAutosaveDrafts();
    std::size_t flushAllAutosaveDrafts();
    // How many times the O(document) syntax
    // highlight pass and the O(workspace) tree scan have actually run.  Exposed
    // so the startup oracle can assert deferred enrichment does not run before
    // prime_deferred().
    struct DeferredWorkCounts {
        std::uint64_t syntaxRuns = 0;
        std::uint64_t treeScans = 0;
    };
    [[nodiscard]] DeferredWorkCounts deferredWorkCounts() const;
    [[nodiscard]] static std::uint64_t liveDocumentRuntimeStateCountForTests();
    // Lower the per-draft autosave byte cap so a test can exercise the
    // oversized-draft path without materialising a multi-MiB buffer. Test-only;
    // production keeps the default cap.
    void setAutosaveDraftByteCapForTests(std::uint64_t cap);
    // Injects normalized watch events straight into the runtime-thread external-
    // modification reconcile, standing in for the watcher worker's queue+drain so a
    // test drives the real reconcile deterministically without depending on
    // inotify timing. Test-only; production ingress runs through the wake drain.
    void reconcileExternalWatchEventsForTest(std::vector<WatchEvent> events);
    // Whether the shared DiffModel currently holds an entry for `id`. Test-only, so
    // the external-modification oracle can assert a rejected event orphans no diff
    // entry keyed to a path no pending action owns.
    [[nodiscard]] bool diffModelHasFileForTest(const DiffFileId& id) const;
    // Drives a watcher-availability transition through the runtime-thread path a
    // real worker uses. Test-only;
    // production transitions arrive via the worker's wake drain.
    void reportWatcherAvailabilityForTest(bool available);
    // Drives the synchronous worker-side filesystem refresh without relying on
    // platform watcher timing.
    void refreshFilesystemForTest();
    [[nodiscard]] int gitDiffWakeDescriptor() const;
    [[nodiscard]] std::uint64_t gitFullRefreshCountForTest() const;
    [[nodiscard]] std::string activeDocumentText() const;

    // The reopen outcome of the active document's recovered draft (single-file
    // draft recovery, M15). `None` when the document has no recovered draft (the
    // common case — the feature is invisible without unsaved edits). `Restored`
    // when a draft was reloaded as a dirty buffer and the disk file is unchanged
    // since the edits branched (a subtle badge, no conflict). `Conflict` when the
    // draft was reloaded but the disk file changed externally, so the non-modal
    // conflict notice applies. Read by the notice/discard phases and by tests.
    enum class DraftReopenNotice { None, Restored, Conflict };
    [[nodiscard]] DraftReopenNotice activeDraftReopenNotice() const;

    struct Impl;

private:
    friend class GridPresenter;

    explicit EditorSession(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
