#pragma once

#include <ssg/UiNodeState.h>
#include <ssg/UiPresence.h>
#include <ssg/UiTree.h>
#include <ssg/focus.h>

#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace ssg {

struct UiFrameVersion {
    Generation generation{0};
    PresenceBasis presenceBasis{0};

    friend bool operator==(const UiFrameVersion&, const UiFrameVersion&) = default;
};

class UiFrame {
public:
    UiFrame();

    [[nodiscard]] static std::optional<UiFrame> create(
        UiSchema schema, UiStateSection state, UiPresenceSection presence);
    [[nodiscard]] static UiFrame require(
        UiSchema schema, UiStateSection state, UiPresenceSection presence);

    [[nodiscard]] const UiSchema& schema() const noexcept { return schema_; }
    [[nodiscard]] const UiStateSection& state() const noexcept { return state_; }
    [[nodiscard]] const UiPresenceSection& presence() const noexcept {
        return presence_;
    }
    [[nodiscard]] const std::vector<UiNodeId>& focusPath() const noexcept {
        return *state_.focusPath;
    }
    [[nodiscard]] FocusTarget effectiveFocus() const noexcept;
    [[nodiscard]] UiFrameVersion version() const noexcept {
        return {schema_.generation, presence_.basis};
    }

    friend bool operator==(const UiFrame&, const UiFrame&) = default;

private:
    struct ValidatedTag {};
    UiFrame(UiSchema schema, UiStateSection state, UiPresenceSection presence,
            ValidatedTag)
        : schema_{std::move(schema)},
          state_{std::move(state)},
          presence_{std::move(presence)} {}

    UiSchema schema_;
    UiStateSection state_;
    UiPresenceSection presence_;
};

struct UiFrameReplacement {
    UiFrame frame;
};

struct UiFrameChanges {
    std::vector<UiNodeState> state;
    std::vector<UiPresenceRecord> presence;
    bool focusPathChanged = false;
    std::optional<std::vector<UiNodeId>> focusPath;
};

using UiFrameDeltaBody =
    std::variant<UiFrameReplacement, UiFrameChanges>;

class UiFrameDelta {
public:
    [[nodiscard]] static UiFrameDelta replacement(
        UiFrameVersion base, UiFrame frame);
    [[nodiscard]] static UiFrameDelta changes(
        UiFrameVersion base, UiFrameVersion target, UiFrameChanges changes);

    [[nodiscard]] UiFrameVersion base() const noexcept { return base_; }
    [[nodiscard]] UiFrameVersion target() const noexcept { return target_; }
    [[nodiscard]] const UiFrameDeltaBody& body() const noexcept { return body_; }

private:
    UiFrameDelta(UiFrameVersion base, UiFrameVersion target,
                 UiFrameDeltaBody body)
        : base_{base}, target_{target}, body_{std::move(body)} {}

    UiFrameVersion base_;
    UiFrameVersion target_;
    UiFrameDeltaBody body_;
};

enum class UiFrameReplayError {
    None,
    StaleVersion,
    MalformedDelta,
    InvalidFrame,
};

struct UiFrameReplayResult {
    std::optional<UiFrame> frame;
    UiFrameReplayError error = UiFrameReplayError::None;

    [[nodiscard]] bool accepted() const noexcept { return frame.has_value(); }
};

class UiFrameDeltaCodec {
public:
    [[nodiscard]] UiFrameDelta derive(const UiFrame& base,
                                      const UiFrame& target) const;
    [[nodiscard]] UiFrameReplayResult replay(const UiFrame& base,
                                             const UiFrameDelta& delta) const;
};

}  // namespace ssg
