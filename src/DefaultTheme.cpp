#include "ssg/Theme.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ssg {

// The compiled-in built-in theme (see defaultTheme()'s doc comment in
// Theme.h): EditorRuntime::create()'s starting ThemeSnapshot, before any
// init.lua theme.define() call runs. This table lives in its OWN
// translation unit, separate from Theme.cpp, deliberately: Theme.cpp is
// the generic color-parsing/derivation engine and is scanned by
// tests/test_theme.cpp's sourceAndConfigHaveNoIndependentColorSources for
// hardcoded literal colors (hex strings, SrgbColor construction, bare RGB
// tuples) precisely so it never grows a second, parallel place a color
// value could be hardcoded outside a caller-supplied theme.define table --
// this file is the one narrow, reviewed exception to that rule, holding
// the literal values a theme must start from before any override exists.
ThemeSnapshot defaultTheme() noexcept {
    // Readable dark theme derived from the VSCode-style palette in
    // caco/public/themes/dark.css. Low indices are dark fills, high indices
    // are light text, hues sit in the middle. Role assignments keep every
    // co_visible_role_pairs member on a distinct palette index. This is the
    // ONE compiled-in copy of these values -- do not duplicate them
    // elsewhere (see this file's header comment above).
    ThemeSnapshot snapshot{};
    constexpr std::array<std::array<std::uint8_t, 3>, kThemePaletteSize>
        palette{{
            {30, 30, 30},
            {212, 212, 212},
            {62, 62, 66},
            {133, 133, 133},
            {77, 170, 252},
            {229, 192, 123},
            {239, 74, 74},
            {76, 175, 80},
            {171, 71, 188},
            {38, 192, 192},
            {212, 149, 106},
            {209, 109, 158},
            {187, 187, 187},
            {106, 106, 106},
            {232, 232, 232},
            {255, 255, 255},
        }};
    for (std::size_t index = 0; index < snapshot.palette.size(); ++index) {
        snapshot.palette[index] = SrgbColor::fromSerializedChannels(
            palette[index][0], palette[index][1], palette[index][2]);
    }

    auto role = [&](SemanticRole which, std::uint8_t index) {
        snapshot.semanticIndices[static_cast<std::size_t>(which)] = index;
    };
    role(SemanticRole::Foreground, 1);
    role(SemanticRole::Background, 0);
    role(SemanticRole::Caret, 15);
    role(SemanticRole::Selection, 4);
    role(SemanticRole::DiagnosticError, 6);
    role(SemanticRole::DiagnosticWarning, 5);
    role(SemanticRole::DiagnosticInfo, 9);
    role(SemanticRole::DiagnosticHint, 8);
    role(SemanticRole::GitAdded, 7);
    role(SemanticRole::GitModified, 10);
    role(SemanticRole::GitDeleted, 6);
    role(SemanticRole::GitConflict, 11);
    role(SemanticRole::TreeBackground, 2);
    role(SemanticRole::TreeFocus, 4);
    role(SemanticRole::TabActive, 4);
    role(SemanticRole::TabInactive, 13);
    role(SemanticRole::PanelActive, 9);
    role(SemanticRole::PanelInactive, 3);
    role(SemanticRole::Header, 12);
    role(SemanticRole::Footer, 12);
    role(SemanticRole::StatusInfo, 9);
    role(SemanticRole::StatusWarning, 5);
    role(SemanticRole::StatusError, 6);
    role(SemanticRole::LineNumber, 3);
    role(SemanticRole::ActiveLineNumber, 14);
    role(SemanticRole::SearchMatch, 10);
    role(SemanticRole::Prompt, 8);
    role(SemanticRole::ScrollbarTrack, 2);
    role(SemanticRole::ScrollbarThumb, 13);
    role(SemanticRole::DiffAdded, 7);
    role(SemanticRole::DiffRemoved, 6);
    role(SemanticRole::DiffModified, 10);

    auto syntax = [&](SyntaxScope scope, std::uint8_t index) {
        snapshot.syntaxIndices[static_cast<std::size_t>(scope)] = index;
    };
    syntax(SyntaxScope::PlainText, 1);
    syntax(SyntaxScope::Comment, 3);
    syntax(SyntaxScope::Keyword, 8);
    syntax(SyntaxScope::String, 7);
    syntax(SyntaxScope::Number, 10);
    syntax(SyntaxScope::Type, 9);
    syntax(SyntaxScope::Function, 4);
    syntax(SyntaxScope::Variable, 1);
    syntax(SyntaxScope::OperatorToken, 5);
    syntax(SyntaxScope::Punctuation, 14);
    syntax(SyntaxScope::Invalid, 6);
    snapshot.diffTints = deriveDiffTints(
        snapshot.palette, snapshot.semanticIndices, snapshot.syntaxIndices);
    snapshot.selectionFill = deriveSelectionFill(
        snapshot.palette, snapshot.semanticIndices, snapshot.syntaxIndices);
    return snapshot;
}

}  // namespace ssg
