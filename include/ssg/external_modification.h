#pragma once

#include "ssg/diff.h"
#include "ssg/recovery.h"
#include "ssg/watcher.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class ExternalAction : std::uint8_t {
    reload,
    keep_buffer,
    open_diff,
};

enum class ExternalDocumentStatus : std::uint8_t {
    externally_modified,
    externally_removed,
};

struct ExternalModificationCommandDescriptor {
    std::string_view id;
    ExternalAction action;
    friend bool operator==(const ExternalModificationCommandDescriptor&,
                           const ExternalModificationCommandDescriptor&) =
        default;
};

class ExternalModificationCommandSet {
public:
    [[nodiscard]]
    const std::array<ExternalModificationCommandDescriptor, 3>& descriptors()
        const noexcept {
        return descriptors_;
    }

private:
    const std::array<ExternalModificationCommandDescriptor, 3> descriptors_{{
        {"external.reload", ExternalAction::reload},
        {"external.keep_buffer", ExternalAction::keep_buffer},
        {"external.open_diff", ExternalAction::open_diff},
    }};
};

[[nodiscard]] ExternalModificationCommandSet
external_modification_command_set();

struct ExternalDocumentView {
    DiffFileId id;
    std::filesystem::path path;
    ExternalDocumentStatus status =
        ExternalDocumentStatus::externally_modified;
    std::string accessible_status;
    std::vector<ExternalAction> actions;

    friend bool operator==(const ExternalDocumentView&,
                           const ExternalDocumentView&) = default;
};

struct ExternalModificationViewState {
    Revision revision{0};
    std::vector<ExternalDocumentView> files;

    friend bool operator==(const ExternalModificationViewState&,
                           const ExternalModificationViewState&) = default;
};

struct ExternalModificationDelta {
    Revision base_revision{0};
    Revision revision{0};
    std::vector<ExternalDocumentView> upserted;
    std::vector<DiffFileId> removed;

    friend bool operator==(const ExternalModificationDelta&,
                           const ExternalModificationDelta&) = default;
};

enum class ExternalDeltaError : std::uint8_t {
    none,
    stale_revision,
    malformed_delta,
};

struct ExternalDeltaReplayResult {
    std::optional<ExternalModificationViewState> state;
    ExternalDeltaError error = ExternalDeltaError::none;
    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

[[nodiscard]] ExternalModificationDelta derive_external_modification_delta(
    const ExternalModificationViewState& base,
    const ExternalModificationViewState& target);
[[nodiscard]] ExternalDeltaReplayResult replay_external_modification_delta(
    const ExternalModificationViewState& base,
    const ExternalModificationDelta& delta);

struct ExternalEventInput {
    WatchEvent event;
    DiffFileId id;
    std::optional<std::string> disk_content;
};

enum class ExternalModificationError : std::uint8_t {
    none,
    stale_event,
    unsupported_event,
    invalid_event,
    document_missing,
    content_required,
    diff_rejected,
    no_external_change,
    recovery_failed,
};

struct ExternalModificationResult {
    ExternalModificationError error = ExternalModificationError::none;
    bool status_published = false;
    bool diff_routed = true;
    std::optional<RecoveryRecordId> compensation;
    [[nodiscard]] bool accepted() const noexcept {
        return error == ExternalModificationError::none;
    }
};

struct ExternalOpenDiffResult {
    ExternalModificationError error = ExternalModificationError::none;
    std::optional<DiffOpenTarget> target;
    [[nodiscard]] bool accepted() const noexcept {
        return error == ExternalModificationError::none && target.has_value();
    }
};

class ExternalModificationFlow {
public:
    ExternalModificationFlow(RecoveryActions& recovery, DiffModel& diff);
    ~ExternalModificationFlow();
    ExternalModificationFlow(ExternalModificationFlow&&) noexcept;
    ExternalModificationFlow& operator=(ExternalModificationFlow&&) noexcept;
    ExternalModificationFlow(const ExternalModificationFlow&) = delete;
    ExternalModificationFlow& operator=(const ExternalModificationFlow&) =
        delete;

    [[nodiscard]] ExternalModificationResult process_event(
        ExternalEventInput input,
        std::optional<JournalDocument>& document);
    [[nodiscard]] ExternalModificationResult reload(
        const DiffFileId& id, std::optional<JournalDocument>& document);
    [[nodiscard]] ExternalModificationResult keep_buffer(const DiffFileId& id);
    [[nodiscard]] ExternalOpenDiffResult open_diff(const DiffFileId& id) const;
    [[nodiscard]] ExternalModificationViewState view_state() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
