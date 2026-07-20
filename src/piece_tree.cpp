#include "piece_tree.h"

#include <ssg/open_metrics.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ssg::detail {

struct PieceTree::Node {
    bool add_buffer = false;
    std::size_t start = 0;
    std::size_t length = 0;
    std::size_t piece_newlines = 0;
    std::size_t subtree_bytes = 0;
    std::size_t subtree_newlines = 0;
    int height = 1;
    NodePtr left;
    NodePtr right;
};

namespace {

using Node = PieceTree::Node;
using NodePtr = PieceTree::NodePtr;

std::size_t bytes(const NodePtr& node) noexcept {
    return node ? node->subtree_bytes : 0;
}

std::size_t newlines(const NodePtr& node) noexcept {
    return node ? node->subtree_newlines : 0;
}

int height(const NodePtr& node) noexcept {
    return node ? node->height : 0;
}

void update(Node& node) noexcept {
    node.subtree_bytes = bytes(node.left) + node.length + bytes(node.right);
    node.subtree_newlines =
        newlines(node.left) + node.piece_newlines + newlines(node.right);
    node.height = 1 + std::max(height(node.left), height(node.right));
}

NodePtr rotate_left(NodePtr root) noexcept {
    auto result = std::move(root->right);
    root->right = std::move(result->left);
    update(*root);
    result->left = std::move(root);
    update(*result);
    return result;
}

NodePtr rotate_right(NodePtr root) noexcept {
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
            root->left = rotate_left(std::move(root->left));
        }
        return rotate_right(std::move(root));
    }
    if (balance < -1) {
        if (height(root->right->right) < height(root->right->left)) {
            root->right = rotate_right(std::move(root->right));
        }
        return rotate_left(std::move(root));
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

std::pair<NodePtr, NodePtr> detach_min(NodePtr root) noexcept {
    if (!root->left) {
        auto remainder = std::move(root->right);
        root->right.reset();
        update(*root);
        return {std::move(root), std::move(remainder)};
    }
    auto [minimum, left_remainder] = detach_min(std::move(root->left));
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
    auto [pivot, remainder] = detach_min(std::move(right));
    return join(std::move(left), std::move(pivot), std::move(remainder));
}

void append_text(
    const NodePtr& node,
    const std::string& original,
    const std::string& add,
    std::string& output) {
    if (!node) {
        return;
    }
    append_text(node->left, original, add, output);
    const auto& buffer = node->add_buffer ? add : original;
    output.append(buffer, node->start, node->length);
    append_text(node->right, original, add, output);
}

void append_range(
    const NodePtr& node,
    const std::string& original,
    const std::string& add,
    std::size_t offset,
    std::size_t count,
    std::string& output) {
    if (!node || count == 0) {
        return;
    }

    const auto left_bytes = bytes(node->left);
    if (offset < left_bytes) {
        const auto from_left = std::min(count, left_bytes - offset);
        append_range(node->left, original, add, offset, from_left, output);
        count -= from_left;
        offset = left_bytes;
    }
    if (count == 0) {
        return;
    }

    const auto piece_end = left_bytes + node->length;
    if (offset < piece_end) {
        const auto piece_offset = offset > left_bytes ? offset - left_bytes : 0;
        const auto from_piece = std::min(count, node->length - piece_offset);
        const auto& buffer = node->add_buffer ? add : original;
        output.append(buffer, node->start + piece_offset, from_piece);
        count -= from_piece;
        offset = piece_end;
    }
    if (count != 0) {
        append_range(
            node->right, original, add, offset - piece_end, count, output);
    }
}

std::size_t count_newlines_before(
    const NodePtr& node,
    const std::string& original,
    const std::string& add,
    std::size_t offset) noexcept {
    if (!node || offset == 0) {
        return 0;
    }

    const auto left_bytes = bytes(node->left);
    if (offset <= left_bytes) {
        return count_newlines_before(
            node->left, original, add, offset);
    }

    auto result = newlines(node->left);
    const auto in_piece = std::min(offset - left_bytes, node->length);
    const auto& buffer = node->add_buffer ? add : original;
    result += static_cast<std::size_t>(std::count(
        buffer.begin() + static_cast<std::ptrdiff_t>(node->start),
        buffer.begin() + static_cast<std::ptrdiff_t>(node->start + in_piece),
        '\n'));
    if (offset <= left_bytes + node->length) {
        return result;
    }
    return result + count_newlines_before(
        node->right, original, add, offset - left_bytes - node->length);
}

std::size_t nth_newline_offset(
    const NodePtr& node,
    const std::string& original,
    const std::string& add,
    std::size_t newline_index,
    std::size_t base) {
    const auto left_newlines = newlines(node->left);
    if (newline_index < left_newlines) {
        return nth_newline_offset(
            node->left, original, add, newline_index, base);
    }

    const auto left_bytes = bytes(node->left);
    newline_index -= left_newlines;
    if (newline_index < node->piece_newlines) {
        const auto& buffer = node->add_buffer ? add : original;
        auto begin = buffer.begin() + static_cast<std::ptrdiff_t>(node->start);
        const auto end = begin + static_cast<std::ptrdiff_t>(node->length);
        while (begin != end) {
            if (*begin == '\n' && newline_index-- == 0) {
                return base + left_bytes
                    + static_cast<std::size_t>(
                        begin - buffer.begin()
                        - static_cast<std::ptrdiff_t>(node->start));
            }
            ++begin;
        }
    }
    return nth_newline_offset(
        node->right,
        original,
        add,
        newline_index - node->piece_newlines,
        base + left_bytes + node->length);
}

struct Validation {
    bool valid = true;
    std::size_t bytes = 0;
    std::size_t newlines = 0;
    int height = 0;
};

Validation validate_node(
    const NodePtr& node,
    const std::string& original,
    const std::string& add) noexcept {
    if (!node) {
        return {};
    }
    const auto left = validate_node(node->left, original, add);
    const auto right = validate_node(node->right, original, add);
    const auto& buffer = node->add_buffer ? add : original;
    const auto range_valid =
        node->start <= buffer.size()
        && node->length <= buffer.size() - node->start;
    std::size_t piece_newlines = 0;
    if (range_valid) {
        piece_newlines = static_cast<std::size_t>(std::count(
            buffer.begin() + static_cast<std::ptrdiff_t>(node->start),
            buffer.begin()
                + static_cast<std::ptrdiff_t>(node->start + node->length),
            '\n'));
    }
    const auto expected_bytes = left.bytes + node->length + right.bytes;
    const auto expected_newlines =
        left.newlines + piece_newlines + right.newlines;
    const auto expected_height = 1 + std::max(left.height, right.height);
    const auto balanced = std::abs(left.height - right.height) <= 1;
    return {
        left.valid && right.valid && range_valid && node->length != 0
            && node->piece_newlines == piece_newlines
            && node->subtree_bytes == expected_bytes
            && node->subtree_newlines == expected_newlines
            && node->height == expected_height && balanced,
        expected_bytes,
        expected_newlines,
        expected_height,
    };
}

} // namespace

