#pragma once

#include "ssg/CommandInvocation.h"
#include "ssg/DiffModel.h"
#include "ssg/ShellState.h"
#include "ssg/Viewport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class FollowMode : std::uint8_t { Following, Paused };

enum class NavigationClass : std::uint8_t {
    User,
    Programmatic,
    NonNavigation,
};

struct FollowScrollOffset {
    std::uint64_t firstRow = 0;
    std::uint64_t firstColumn = 0;
    friend bool operator==(const FollowScrollOffset&,
                           const FollowScrollOffset&) = default;
};

struct FollowTarget {
    DiffFileId id;
    std::filesystem::path path;
    bool deleted = false;
    std::size_t newestHunkLine = 0;
    Revision sourceRevision{0};
    friend bool operator==(const FollowTarget&, const FollowTarget&) = default;
};

struct FollowClientView {
    ClientId client;
    ViewportDimensions dimensions;
    FollowScrollOffset offset;
    friend bool operator==(const FollowClientView&,
                           const FollowClientView&) = default;
};

struct FollowEditsViewState {
    std::uint64_t generation = 0;
    FollowMode mode = FollowMode::Following;
    PaneId activePane;
    std::optional<FollowTarget> activeTarget;
    std::vector<FollowTarget> queuedTargets;
    std::vector<FollowClientView> clients;
    friend bool operator==(const FollowEditsViewState&,
                           const FollowEditsViewState&) = default;
};

struct FollowEditsDelta {
    std::uint64_t baseGeneration = 0;
    std::uint64_t generation = 0;
    std::optional<FollowEditsViewState> replacement;
    friend bool operator==(const FollowEditsDelta&,
                           const FollowEditsDelta&) = default;
};

class FollowEditsDeltaCodec {
public:
    [[nodiscard]] FollowEditsDelta derive(const FollowEditsViewState& base,
                                          const FollowEditsViewState& target);
};

struct FollowEditsFooterProjection {
    std::string mode;
    std::optional<std::string> resumeBinding;
    std::optional<std::string> resumeCommand;
    friend bool operator==(const FollowEditsFooterProjection&,
                           const FollowEditsFooterProjection&) = default;
};

struct FollowEditsCommandDescriptor {
    std::string_view id;
    friend bool operator==(const FollowEditsCommandDescriptor&,
                           const FollowEditsCommandDescriptor&) = default;
};

class FollowEditsCommandSet {
public:
    [[nodiscard]]
    const std::array<FollowEditsCommandDescriptor, 3>& descriptors()
        const noexcept {
        return descriptors_;
    }

private:
    const std::array<FollowEditsCommandDescriptor, 3> descriptors_{{
        {"follow_edits.resume"},
        {"follow_edits.pause"},
        {"follow_edits.toggle"},
    }};
};

[[nodiscard]] FollowEditsCommandSet followEditsCommandSet();

struct FollowEditsConfig {
    std::size_t queueCapacity = 16;
    std::string resumeBinding = "follow_edits.resume";
};

enum class FollowEditsError : std::uint8_t {
    None,
    StaleRevision,
    DuplicateClient,
    UnknownClient,
    InvalidViewport,
};

struct FollowEditsResult {
    FollowEditsError error = FollowEditsError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FollowEditsError::None;
    }
};

struct FollowNavigation {
    ClientId client;
    NavigationClass classification = NavigationClass::NonNavigation;
    std::optional<PaneId> pane;
    std::optional<FollowScrollOffset> offset;
};

struct FollowDiffChange {
    DiffFileView file;
    std::vector<DiffHunk> priorHunks;
    Revision sourceRevision{0};
};

class FollowEditsModel {
public:
    explicit FollowEditsModel(FollowEditsConfig config = {});

    [[nodiscard]] FollowEditsResult attachClient(
        ClientId client, ViewportDimensions dimensions);
    [[nodiscard]] FollowEditsResult detachClient(ClientId client);
    [[nodiscard]] FollowEditsResult acceptExternalChange(
        const DiffFileView& file, Revision sourceRevision);
    [[nodiscard]] FollowEditsResult acceptExternalChanges(
        std::vector<FollowDiffChange> changes);
    [[nodiscard]] FollowEditsResult applyNavigation(
        const FollowNavigation& navigation);
    [[nodiscard]] FollowEditsResult notifyLocalEdit();
    [[nodiscard]] FollowEditsResult pause();
    [[nodiscard]] FollowEditsResult resume(const DiffViewState& currentDiff);
    [[nodiscard]] FollowEditsResult toggle(const DiffViewState& currentDiff);

    [[nodiscard]] FollowEditsViewState viewState() const;
    [[nodiscard]] FollowEditsFooterProjection footerProjection() const;

private:
    [[nodiscard]] FollowTarget targetFor(const DiffFileView& file,
                                          const DiffHunk& hunk,
                                          Revision sourceRevision) const;
    void activate(const FollowTarget& target, const DiffFileView& file);
    void advanceGeneration() noexcept;

    FollowEditsConfig config_;
    FollowEditsViewState state_;
    std::optional<RowProjection> activeProjection_;
    Revision latestSourceRevision_{0};
};

}  // namespace ssg
