#pragma once

#include "ssg/DiffModel.h"
#include "ssg/RecoveryActions.h"
#include "ssg/FilesystemWatcher.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class ExternalAction : std::uint8_t {
    Reload,
    KeepBuffer,
    OpenDiff,
};

enum class ExternalDocumentStatus : std::uint8_t {
    ExternallyModified,
    ExternallyRemoved,
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
        {"external.reload", ExternalAction::Reload},
        {"external.keep_buffer", ExternalAction::KeepBuffer},
        {"external.open_diff", ExternalAction::OpenDiff},
    }};
};

[[nodiscard]] ExternalModificationCommandSet
externalModificationCommandSet();

struct ExternalDocumentView {
    DiffFileId id;
    std::filesystem::path path;
    ExternalDocumentStatus status =
        ExternalDocumentStatus::ExternallyModified;
    std::string accessibleStatus;
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
    Revision baseRevision{0};
    Revision revision{0};
    std::vector<ExternalDocumentView> upserted;
    std::vector<DiffFileId> removed;

    friend bool operator==(const ExternalModificationDelta&,
                           const ExternalModificationDelta&) = default;
};

enum class ExternalDeltaError : std::uint8_t {
    None,
    StaleRevision,
    MalformedDelta,
};

struct ExternalDeltaReplayResult {
    std::optional<ExternalModificationViewState> state;
    ExternalDeltaError error = ExternalDeltaError::None;
    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

class ExternalModificationDeltaCodec {
public:
    [[nodiscard]] ExternalModificationDelta derive(
        const ExternalModificationViewState& base,
        const ExternalModificationViewState& target);
    [[nodiscard]] ExternalDeltaReplayResult replay(
        const ExternalModificationViewState& base,
        const ExternalModificationDelta& delta);
};

struct ExternalEventInput {
    WatchEvent event;
    DiffFileId id;
    std::optional<std::string> diskContent;
};

enum class ExternalModificationError : std::uint8_t {
    None,
    StaleEvent,
    UnsupportedEvent,
    InvalidEvent,
    DocumentMissing,
    ContentRequired,
    DiffRejected,
    NoExternalChange,
    RecoveryFailed,
};

struct ExternalModificationResult {
    ExternalModificationError error = ExternalModificationError::None;
    bool statusPublished = false;
    bool diffRouted = true;
    std::optional<RecoveryRecordId> compensation;
    [[nodiscard]] bool accepted() const noexcept {
        return error == ExternalModificationError::None;
    }
};

struct ExternalOpenDiffResult {
    ExternalModificationError error = ExternalModificationError::None;
    std::optional<DiffOpenTarget> target;
    [[nodiscard]] bool accepted() const noexcept {
        return error == ExternalModificationError::None && target.has_value();
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

    [[nodiscard]] ExternalModificationResult processEvent(
        ExternalEventInput input,
        std::optional<JournalDocument>& document);
    [[nodiscard]] ExternalModificationResult reload(
        const DiffFileId& id, std::optional<JournalDocument>& document);
    [[nodiscard]] ExternalModificationResult keepBuffer(const DiffFileId& id);
    [[nodiscard]] ExternalOpenDiffResult openDiff(const DiffFileId& id) const;
    [[nodiscard]] ExternalModificationViewState viewState() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
