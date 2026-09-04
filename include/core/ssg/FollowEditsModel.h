#pragma once

#include "ssg/CommandCatalog.h"
#include "ssg/DiffModel.h"
#include "ssg/PaneNavigation.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

enum class FollowMode : std::uint8_t {
    Following = 0,
    Paused = 1,
};

enum class NavigationClass : std::uint8_t {
    User,
    Programmatic,
    NonNavigation,
};

struct FollowTarget {
    DiffFileId id;
    std::filesystem::path path;
    bool deleted = false;
    std::size_t newestHunkLine = 0;
    std::uint64_t sourceRevision{0};
    friend bool operator==(const FollowTarget&, const FollowTarget&) = default;
};

struct FollowEditsViewState {
    std::uint64_t generation = 0;
    FollowMode mode = FollowMode::Following;
    PaneId activePane;
    std::optional<FollowTarget> activeTarget;
    std::vector<FollowTarget> queuedTargets;
    friend bool operator==(const FollowEditsViewState&,
                           const FollowEditsViewState&) = default;
};

struct FollowEditsFooterProjection {
    std::string mode;
    std::optional<std::string> resumeBinding;
    std::optional<std::string> resumeCommand;
    friend bool operator==(const FollowEditsFooterProjection&,
                           const FollowEditsFooterProjection&) = default;
};

struct FollowEditsConfig {
    std::size_t queueCapacity = 16;
    std::string resumeBinding = "follow_edits.resume";
};

enum class FollowEditsError : std::uint8_t {
    None,
    StaleRevision,
};

struct FollowEditsResult {
    FollowEditsError error = FollowEditsError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FollowEditsError::None;
    }
};

struct FollowNavigation {
    NavigationClass classification = NavigationClass::NonNavigation;
};

struct FollowDiffChange {
    DiffFileView file;
    std::vector<DiffHunk> priorHunks;
    std::uint64_t sourceRevision{0};
};

class FollowEditsModel {
public:
    explicit FollowEditsModel(FollowEditsConfig config = {});

    [[nodiscard]] FollowEditsResult acceptExternalChange(
        const DiffFileView& file, std::uint64_t sourceRevision);
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
                                          std::uint64_t sourceRevision) const;
    void activate(const FollowTarget& target);
    void advanceGeneration() noexcept;

    FollowEditsConfig config_;
    FollowEditsViewState state_;
    std::uint64_t latestSourceRevision_{0};
};

}  // namespace ssg
