#include <ssg/platform_files.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace ssg {
namespace {

struct Utf8Length {
    bool valid = true;
    std::size_t utf16Units = 0;
};

Utf8Length utf8Length(std::string_view text) noexcept {
    Utf8Length result;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::uint32_t codepoint = 0;
        std::size_t length = 0;
        if (lead < 0x80) {
            codepoint = lead;
            length = 1;
        } else if ((lead & 0xe0) == 0xc0) {
            codepoint = lead & 0x1f;
            length = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            codepoint = lead & 0x0f;
            length = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            codepoint = lead & 0x07;
            length = 4;
        } else {
            result.valid = false;
            return result;
        }
        if (i + length > text.size()) {
            result.valid = false;
            return result;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[i + continuation]);
            if ((byte & 0xc0) != 0x80) {
                result.valid = false;
                return result;
            }
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        const std::array minimum{std::uint32_t{0}, std::uint32_t{0x80},
                                 std::uint32_t{0x800}, std::uint32_t{0x10000}};
        if (codepoint < minimum[length - 1] || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            result.valid = false;
            return result;
        }
        result.utf16Units += codepoint > 0xffff ? 2 : 1;
        i += length;
    }
    return result;
}

bool isWindowsReserved(std::string_view component) {
    const auto dot = component.find('.');
    component = component.substr(0, dot);
    std::string base(component);
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        base == "CLOCK$") {
        return true;
    }
    if (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT"))) {
        return base[3] >= '1' && base[3] <= '9';
    }
    return false;
}

PathValidation validateComponent(std::string_view component,
                                  PathSyntax syntax,
                                  std::size_t index) noexcept {
    if (component.empty() || component == "." || component == "..") {
        return {PathError::Traversal, index};
    }

    const auto length = utf8Length(component);
    if (!length.valid) {
        return {PathError::InvalidUtf8, index};
    }

    if (syntax == PathSyntax::Linux) {
        if (component.find('\0') != std::string_view::npos) {
            return {PathError::InvalidCharacter, index};
        }
        if (component.size() > 255) {
            return {PathError::ComponentTooLong, index};
        }
        return {};
    }

    for (const unsigned char value : component) {
        if (value < 32 || value == '<' || value == '>' || value == ':' ||
            value == '"' || value == '|' || value == '?' || value == '*' ||
            value == '\0') {
            return {PathError::InvalidCharacter, index};
        }
    }
    if (component.back() == '.' || component.back() == ' ') {
        return {PathError::TrailingDotOrSpace, index};
    }
    if (isWindowsReserved(component)) {
        return {PathError::ReservedName, index};
    }
    if (length.utf16Units > 255) {
        return {PathError::ComponentTooLong, index};
    }
    return {};
}

} // namespace

PathValidation validateWorkspaceRelativePath(std::string_view path,
                                                PathSyntax syntax,
                                                LongPathPolicy longPaths) noexcept {
    if (path.empty()) {
        return {PathError::Empty, 0};
    }
    if (path.front() == '/' ||
        (syntax == PathSyntax::Windows &&
         (path.front() == '\\' ||
          (path.size() >= 2 &&
           std::isalpha(static_cast<unsigned char>(path.front())) &&
           path[1] == ':')))) {
        return {PathError::Absolute, 0};
    }

    const auto isSeparator = [syntax](char value) {
        return value == '/' || (syntax == PathSyntax::Windows && value == '\\');
    };
    std::size_t componentStart = 0;
    std::size_t componentIndex = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size() && !isSeparator(path[i])) {
            continue;
        }
        const auto validation =
            validateComponent(path.substr(componentStart, i - componentStart),
                               syntax, componentIndex);
        if (!validation.valid()) {
            return validation;
        }
        componentStart = i + 1;
        ++componentIndex;
    }

    const auto totalLength = utf8Length(path);
    if (!totalLength.valid) {
        return {PathError::InvalidUtf8, 0};
    }
    const std::size_t maximum =
        syntax == PathSyntax::Linux
            ? 4095
            : (longPaths == LongPathPolicy::Legacy ? 259 : 32766);
    const std::size_t measured =
        syntax == PathSyntax::Linux ? path.size() : totalLength.utf16Units;
    if (measured > maximum) {
        return {PathError::PathTooLong, 0};
    }
    return {};
}

} // namespace ssg
