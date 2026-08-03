#include "ssg/Theme.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ssg {

// The compiled-in built-in theme (see defaultTheme()'s doc comment in Theme.h):
// EditorRuntime::create()'s starting ThemeSnapshot, before any init.lua
// theme.set() call runs. This table lives in its OWN translation unit, separate
// from Theme.cpp, deliberately: Theme.cpp is scanned by
// tests/test_theme.cpp's sourceAndConfigHaveNoIndependentColorSources for
// hardcoded literal colors precisely so it never grows a second, parallel place
// a color could be hardcoded outside a caller-supplied theme.set table -- this
// file is the one narrow, reviewed exception, holding the literal values a theme
// starts from before any override exists.
//
// Each role and scope has its OWN color (no shared 16-slot palette). The values
// below are the historical VSCode-derived palette, assigned per role/scope so
// the shipped appearance is unchanged from the palette+index model.
ThemeSnapshot defaultTheme() noexcept {
    // The historical 16-color palette this theme was seeded from, kept local so
    // the per-role assignments below read as "role = <one of these tones>".
    constexpr std::array<std::array<std::uint8_t, 3>, 16> tone{{
        {30, 30, 30},     // 0  near-black (document background)
        {212, 212, 212},  // 1  light gray (foreground)
        {62, 62, 66},     // 2  dark gray (chrome band)
        {133, 133, 133},  // 3  mid gray
        {77, 170, 252},   // 4  blue
        {229, 192, 123},  // 5  amber
        {239, 74, 74},    // 6  red
        {76, 175, 80},    // 7  green
        {171, 71, 188},   // 8  purple
        {38, 192, 192},   // 9  cyan
        {212, 149, 106},  // 10 orange
        {209, 109, 158},  // 11 pink
        {187, 187, 187},  // 12 light gray
        {106, 106, 106},  // 13 mid-dark gray
        {232, 232, 232},  // 14 near-white
        {255, 255, 255},  // 15 white
    }};
    auto c = [&](std::size_t i) {
        return SrgbColor::fromSerializedChannels(tone[i][0], tone[i][1],
                                                 tone[i][2]);
    };

    ThemeSnapshot snapshot{};
    auto role = [&](SemanticRole which, std::size_t toneIndex) {
        snapshot.roleColors[static_cast<std::size_t>(which)] = c(toneIndex);
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
    role(SemanticRole::TabInactive, 12);
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
    role(SemanticRole::ScrollbarTrack, 13);
    role(SemanticRole::ScrollbarThumb, 3);
    role(SemanticRole::DiffAdded, 7);
    role(SemanticRole::DiffRemoved, 6);
    role(SemanticRole::DiffModified, 10);
    role(SemanticRole::TabInactiveBackground, 2);
    role(SemanticRole::HeaderBackground, 2);
    role(SemanticRole::FooterBackground, 2);

    auto syntax = [&](SyntaxScope scope, std::size_t toneIndex) {
        snapshot.syntaxColors[static_cast<std::size_t>(scope)] = c(toneIndex);
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
    return snapshot;
}

}  // namespace ssg
