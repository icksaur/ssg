#include <ssg/TerminalOutput.h>

#include <ssg/Terminal.h>
#include <ssg/Theme.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace ssg {
namespace {

std::string lowercase(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

bool matchesAny(std::string_view value,
                std::initializer_list<std::string_view> options) {
    return std::ranges::any_of(options, [&](std::string_view option) {
        return value == option;
    });
}

} // namespace

ssg::ColorDepth detectColorDepth(char const* colorDepthOverride,
                                 char const* colorterm, char const* term,
                                 char const* termProgram) {
    if (colorDepthOverride != nullptr) {
        const auto overrideValue = lowercase(colorDepthOverride);
        if (matchesAny(overrideValue, {"truecolor", "24bit"})) {
            return ssg::ColorDepth::Truecolor;
        }
        if (matchesAny(overrideValue, {"256", "256color", "indexed256"})) {
            return ssg::ColorDepth::Indexed256;
        }
        if (matchesAny(overrideValue, {"16", "ansi16"})) {
            return ssg::ColorDepth::Ansi16;
        }
    }

    if (colorterm != nullptr) {
        const auto colortermValue = lowercase(colorterm);
        if (matchesAny(colortermValue, {"truecolor", "24bit"})) {
            return ssg::ColorDepth::Truecolor;
        }
    }

    const auto termValue =
        term == nullptr ? std::string{} : lowercase(std::string_view{term});
    if (termValue == "dumb" || termValue.empty()) {
        return ssg::ColorDepth::Ansi16;
    }

    const auto termProgramValue = termProgram == nullptr
                                      ? std::string{}
                                      : lowercase(std::string_view{termProgram});
    if (matchesAny(termProgramValue,
                   {"iterm.app", "wezterm", "vscode", "hyper", "ghostty"})) {
        return ssg::ColorDepth::Truecolor;
    }
    constexpr std::array<std::string_view, 6> termTruecolorHints{
        "kitty", "alacritty", "wezterm", "foot", "contour", "ghostty"};
    if (std::ranges::any_of(termTruecolorHints, [&](std::string_view hint) {
            return termValue.find(hint) != std::string::npos;
        })) {
        return ssg::ColorDepth::Truecolor;
    }

    return ssg::ColorDepth::Truecolor;
}

// Base64 for OSC 52, which carries its payload that way.  Written out rather
// than pulled in: this is the only base64 in the app, and a dependency for
// twenty lines would cost more than it saves.
std::string base64(std::string_view bytes) {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 2 < bytes.size()) {
        std::uint32_t const triple =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2]));
        encoded += kAlphabet[(triple >> 18) & 0x3f];
        encoded += kAlphabet[(triple >> 12) & 0x3f];
        encoded += kAlphabet[(triple >> 6) & 0x3f];
        encoded += kAlphabet[triple & 0x3f];
        index += 3;
    }
    if (index < bytes.size()) {
        std::uint32_t triple =
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << 16;
        bool const two = index + 1 < bytes.size();
        if (two) {
            triple |= static_cast<std::uint32_t>(
                          static_cast<unsigned char>(bytes[index + 1]))
                      << 8;
        }
        encoded += kAlphabet[(triple >> 18) & 0x3f];
        encoded += kAlphabet[(triple >> 12) & 0x3f];
        encoded += two ? kAlphabet[(triple >> 6) & 0x3f] : '=';
        encoded += '=';
    }
    return encoded;
}

std::string encodeClipboardWrite(std::string_view text) {
    // OSC 52 selection 'c' is the system clipboard.  Terminated with ST rather
    // than BEL: both are accepted, and ST is the form the standard specifies.
    return "\x1b]52;c;" + base64(text) + "\x1b\\";
}

std::optional<std::string> SystemClipboardWriter::bytesFor(
    std::optional<ssg::ClipboardWrite> const& write, bool terminalCanWrite) {
    if (!write || !terminalCanWrite) return std::nullopt;
    if (served_ && *served_ == write->id) return std::nullopt;
    served_ = write->id;
    return encodeClipboardWrite(write->text);
}

std::string encodeFrame(ssg::CellGrid const& screen, ssg::ColorDepth depth,
                         bool showCursor) {
    std::string frame;
    {
        // Appends to the frame rather than writing to the terminal, so the whole
        // frame is still one write.
        TerminalModes modes{
            [&frame](std::string_view bytes) { frame.append(bytes); }};
        auto const hidden = modes.enter(kCursorHidden);
        frame += encodeAnsiFrame(screen, depth);
        if (screen.caret && showCursor) {
            frame += "\x1b[" + std::to_string(screen.caret->row + 1) + ";" +
                     std::to_string(screen.caret->column + 1) + "H";
        }
    }
    if (!showCursor) {
        // Painting hides the cursor and the guard above shows it again, which is
        // right for a normal frame.  While a scrollbar thumb is being dragged the
        // caret is not what the user is looking at, and a visible cursor lands
        // wherever painting ended -- it flickers around the screen chasing each
        // frame.  Re-hide it, using the mode's own declared bytes rather than a
        // hand-written escape.  Deliberately NOT balanced within the frame: it is
        // meant to persist until the drag ends and the next frame shows it again,
        // and terminal teardown restores every mode in kAllModes unconditionally,
        // so no exit path can leave the cursor hidden
        frame.append(kCursorHidden.enter);
    }
    return frame;
}

