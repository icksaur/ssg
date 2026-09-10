#include <ssg/PieceTree.h>

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ssg::detail {

struct PieceTree::Node {
    bool addBuffer = false;
    std::size_t start = 0;
    std::size_t length = 0;
    std::size_t subtreeBytes = 0;
    int height = 1;
    NodePtr left;
    NodePtr right;
};

namespace {

using Node = PieceTree::Node;
using NodePtr = PieceTree::NodePtr;

std::size_t bytes(const NodePtr& node) noexcept {
    return node ? node->subtreeBytes : 0;
}

int height(const NodePtr& node) noexcept {
    return node ? node->height : 0;
}

void update(Node& node) noexcept {
    node.subtreeBytes = bytes(node.left) + node.length + bytes(node.right);
    node.height = 1 + std::max(height(node.left), height(node.right));
}

NodePtr rotateLeft(NodePtr root) noexcept {
    auto result = std::move(root->right);
    root->right = std::move(result->left);
    update(*root);
    result->left = std::move(root);
    update(*result);
    return result;
}

NodePtr rotateRight(NodePtr root) noexcept {
    auto result = std::move(root->left);
    root->left = std::move(result->right);
    update(*root);
    result->right = std::move(root);
    update(*result);
    return result;
}

NodePtr rebalance(NodePtr root) noexcept {
    update(*root);
    const auto balance = height(root->left) - height(root->right);
    if (balance > 1) {
        if (height(root->left->left) < height(root->left->right)) {
            root->left = rotateLeft(std::move(root->left));
        }
        return rotateRight(std::move(root));
    }
    if (balance < -1) {
        if (height(root->right->right) < height(root->right->left)) {
            root->right = rotateRight(std::move(root->right));
        }
        return rotateLeft(std::move(root));
    }
    return root;
}

NodePtr join(NodePtr left, NodePtr pivot, NodePtr right) noexcept {
    if (height(left) > height(right) + 1) {
        left->right = join(
            std::move(left->right), std::move(pivot), std::move(right));
        return rebalance(std::move(left));
    }
    if (height(right) > height(left) + 1) {
        right->left = join(
            std::move(left), std::move(pivot), std::move(right->left));
        return rebalance(std::move(right));
    }
    pivot->left = std::move(left);
    pivot->right = std::move(right);
    return rebalance(std::move(pivot));
}

std::pair<NodePtr, NodePtr> detachMin(NodePtr root) noexcept {
    if (!root->left) {
        auto remainder = std::move(root->right);
        root->right.reset();
        update(*root);
        return {std::move(root), std::move(remainder)};
    }
    auto [minimum, left_remainder] = detachMin(std::move(root->left));
    root->left = std::move(left_remainder);
    return {std::move(minimum), rebalance(std::move(root))};
}

NodePtr concatenate(NodePtr left, NodePtr right) noexcept {
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    auto [pivot, remainder] = detachMin(std::move(right));
    return join(std::move(left), std::move(pivot), std::move(remainder));
}

void appendText(
    const NodePtr& node,
    std::string_view original,
    const std::string& add,
    std::string& output) {
    if (!node) {
        return;
    }
    appendText(node->left, original, add, output);
    const auto& buffer = node->addBuffer ? add : original;
    output.append(buffer, node->start, node->length);
    appendText(node->right, original, add, output);
}

} // namespace

PieceTree::PieceTree(std::string original)
    : originalBuffer_(std::move(original)) {
    if (!originalBuffer_.empty()) {
        root_ = makeNode(false, 0, originalBuffer_.size());
    }
}

PieceTree::~PieceTree() = default;
PieceTree::PieceTree(PieceTree&&) noexcept = default;
PieceTree& PieceTree::operator=(PieceTree&&) noexcept = default;

std::size_t PieceTree::size() const noexcept {
    return bytes(root_);
}

std::string PieceTree::text() const {
    std::string result;
    result.reserve(size());
    appendText(root_, originalBuffer_, addBuffer_, result);
    return result;
}

void PieceTree::insert(std::size_t offset, std::string_view inserted) {
    if (offset > size()) {
        throw std::out_of_range("piece tree insert offset exceeds text size");
    }
    if (inserted.empty()) {
        return;
    }

    const auto start = addBuffer_.size();
    addBuffer_.append(inserted);
    auto insertedNode = makeNode(true, start, inserted.size());
    auto [left, right] = split(std::move(root_), offset);
    root_ = concatenate(
        concatenate(std::move(left), std::move(insertedNode)),
        std::move(right));
}

void PieceTree::erase(std::size_t offset, std::size_t count) {
    if (offset > size() || count > size() - offset) {
        throw std::out_of_range("piece tree erase range exceeds text size");
    }
    if (count == 0) {
        return;
    }

    auto [left, tail] = split(std::move(root_), offset);
    auto [discarded, right] = split(std::move(tail), count);
    root_ = concatenate(std::move(left), std::move(right));
}

PieceTree::NodePtr PieceTree::makeNode(
    bool addBuffer,
    std::size_t start,
    std::size_t length) const {
    auto node = std::make_unique<Node>();
    node->addBuffer = addBuffer;
    node->start = start;
    node->length = length;
    update(*node);
    return node;
}

std::pair<PieceTree::NodePtr, PieceTree::NodePtr> PieceTree::split(
    NodePtr root,
    std::size_t offset) const {
    if (!root) {
        return {};
    }

    const auto leftBytes = bytes(root->left);
    if (offset < leftBytes) {
        auto leftTree = std::move(root->left);
        auto rightTree = std::move(root->right);
        root->left.reset();
        root->right.reset();
        update(*root);
        auto [left, middle] = split(std::move(leftTree), offset);
        auto rootAndRight =
            concatenate(std::move(root), std::move(rightTree));
        return {
            std::move(left),
            concatenate(std::move(middle), std::move(rootAndRight)),
        };
    }

    const auto pieceEnd = leftBytes + root->length;
    if (offset > pieceEnd) {
        auto leftTree = std::move(root->left);
        auto rightTree = std::move(root->right);
        root->left.reset();
        root->right.reset();
        update(*root);
        auto [middle, right] =
            split(std::move(rightTree), offset - pieceEnd);
        auto leftAndRoot =
            concatenate(std::move(leftTree), std::move(root));
        return {
            concatenate(std::move(leftAndRoot), std::move(middle)),
            std::move(right),
        };
    }

    auto leftTree = std::move(root->left);
    auto rightTree = std::move(root->right);
    root->left.reset();
    root->right.reset();
    update(*root);
    if (offset == leftBytes) {
        return {
            std::move(leftTree),
            concatenate(std::move(root), std::move(rightTree)),
        };
    }
    if (offset == pieceEnd) {
        return {
            concatenate(std::move(leftTree), std::move(root)),
            std::move(rightTree),
        };
    }

    const auto pieceOffset = offset - leftBytes;
    auto leftPiece =
        makeNode(root->addBuffer, root->start, pieceOffset);
    auto rightPiece = makeNode(
        root->addBuffer,
        root->start + pieceOffset,
        root->length - pieceOffset);
    return {
        concatenate(std::move(leftTree), std::move(leftPiece)),
        concatenate(std::move(rightPiece), std::move(rightTree)),
    };
}

} // namespace ssg::detail
