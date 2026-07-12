#pragma once

#include <ssg/session.h>
#include <ssg/session_snapshot.h>
#include <ssg/viewport.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ssg {

struct EditorRuntimeConfig {
    std::filesystem::path cwd;
    std::filesystem::path scratch_root;
    std::filesystem::path recovery_root;
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
                                      ViewId view_id);
    [[nodiscard]] bool detach(ClientId client_id);
    [[nodiscard]] CommandResult dispatch(ClientId client_id,
                                         ClientCommand const& command);

    [[nodiscard]] Revision revision() const;
    [[nodiscard]] std::filesystem::path const& workspace_root() const noexcept;
    [[nodiscard]] std::optional<SessionSnapshot> snapshot(
        ClientId client_id, ViewportDimensions dimensions) const;
    [[nodiscard]] std::string active_document_text() const;

    struct Impl;

private:
    explicit EditorRuntime(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