PieceTree::PieceTree(std::string_view original)
    : original_buffer_(original) {
    if (!original.empty()) {
        root_ = make_node(false, 0, original.size());
    }
}

PieceTree::PieceTree(std::string&& original)
    : original_buffer_(std::move(original)) {
    if (!original_buffer_.empty()) {
        root_ = make_node(false, 0, original_buffer_.size());
    }
}

PieceTree::~PieceTree() = default;
PieceTree::PieceTree(PieceTree&&) noexcept = default;
PieceTree& PieceTree::operator=(PieceTree&&) noexcept = default;

std::size_t PieceTree::size() const noexcept {
    return bytes(root_);
}

bool PieceTree::empty() const noexcept {
    return !root_;
}

std::string PieceTree::text() const {
    note_piece_tree_text();
    std::string result;
    result.reserve(size());
    append_text(root_, original_buffer_, add_buffer_, result);
    return result;
}

std::string PieceTree::substr(std::size_t offset, std::size_t count) const {
    if (offset > size() || count > size() - offset) {
        throw std::out_of_range("piece tree read range exceeds text size");
    }
    std::string result;
    result.reserve(count);
    append_range(
        root_, original_buffer_, add_buffer_, offset, count, result);
    return result;
}

void PieceTree::insert(std::size_t offset, std::string_view inserted) {
    if (offset > size()) {
        throw std::out_of_range("piece tree insert offset exceeds text size");
    }
    if (inserted.empty()) {
        return;
    }

    const auto start = add_buffer_.size();
    add_buffer_.append(inserted);
    auto inserted_node = make_node(true, start, inserted.size());
    auto [left, right] = split(std::move(root_), offset);
    root_ = concatenate(
        concatenate(std::move(left), std::move(inserted_node)),
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

std::size_t PieceTree::line_count() const noexcept {
    return newlines(root_) + 1;
}

std::size_t PieceTree::line_start(std::size_t line) const {
    if (line >= line_count()) {
        throw std::out_of_range("piece tree line exceeds line count");
    }
    if (line == 0) {
        return 0;
    }
    return nth_newline_offset(
        root_, original_buffer_, add_buffer_, line - 1, 0) + 1;
}

std::size_t PieceTree::line_of_offset(std::size_t offset) const {
    if (offset > size()) {
        throw std::out_of_range("piece tree line offset exceeds text size");
    }
    return count_newlines_before(
        root_, original_buffer_, add_buffer_, offset);
}

bool PieceTree::validate() const noexcept {
    return validate_node(root_, original_buffer_, add_buffer_).valid;
}

PieceTree::NodePtr PieceTree::make_node(
    bool add_buffer,
    std::size_t start,
    std::size_t length) const {
    auto node = std::make_unique<Node>();
    node->add_buffer = add_buffer;
    node->start = start;
    node->length = length;
    const auto source = piece_text(*node);
    node->piece_newlines =
        static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n'));
    update(*node);
    return node;
}

std::pair<PieceTree::NodePtr, PieceTree::NodePtr> PieceTree::split(
    NodePtr root,
    std::size_t offset) const {
    if (!root) {
        return {};
    }

    const auto left_bytes = bytes(root->left);
    if (offset < left_bytes) {
        auto left_tree = std::move(root->left);
        auto right_tree = std::move(root->right);
        root->left.reset();
        root->right.reset();
        update(*root);
        auto [left, middle] = split(std::move(left_tree), offset);
        auto root_and_right =
            concatenate(std::move(root), std::move(right_tree));
        return {
            std::move(left),
            concatenate(std::move(middle), std::move(root_and_right)),
        };
    }

    const auto piece_end = left_bytes + root->length;
    if (offset > piece_end) {
        auto left_tree = std::move(root->left);
        auto right_tree = std::move(root->right);
        root->left.reset();
        root->right.reset();
        update(*root);
        auto [middle, right] =
            split(std::move(right_tree), offset - piece_end);
        auto left_and_root =
            concatenate(std::move(left_tree), std::move(root));
        return {
            concatenate(std::move(left_and_root), std::move(middle)),
            std::move(right),
        };
    }

    auto left_tree = std::move(root->left);
    auto right_tree = std::move(root->right);
    root->left.reset();
    root->right.reset();
    update(*root);
    if (offset == left_bytes) {
        return {
            std::move(left_tree),
            concatenate(std::move(root), std::move(right_tree)),
        };
    }
    if (offset == piece_end) {
        return {
            concatenate(std::move(left_tree), std::move(root)),
            std::move(right_tree),
        };
    }

    const auto piece_offset = offset - left_bytes;
    auto left_piece =
        make_node(root->add_buffer, root->start, piece_offset);
    auto right_piece = make_node(
        root->add_buffer,
        root->start + piece_offset,
        root->length - piece_offset);
    return {
        concatenate(std::move(left_tree), std::move(left_piece)),
        concatenate(std::move(right_piece), std::move(right_tree)),
    };
}

std::string_view PieceTree::piece_text(const Node& node) const noexcept {
    const auto& buffer = node.add_buffer ? add_buffer_ : original_buffer_;
    return std::string_view(buffer).substr(node.start, node.length);
}

} // namespace ssg::detail
