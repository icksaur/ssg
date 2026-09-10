#pragma once

#include <ssg/SharedBytes.h>

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
    [[nodiscard]] std::string text() const;

    void insert(std::size_t offset, std::string_view text);
    void erase(std::size_t offset, std::size_t count);

private:
    [[nodiscard]] NodePtr makeNode(
        bool addBuffer,
        std::size_t start,
        std::size_t length) const;
    [[nodiscard]] std::pair<NodePtr, NodePtr> split(
        NodePtr root,
        std::size_t offset) const;

    SharedBytes originalBuffer_;
    std::string addBuffer_;
    NodePtr root_;
};

} // namespace ssg::detail
