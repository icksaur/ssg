#pragma once

// Construction-time configuration types for SSG components.
//
// These are immutable value objects passed to constructors to separate
// configuration from operational state (cpp-lib-values.md).  They differ from
// SettingsModel (owned by the settings-model feature task): config.h types are
// fixed at object-construction time, while SettingsModel values may change
// during a live session and include multi-scope resolution, persistence, and
// delta publication.  When a component needs runtime-adjustable settings, it
// receives them through SettingsModel; when it needs invariant construction
// parameters, it receives them through types from this header.
//
// Construction failure: validated types throw std::invalid_argument with an
// actionable message when constructed with an out-of-range value.  This
// satisfies spec invariant I4 (valid state by construction; failures are
// actionable).  Factory functions and default constructors always produce
// valid objects.
//
// See doc/spec.md §Foundation infrastructure for the full contract.

#include <cstdint>
#include <stdexcept>
#include <string>

namespace ssg {

enum class IndentStyle : uint8_t {
    Spaces,
    Tabs,
};

// Line-ending convention stored in a document.
// `mixed` preserves per-line terminators until an explicit normalization
// command (spec §Design, file.set_line_ending).
enum class LineEnding : uint8_t {
    Lf,
    Crlf,
    Cr,
    Mixed,
};

class TabWidth {
public:
    static constexpr int kMinValue = 1;
    static constexpr int kMaxValue = 16;

    explicit TabWidth(int w);

    [[nodiscard]] int value() const noexcept { return value_; }
    bool operator==(TabWidth const&) const noexcept = default;

private:
    int value_{4};
};

// byte_budget = 0 disables history.
// coalesce_ms is the typing-coalescing window (spec §Design, default 750 ms).
struct HistoryConfig {
    uint64_t byteBudget{16u * 1024u * 1024u};
    uint32_t coalesceMs{750u};

    bool operator==(HistoryConfig const&) const noexcept = default;

    [[nodiscard]] static constexpr HistoryConfig defaults() noexcept {
        return {};
    }
};

struct IndentConfig {
    IndentStyle style{IndentStyle::Spaces};
    TabWidth width{4};
    bool autoDetect{true};

    bool operator==(IndentConfig const&) const noexcept = default;
};

}  // namespace ssg
