#pragma once

#include <ssg/session.h>
#include <ssg/session_snapshot.h>
#include <ssg/viewport.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ssg {

struct EditorRuntimeConfig {
    std::filesystem::path cwd;
    std::filesystem::path scratch_root;
    std::filesystem::path recovery_root;
    // M10 fast startup: when true, deferrable enrichment (workspace tree scan,
    // syntax highlighting) is NOT run during construction or the initial
    // file.open; it runs when the client calls prime_deferred() after drawing its
    // first frame.  Default false preserves the eager, fully-populated behavior
    // every non-startup caller (tests, in-process embedders) already relies on.
    bool defer_enrichment = false;
};

class EditorRuntime;

struct EditorRuntimeCreateResult {
    std::unique_ptr<EditorRuntime> runtime;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept { return runtime != nullptr; }
};

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

    [[nodiscard]] Revision revision() const;
    [[nodiscard]] std::filesystem::path const& workspaceRoot() const noexcept;
    // M10 fast startup: run the enrichment work that was deferred when the
    // runtime was created with defer_enrichment=true (the workspace tree scan and
    // syntax highlighting), then publish it through the normal snapshot/delta
    // channel.  Idempotent and a no-op when nothing was deferred; the client
    // calls it once after drawing its first frame.
    void primeDeferred();
    // M10 startup instrumentation: how many times the O(document) syntax
    // highlight pass and the O(workspace) tree scan have actually run.  Exposed
    // so the startup oracle can assert deferred enrichment does not run before
    // prime_deferred() (doc/spec-fast-startup.md).
    struct DeferredWorkCounts {
        std::uint64_t syntax_runs = 0;
        std::uint64_t tree_scans = 0;
    };
    [[nodiscard]] DeferredWorkCounts deferredWorkCounts() const;
    [[nodiscard]] std::optional<SessionSnapshot> snapshot(
        ClientId clientId, ViewportDimensions dimensions,
        KeySequence leaderPending = {},
        PaletteReport paletteReport = {}) const;
    [[nodiscard]] std::string activeDocumentText() const;

    struct Impl;

private:
    explicit EditorRuntime(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
