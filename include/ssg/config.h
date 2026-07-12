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

// ---------------------------------------------------------------------------
// Enumerations

// Indentation style for new text insertion.
enum class IndentStyle : uint8_t {
    spaces,
    tabs,
};

// Line-ending convention stored in a document.
// `mixed` preserves per-line terminators until an explicit normalization
// command (spec §Design, file.set_line_ending).
enum class LineEnding : uint8_t {
    lf,
    crlf,
    cr,
    mixed,
};

// ---------------------------------------------------------------------------
// Validated value types

// Validated tab-stop and indentation width. Accepts values in [1, 16].
// Throws std::invalid_argument for out-of-range inputs (I4).
class TabWidth {
public:
    static constexpr int min_value = 1;
    static constexpr int max_value = 16;

    // Throws std::invalid_argument if w < min_value or w > max_value.
    explicit TabWidth(int w);

    [[nodiscard]] int  value() const noexcept { return value_; }
    bool operator==(TabWidth const&) const noexcept = default;

private:
    int value_{4};
};

// ---------------------------------------------------------------------------
// Configuration aggregates

// Per-document undo/redo history configuration.
// byte_budget = 0 disables history.
// coalesce_ms is the typing-coalescing window (spec §Design, default 750 ms).
struct HistoryConfig {
    uint64_t byte_budget{16u * 1024u * 1024u};
    uint32_t coalesce_ms{750u};

    bool operator==(HistoryConfig const&) const noexcept = default;

    // Returns the spec-default configuration.
    [[nodiscard]] static constexpr HistoryConfig defaults() noexcept {
        return {};
    }
};

// Indentation policy passed to document and language configurations.
// All fields have valid defaults; TabWidth{4} is the standard default width.
struct IndentConfig {
    IndentStyle style{IndentStyle::spaces};
    TabWidth    width{4};
    bool        auto_detect{true};

    bool operator==(IndentConfig const&) const noexcept = default;
};

}  // namespace ssg