namespace {

std::string_view underlineSgr(ssg::CellUnderline underline) {
    // SGR 4:3 is a CURLY underline and 58;5;n colours it independently of the
    // text, so a squiggle sits under syntax-coloured code without recolouring
    // it.  Both are widely supported and, crucially, DEGRADE WELL: a terminal
    // that does not know 4:3 draws a plain underline, and one that does not know
    // 58 draws it in the text colour.  Nothing is queried because there is
    // nothing to ask -- and nothing worth refusing to draw.
    switch (underline) {
    case ssg::CellUnderline::Error: return "\x1b[4:3m\x1b[58;5;1m";
    case ssg::CellUnderline::Warning: return "\x1b[4:3m\x1b[58;5;3m";
    case ssg::CellUnderline::Info: return "\x1b[4:2m\x1b[58;5;4m";
    case ssg::CellUnderline::None: return "\x1b[4:0m\x1b[59m";
    }
    return "\x1b[4:0m\x1b[59m";
}

}  // namespace

std::string encodeAnsiFrame(ssg::CellGrid const& screen, ssg::ColorDepth depth) {    constexpr std::size_t maxIndex = ssg::kThemeColorSlotCount - 1;
    auto color = [&](ssg::SrgbColor c, char kind) -> std::string {
        auto const resolved = ssg::ColorResolver{depth}.resolve(c);
        switch (resolved.encoding) {
            case ssg::ColorDepth::Truecolor:
                return "\x1b[" + std::string{kind} + "8;2;" +
                       std::to_string(resolved.rgb.red) + ";" +
                       std::to_string(resolved.rgb.green) + ";" +
                       std::to_string(resolved.rgb.blue) + "m";
            case ssg::ColorDepth::Indexed256:
                return "\x1b[" + std::string{kind} + "8;5;" +
                       std::to_string(resolved.index) + "m";
            case ssg::ColorDepth::Ansi16: {
                int const base = kind == '3' ? 30 : 40;
                int const bright = kind == '3' ? 90 : 100;
                int const code = resolved.index < 8
                                     ? base + resolved.index
                                     : bright + (resolved.index - 8);
                return "\x1b[" + std::to_string(code) + "m";
            }
        }
        return {};
    };
    auto paletteColor = [&](std::uint8_t index, char kind) {
        return color(screen.colors[std::min<std::size_t>(index, maxIndex)], kind);
    };
    auto tintColor = [&](ssg::DiffTint tint) {
        switch (tint) {
            case ssg::DiffTint::AddedRow: return screen.diffTints.addedRow;
            case ssg::DiffTint::RemovedRow: return screen.diffTints.removedRow;
            case ssg::DiffTint::ModifiedRow: return screen.diffTints.modifiedRow;
            case ssg::DiffTint::AddedWord: return screen.diffTints.addedWord;
            case ssg::DiffTint::RemovedWord: return screen.diffTints.removedWord;
            case ssg::DiffTint::ModifiedWord: return screen.diffTints.modifiedWord;
            case ssg::DiffTint::None: break;
        }
        return screen.diffTints.addedRow;
    };

    std::string out = "\x1b[H";
    int const columns = screen.size.columns;
    int const rows = screen.size.rows;
    // Where each clickable run starts and ends, so the OSC 8 open and close land
    // exactly around it.  A hyperlink left open would make the rest of the line
    // clickable, which is worse than not linking at all.
    std::unordered_map<std::int64_t, std::string const*> linkOpensAt;
    std::unordered_set<std::int64_t> linkClosesAt;
    auto const cellKey = [columns](int x, int y) {
        return static_cast<std::int64_t>(y) * columns + x;
    };
    for (auto const& link : screen.hyperlinks) {
        if (link.width <= 0) continue;
        linkOpensAt.emplace(cellKey(link.column, link.row), &link.uri);
        linkClosesAt.insert(cellKey(link.column + link.width, link.row));
    }
    for (int y = 0; y < rows; ++y) {
        out += "\x1b[" + std::to_string(y + 1) + ";1H\x1b[0m";
        int foreground = -1;
        int background = -1;
        int role = -1;
        auto tint = ssg::DiffTint::None;
        auto underline = ssg::CellUnderline::None;
        for (int x = 0; x < columns; ++x) {
            auto const& cell =
                screen.cells[static_cast<std::size_t>(y * columns + x)];
            if (cell.continuation) continue;
            // OSC 8: the terminal makes the bytes between open and close
            // clickable.  Emitted around the run rather than per cell, and
            // always closed, so a link cannot bleed into the text after it.
            auto const key = cellKey(x, y);
            if (linkClosesAt.contains(key)) out += "\x1b]8;;\x1b\\";
            if (auto const opens = linkOpensAt.find(key);
                opens != linkOpensAt.end()) {
                out += "\x1b]8;;" + *opens->second + "\x1b\\";
            }
            if (cell.foreground != foreground || cell.background != background ||
                cell.tint != tint || cell.underline != underline ||
                static_cast<int>(cell.role) != role) {
                out += paletteColor(cell.foreground, '3');
                if (cell.tint != ssg::DiffTint::None) {
                    out += color(tintColor(cell.tint), '4');
                } else if (cell.role == ssg::SemanticRole::Selection) {
                    out += color(screen.selectionFill, '4');
                } else {
                    out += paletteColor(cell.background, '4');
                }
                out += underlineSgr(cell.underline);
                foreground = cell.foreground;
                background = cell.background;
                tint = cell.tint;
                underline = cell.underline;
                role = static_cast<int>(cell.role);
            }
            out += cell.text.empty() ? std::string{" "} : cell.text;
        }
        // A run reaching the last column has no cell to close at, so close here
        // rather than leaving the link open into the next row.
        if (linkClosesAt.contains(cellKey(columns, y))) out += "\x1b]8;;\x1b\\";
    }
    out += "\x1b[0m";
    return out;
}

} // namespace ssg
