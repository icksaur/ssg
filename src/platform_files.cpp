#include "ssg/platform_files.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace ssg {
namespace {

struct Utf8Length {
    bool valid = true;
    std::size_t utf16_units = 0;
};

Utf8Length utf8_length(std::string_view text) noexcept {
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
        result.utf16_units += codepoint > 0xffff ? 2 : 1;
        i += length;
    }
    return result;
}

bool is_windows_reserved(std::string_view component) {
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

PathValidation validate_component(std::string_view component,
                                  PathSyntax syntax,
                                  std::size_t index) noexcept {
    if (component.empty() || component == "." || component == "..") {
        return {PathError::traversal, index};
    }

    const auto length = utf8_length(component);
    if (!length.valid) {
        return {PathError::invalid_utf8, index};
    }

    if (syntax == PathSyntax::linux) {
        if (component.find('\0') != std::string_view::npos) {
            return {PathError::invalid_character, index};
        }
        if (component.size() > 255) {
            return {PathError::component_too_long, index};
        }
        return {};
    }

    for (const unsigned char value : component) {
        if (value < 32 || value == '<' || value == '>' || value == ':' ||
            value == '"' || value == '|' || value == '?' || value == '*' ||
            value == '\0') {
            return {PathError::invalid_character, index};
        }
    }
    if (component.back() == '.' || component.back() == ' ') {
        return {PathError::trailing_dot_or_space, index};
    }
    if (is_windows_reserved(component)) {
        return {PathError::reserved_name, index};
    }
    if (length.utf16_units > 255) {
        return {PathError::component_too_long, index};
    }
    return {};
}

} // namespace

PathValidation validate_workspace_relative_path(std::string_view path,
                                                PathSyntax syntax,
                                                LongPathPolicy long_paths) noexcept {
    if (path.empty()) {
        return {PathError::empty, 0};
    }
    if (path.front() == '/' ||
        (syntax == PathSyntax::windows &&
         (path.front() == '\\' ||
          (path.size() >= 2 &&
           std::isalpha(static_cast<unsigned char>(path.front())) &&
           path[1] == ':')))) {
        return {PathError::absolute, 0};
    }

    const auto is_separator = [syntax](char value) {
        return value == '/' || (syntax == PathSyntax::windows && value == '\\');
    };
    std::size_t component_start = 0;
    std::size_t component_index = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size() && !is_separator(path[i])) {
            continue;
        }
        const auto validation =
            validate_component(path.substr(component_start, i - component_start),
                               syntax, component_index);
        if (!validation.valid()) {
            return validation;
        }
        component_start = i + 1;
        ++component_index;
    }

    const auto total_length = utf8_length(path);
    if (!total_length.valid) {
        return {PathError::invalid_utf8, 0};
    }
    const std::size_t maximum =
        syntax == PathSyntax::linux
            ? 4095
            : (long_paths == LongPathPolicy::legacy ? 259 : 32766);
    const std::size_t measured =
        syntax == PathSyntax::linux ? path.size() : total_length.utf16_units;
    if (measured > maximum) {
        return {PathError::path_too_long, 0};
    }
    return {};
}

} // namespace ssg
