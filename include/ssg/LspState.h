#pragma once

#include <ssg/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct LspPosition {
    std::uint64_t line = 0;
    std::uint64_t character = 0;
    friend bool operator==(const LspPosition&, const LspPosition&) = default;
};

enum class LspPositionError : std::uint8_t {
    None,
    InvalidUtf8,
    InvalidUtf8Boundary,
    LineOutOfRange,
    CharacterOutOfRange,
    SplitSurrogate,
};

struct LspPositionResult {
    LspPosition position;
    LspPositionError error = LspPositionError::None;
};

struct LspByteOffsetResult {
    ByteOffset offset;
    LspPositionError error = LspPositionError::None;

    [[nodiscard]] bool accepted() const noexcept {
        return error == LspPositionError::None;
    }
};

[[nodiscard]] LspPositionResult byteOffsetToLspPosition(
    std::string_view utf8, ByteOffset offset);
[[nodiscard]] LspByteOffsetResult lspPositionToByteOffset(
    std::string_view utf8, LspPosition position);

struct LspRange {
    LspPosition start;
    LspPosition end;
    friend bool operator==(const LspRange&, const LspRange&) = default;
};

enum class LspDiagnosticSeverity : std::uint8_t {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

struct LspDiagnostic {
    LspRange range;
    std::optional<LspDiagnosticSeverity> severity;
    std::string code;
    std::string message;
    friend bool operator==(const LspDiagnostic&, const LspDiagnostic&) = default;
};

struct LspDocumentDiagnostics {
    std::string uri;
    std::uint64_t revision{0};
    std::vector<LspDiagnostic> diagnostics;
    friend bool operator==(const LspDocumentDiagnostics&,
                           const LspDocumentDiagnostics&) = default;
};

struct LspSyncViewState {
    std::uint64_t revision{0};
    std::vector<LspDocumentDiagnostics> documents;
    friend bool operator==(const LspSyncViewState&,
                           const LspSyncViewState&) = default;
};

struct LspCompletionItem {
    std::string label;
    std::string detail;
    std::string sortText;
    std::string insertText;
    std::optional<LspRange> replacementRange;
    friend bool operator==(const LspCompletionItem&,
                           const LspCompletionItem&) = default;
};

struct LspCompletionViewState {
    bool visible = false;
    bool loading = false;
    std::vector<LspCompletionItem> items;
    std::optional<std::size_t> selectedIndex;
    friend bool operator==(const LspCompletionViewState&,
                           const LspCompletionViewState&) = default;
};

struct LspHover {
    std::string contents;
    std::optional<LspRange> range;
    friend bool operator==(const LspHover&, const LspHover&) = default;
};

struct LspNavigationTarget {
    std::string uri;
    LspRange range;
    friend bool operator==(const LspNavigationTarget&,
                           const LspNavigationTarget&) = default;
};

struct LspNavigationViewState {
    std::vector<LspNavigationTarget> targets;
    std::optional<std::size_t> selectedIndex;
    bool userNavigation = false;
    bool revealPrimaryCaret = false;
    friend bool operator==(const LspNavigationViewState&,
                           const LspNavigationViewState&) = default;
};

struct LspFeatureViewState {
    std::uint64_t revision{0};
    LspCompletionViewState completion;
    std::optional<LspHover> hover;
    LspNavigationViewState navigation;
    std::string status;
    friend bool operator==(const LspFeatureViewState&,
                           const LspFeatureViewState&) = default;
};

} // namespace ssg
