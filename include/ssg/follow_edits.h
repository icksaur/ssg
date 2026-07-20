#pragma once

#include "ssg/command_registry.h"
#include "ssg/diff.h"
#include "ssg/ui_layout.h"
#include "ssg/viewport.h"

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
    std::uint64_t first_row = 0;
    std::uint64_t first_column = 0;
    friend bool operator==(const FollowScrollOffset&,
                           const FollowScrollOffset&) = default;
};

struct FollowTarget {
    DiffFileId id;
    std::filesystem::path path;
    bool deleted = false;
    std::size_t newest_hunk_line = 0;
    Revision source_revision{0};
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
    PaneId active_pane;
    std::optional<FollowTarget> active_target;
    std::vector<FollowTarget> queued_targets;
    std::vector<FollowClientView> clients;
    friend bool operator==(const FollowEditsViewState&,
                           const FollowEditsViewState&) = default;
};

struct FollowEditsDelta {
    std::uint64_t base_generation = 0;
    std::uint64_t generation = 0;
    std::optional<FollowEditsViewState> replacement;
    friend bool operator==(const FollowEditsDelta&,
                           const FollowEditsDelta&) = default;
};

[[nodiscard]] FollowEditsDelta derive_follow_edits_delta(
    const FollowEditsViewState& base, const FollowEditsViewState& target);

struct FollowEditsFooterProjection {
    std::string mode;
    std::optional<std::string> resume_binding;
    std::optional<std::string> resume_command;
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
    const std::array<FollowEditsCommandDescriptor, 2>& descriptors()
        const noexcept {
        return descriptors_;
    }

private:
    const std::array<FollowEditsCommandDescriptor, 2> descriptors_{{
        {"follow_edits.resume"},
        {"follow_edits.pause"},
    }};
};

[[nodiscard]] FollowEditsCommandSet follow_edits_command_set();

struct FollowEditsConfig {
    std::size_t queue_capacity = 16;
    std::string resume_binding = "follow_edits.resume";
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

class FollowEditsModel {
public:
    explicit FollowEditsModel(FollowEditsConfig config = {});

    [[nodiscard]] FollowEditsResult attach_client(
        ClientId client, ViewportDimensions dimensions);
    [[nodiscard]] FollowEditsResult detach_client(ClientId client);
    [[nodiscard]] FollowEditsResult accept_external_change(
        const DiffFileView& file, Revision source_revision);
    [[nodiscard]] FollowEditsResult apply_navigation(
        const FollowNavigation& navigation);
    [[nodiscard]] FollowEditsResult pause();
    [[nodiscard]] FollowEditsResult resume(const DiffViewState& current_diff);

    [[nodiscard]] FollowEditsViewState view_state() const;
    [[nodiscard]] FollowEditsFooterProjection footer_projection() const;

private:
    [[nodiscard]] FollowTarget target_for(const DiffFileView& file,
                                          Revision source_revision) const;
    void activate(const FollowTarget& target);
    void advance_generation() noexcept;

    FollowEditsConfig config_;
    FollowEditsViewState state_;
    Revision latest_source_revision_{0};
};

}  // namespace ssg
