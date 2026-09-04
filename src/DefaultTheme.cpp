#include <ssg/Theme.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ssg {

// The compiled-in built-in theme (see defaultTheme()'s doc comment in Theme.h):
// EditorSession::create()'s starting ThemeSnapshot, before any init.lua
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
    // A dark theme (see doc for the color model). The palette draws from the
    // caco "dark" theme (../caco/public/themes/dark.css + style.css): a near-black
    // document, a dark-green selection, and greyscale chrome, with syntax the
    // only saturated color. Design intent, per role group below:
    //  - editor text and file/folder names read WHITE; other UI text is greyscale
    //    and progressively dimmer (active > header/footer > inactive/line numbers).
    //  - selection (editor AND the panel/picker "bar") is a dark green, the caco
    //    list-selection color (green mixed into the base).
    //  - the chrome bands (tab bar, header, footer, gutter) are distinct greyscale
    //    shades rather than one flat band, so the regions read apart.
    //  - syntax stays colorful (it is explicitly outside the greyscale rule).
    auto rgb = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return SrgbColor::fromSerializedChannels(r, g, b);
    };

    ThemeSnapshot snapshot{};
    auto role = [&](SemanticRole which, SrgbColor color) {
        snapshot.roleColors[static_cast<std::size_t>(which)] = color;
    };

    // --- Foreground text (greyscale except where noted) ---
    role(SemanticRole::Text, rgb(235, 235, 235));   // editor text + file names: white
    role(SemanticRole::Caret, rgb(255, 255, 255));
    role(SemanticRole::PanelActive, rgb(228, 232, 238));   // directory names / active panel: white, faint cool tint
    role(SemanticRole::PanelInactive, rgb(140, 140, 146));  // inactive panel provider: dim grey
    role(SemanticRole::Header, rgb(166, 166, 172));   // header text: mid grey
    role(SemanticRole::Footer, rgb(166, 166, 172));   // footer text: mid grey
    role(SemanticRole::TabActive, rgb(232, 232, 235));   // active tab: bright, stands out
    role(SemanticRole::TabInactive, rgb(138, 138, 144));  // inactive tabs: dim grey
    role(SemanticRole::StatusInfo, rgb(150, 150, 156));  // status text: grey
    role(SemanticRole::StatusWarning, rgb(229, 192, 123));  // amber: a warning must not be greyscale
    role(SemanticRole::LineNumber, rgb(96, 96, 102));    // inactive gutter numbers: dim
    role(SemanticRole::CurrentLineNumber, rgb(222, 222, 228));  // current line number: bright
    role(SemanticRole::Prompt, rgb(216, 216, 222));  // prompt input text: near-white
    role(SemanticRole::ScrollbarThumb, rgb(120, 120, 128));

    // --- Backgrounds ---
    role(SemanticRole::Canvas, rgb(30, 30, 30));   // document: near-black (caco --base)
    role(SemanticRole::TreeBackground, rgb(35, 35, 39));   // side panel: a hair off the document
    role(SemanticRole::Selection, rgb(42, 78, 46));   // dark green (caco list selection)
    role(SemanticRole::TreeFocus, rgb(42, 78, 46));   // the selected panel/tree row: same green
    role(SemanticRole::SearchMatch, rgb(92, 78, 30));   // inactive search match: dark amber (active match uses Selection green)
    role(SemanticRole::TabInactiveBackground, rgb(52, 52, 58));  // tab-bar band: the lightest chrome shade
    role(SemanticRole::HeaderBackground, rgb(44, 44, 48));   // header band
    role(SemanticRole::FooterBackground, rgb(38, 38, 42));   // footer band: the darkest chrome shade
    role(SemanticRole::CurrentLineNumberBackground, rgb(46, 46, 50));
    role(SemanticRole::LineNumberBackground, rgb(34, 34, 38));  // inactive gutter band
    role(SemanticRole::ScrollbarTrack, rgb(52, 52, 58));

    // --- Diff (foreground / row tint; saturated so changes are legible) ---
    role(SemanticRole::DiffAdded, rgb(76, 175, 80));    // caco green
    role(SemanticRole::DiffRemoved, rgb(239, 74, 74));   // caco red
    role(SemanticRole::DiffModified, rgb(229, 192, 123));  // amber, distinct from added green

    // --- Syntax (the only saturated foreground text; caco accent palette) ---
    auto syntax = [&](SyntaxScope scope, SrgbColor color) {
        snapshot.syntaxColors[static_cast<std::size_t>(scope)] = color;
    };
    syntax(SyntaxScope::PlainText, rgb(235, 235, 235));   // matches editor Text (white)
    syntax(SyntaxScope::Comment, rgb(110, 116, 110));   // dim grey-green
    syntax(SyntaxScope::Keyword, rgb(171, 113, 220));   // purple
    syntax(SyntaxScope::String, rgb(129, 193, 133));   // green (lighter than the diff/selection green)
    syntax(SyntaxScope::Number, rgb(212, 149, 106));   // orange (caco --orange)
    syntax(SyntaxScope::Type, rgb(78, 201, 176));    // teal
    syntax(SyntaxScope::Function, rgb(97, 175, 239));   // blue
    syntax(SyntaxScope::Variable, rgb(220, 220, 226));  // near-white: identifiers read like editor text
    syntax(SyntaxScope::OperatorToken, rgb(198, 198, 204));  // light grey
    syntax(SyntaxScope::Punctuation, rgb(198, 198, 204));  // light grey
    syntax(SyntaxScope::Invalid, rgb(239, 74, 74));    // red
    return snapshot;
}

}  // namespace ssg
