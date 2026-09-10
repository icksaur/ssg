#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ssg {

class UntitledDocumentId {
public:
    explicit constexpr UntitledDocumentId(
        std::array<std::byte, 16> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] static UntitledDocumentId generate();
    [[nodiscard]] constexpr const std::array<std::byte, 16>& bytes()
        const noexcept {
        return bytes_;
    }

    friend bool operator==(const UntitledDocumentId&,
                           const UntitledDocumentId&) = default;

private:
    std::array<std::byte, 16> bytes_;
};

enum class DocumentKeyKind {
    Saved = 0,
    Untitled = 1,
};

class DocumentKey {
public:
    [[nodiscard]] static DocumentKey saved(
        std::string_view workspaceRelativePath);
    [[nodiscard]] static DocumentKey untitled(UntitledDocumentId id);

    [[nodiscard]] DocumentKeyKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& savedPath() const;
    [[nodiscard]] UntitledDocumentId untitledId() const;

    friend bool operator==(const DocumentKey&, const DocumentKey&) = default;

private:
    DocumentKey(DocumentKeyKind kind, std::string path, UntitledDocumentId id)
        : kind_(kind), path_(std::move(path)), id_(id) {}

    DocumentKeyKind kind_;
    std::string path_;
    UntitledDocumentId id_;
};

} // namespace ssg
