#pragma once

#include <ssg/DiffModel.h>
#include <ssg/EditorSession.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/GitDiffSource.h>
#include <ssg/StatusFields.h>
#include <ssg/session_snapshot.h>
#include <ssg/SyntaxModel.h>
#include <ssg/Viewport.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

class CommandCatalog;

struct EditorRuntimeConfig {
    std::filesystem::path cwd;
    std::filesystem::path scratchRoot;
    std::filesystem::path recoveryRoot;
    // Where deleted files are kept. Defaults beside scratch and recovery under
    // the workspace's `.ssg/`.
    std::filesystem::path archiveRoot;
    // M10 fast startup: when true, deferrable enrichment (workspace tree scan,
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
    // Optional provider overrides keyed by status-field id. These replace the
    // default compiled providers for matching ids.
    std::vector<StatusFieldProviderBinding> statusFieldProviders;
    // When false, disables the internal git-diff refresh worker. Tests can use
    // this for deterministic control; default true keeps git-diff wiring library-owned.
    bool enableGitDiffWorker = true;
};

class EditorRuntime;

struct EditorRuntimeCreateResult {
    std::unique_ptr<EditorRuntime> runtime;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept { return runtime != nullptr; }
};

struct ExternalDiffRevision {
    NonGitDiffEvent event;
    Revision revision{0};
};

enum class ExternalDiffBurstError {
    None,
    EmptyBurst,
    DiffRejected,
    FollowRejected,
};

struct ExternalDiffBurstResult {
    ExternalDiffBurstError error = ExternalDiffBurstError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == ExternalDiffBurstError::None;
    }
};

enum class GitDiffScanError {
    None,
    DiffRejected,
    FollowRejected,
};

struct GitDiffScanResult {
    GitDiffScanError error = GitDiffScanError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == GitDiffScanError::None;
    }
};

// CONTRACT
// EditorRuntime: resetKeymapToDefault, focusEditor, setComposedChrome,
//   primeDeferred, and the autosave-flush methods are host-only orchestration
//   seams, called on the session thread. They deliberately bypass the command
//   registry and are not user-visible actions, so they are never registered or
//   exposed through the Lua API; that omission is intentional, not a gap.
class EditorRuntime {
public:
    [[nodiscard]] static EditorRuntimeCreateResult create(
        EditorRuntimeConfig config);

    ~EditorRuntime();
    EditorRuntime(EditorRuntime const&) = delete;
    EditorRuntime& operator=(EditorRuntime const&) = delete;
    EditorRuntime(EditorRuntime&&) = delete;
    EditorRuntime& operator=(EditorRuntime&&) = delete;

    [[nodiscard]] AttachResult attach(InvocationPrincipal principal,
                                      ViewId viewId);
    [[nodiscard]] bool detach(ClientId clientId);
    [[nodiscard]] CommandResult dispatch(ClientId clientId,
                                         ClientCommand const& command);

    // Asks for `command` to be dispatched once the dispatch in progress
    // finishes, and reports whether the request was taken.
    //
    // For a handler that needs to invoke another command.  A handler runs with
    // the session locked, so it cannot dispatch directly -- `dispatch` refuses
    // it rather than deadlocking.  Queued commands run in order, each rebased
    // on the revision the previous one left, and a failure among them becomes
    // the result of the dispatch that queued them.
    //
    // Returns false when called outside a dispatch (where the caller should
    // simply dispatch) or when the queue is full, which means a handler is
    // queueing without bound.
    [[nodiscard]] bool deferDispatch(ClientId clientId, ClientCommand command);

    // Whether a dispatch is in progress on this thread, and so whether
    // `deferDispatch` is the way to reach another command.
    [[nodiscard]] bool dispatchInProgress() const noexcept;

    // The commands this runtime offers.  Held, not copied: a command
    // registered later is visible through the same pointer.
    [[nodiscard]] std::shared_ptr<CommandCatalog> commandCatalog() const;

    [[nodiscard]] Revision revision() const;
    [[nodiscard]] std::filesystem::path const& workspaceRoot() const noexcept;
    [[nodiscard]] ExternalDiffBurstResult applyExternalDiffBurst(
        std::vector<ExternalDiffRevision> changes);
    [[nodiscard]] GitDiffScanResult applyGitDiffScan(GitDiffScan scan);
    // Resets the live keymap to defaultTerminalKeymap() -- the same
    // hand-reviewed keymap installed at EditorRuntime::create. Called by
    // the host (apps/ssg_main.cpp) immediately before every init.lua
    // evaluation (startup AND auto-reload), so keymap.bind/keymap.unbind
    // always start from a clean slate: init.lua's current content is the
    // WHOLE keymap customization, never additive across reloads. A dedicated
    // method rather
    // than ssg_main.cpp reaching into Impl fields directly.
    void resetKeymapToDefault();
    // Moves keyboard focus to the editor -- the same effect
    // ShellState::focusEditor() has internally (e.g. after tab.activate
    // succeeds), exposed as a dedicated method
    // for the host (apps/ssg_main.cpp) to call after opening a
    // command-line file argument at startup, so focus lands on the
    // editor rather than wherever panel.show_files left it. Not a
    // Lua/keymap/palette command -- an app/runtime seam only, like
    // resetKeymapToDefault() above.
    void focusEditor();
    // Installs the init.lua-composed header/footer, or nullopt to keep/restore
    // the built-in chrome. Called by
    // the host (apps/ssg_main.cpp) after every init.lua evaluation -- startup AND
    // auto-reload -- with the ScriptHost's currently published composition (which
    // already reflects rollback: a rejected reload keeps the prior value). Like
    // resetKeymapToDefault(), an app/runtime seam rather than a Lua command:
    // ssg.chrome stages a nested widget tree, not a flat command argument. A real
    // change advances the session revision so delta-based clients repaint; an
    // identical re-push is a no-op.
    void setComposedChrome(std::optional<ChromeComposition> composition);
    // M10 fast startup: run the enrichment work that was deferred when the
    // runtime was created with defer_enrichment=true (the workspace tree scan and
    // syntax highlighting), then publish it through the normal snapshot/delta
    // channel.  Idempotent and a no-op when nothing was deferred; the client
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
    // M10 startup instrumentation: how many times the O(document) syntax
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
    // CONTRACT
    // EditorRuntime::snapshot: the semantic model and interaction state are never
    //   gated on grid geometry. The dimension-taking overload adds an optional
    //   PresentationSnapshot (viewport, style, footer prompt, shell layout,
    //   selection scroll, tree scroll windows); the dimension-less overload
    //   returns the identical semantic sections with presentation() == nullopt. A
    //   client that lays out the model natively obtains full semantic state
    //   without supplying, or paying for, any grid projection.
    [[nodiscard]] std::optional<SessionSnapshot> snapshot(
        ClientId clientId, ViewportDimensions dimensions,
        PaletteReport paletteReport = {}) const;
    [[nodiscard]] std::optional<SessionSnapshot> snapshot(
        ClientId clientId, PaletteReport paletteReport = {}) const;
    [[nodiscard]] int gitDiffWakeDescriptor() const;
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
    explicit EditorRuntime(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
