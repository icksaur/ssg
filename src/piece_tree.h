#pragma once

#include <ssg/shared_bytes.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ssg::detail {

class PieceTree final {
public:
    struct Node;
    using NodePtr = std::unique_ptr<Node>;

    explicit PieceTree(std::string_view original = {});
    explicit PieceTree(SharedBytes original);
    ~PieceTree();

    PieceTree(const PieceTree&) = delete;
    PieceTree& operator=(const PieceTree&) = delete;
    PieceTree(PieceTree&&) noexcept;
    PieceTree& operator=(PieceTree&&) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string substr(std::size_t offset, std::size_t count) const;

    void insert(std::size_t offset, std::string_view text);
    void erase(std::size_t offset, std::size_t count);

    [[nodiscard]] std::size_t lineCount() const noexcept;
    [[nodiscard]] std::size_t lineStart(std::size_t line) const;
    [[nodiscard]] std::size_t lineOfOffset(std::size_t offset) const;

    [[nodiscard]] bool validate() const noexcept;

private:
    [[nodiscard]] NodePtr makeNode(
        bool addBuffer,
        std::size_t start,
        std::size_t length) const;
    [[nodiscard]] std::pair<NodePtr, NodePtr> split(
        NodePtr root,
        std::size_t offset) const;

    [[nodiscard]] std::string_view pieceText(const Node& node) const noexcept;

    SharedBytes originalBuffer_;
    std::string addBuffer_;
    NodePtr root_;
};

} // namespace ssg::detail
