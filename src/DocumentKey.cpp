#include <ssg/DocumentKey.h>

#include <cstdint>
#include <random>
#include <stdexcept>

namespace ssg {
namespace {

bool validUtf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t continuation = 0;
        std::uint32_t scalar = 0;
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1;
            scalar = first & 0x1fU;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2;
            scalar = first & 0x0fU;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3;
            scalar = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= value.size()) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next =
                static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) return false;
            scalar = (scalar << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2 && scalar < 0x800U) ||
            (continuation == 3 && scalar < 0x10000U) ||
            scalar > 0x10ffffU ||
            (scalar >= 0xd800U && scalar <= 0xdfffU)) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

} // namespace

UntitledDocumentId UntitledDocumentId::generate() {
    std::array<std::byte, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<std::byte>(random() & 0xffU);
    }
    bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
    bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
    return UntitledDocumentId{bytes};
}

DocumentKey DocumentKey::saved(std::string_view workspaceRelativePath) {
    if (workspaceRelativePath.empty() ||
        workspaceRelativePath.front() == '/' ||
        workspaceRelativePath.front() == '\\' ||
        workspaceRelativePath.find('\0') != std::string_view::npos ||
        !validUtf8(workspaceRelativePath)) {
        throw std::invalid_argument(
            "saved document identity must be a valid workspace-relative path");
    }
    std::size_t start = 0;
    while (start <= workspaceRelativePath.size()) {
        const auto end = workspaceRelativePath.find_first_of("/\\", start);
        const auto component = workspaceRelativePath.substr(
            start, end == std::string_view::npos
                       ? std::string_view::npos
                       : end - start);
        if (component.empty() || component == "." || component == "..") {
            throw std::invalid_argument(
                "saved document identity contains an invalid path component");
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {DocumentKeyKind::Saved, std::string{workspaceRelativePath},
            UntitledDocumentId{{}}};
}

DocumentKey DocumentKey::untitled(UntitledDocumentId id) {
    return {DocumentKeyKind::Untitled, {}, id};
}

const std::string& DocumentKey::savedPath() const {
    if (kind_ != DocumentKeyKind::Saved) {
        throw std::logic_error("untitled document key has no saved path");
    }
    return path_;
}

UntitledDocumentId DocumentKey::untitledId() const {
    if (kind_ != DocumentKeyKind::Untitled) {
        throw std::logic_error("saved document key has no untitled ID");
    }
    return id_;
}

} // namespace ssg
