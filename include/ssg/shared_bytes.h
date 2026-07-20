#pragma once

// SharedBytes (Milestone 13, doc/spec-large-files-loading.md, LF-2): an immutable,
// shared, ref-counted handle over a contiguous byte buffer.
//
// The backing is abstract behind a `data()`/`size()` contract: today the only
// backing is an owned `std::string`, but the interface (not a bare
// `std::shared_ptr<const std::string>`) lets a future mmap-backed region slot in
// without re-plumbing consumers that index into `data()`. Copies are cheap (a
// shared_ptr bump) and the bytes never mutate, so several holders can share ONE
// buffer instead of each keeping a full copy.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace ssg {

class SharedBytes {
public:
    // Anything that owns a contiguous immutable byte range. `data()` must stay
    // valid and stable for the backing's lifetime.
    struct Backing {
        Backing() = default;
        virtual ~Backing() = default;
        Backing(const Backing&) = delete;
        Backing& operator=(const Backing&) = delete;
        [[nodiscard]] virtual const char* data() const noexcept = 0;
        [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    };

    SharedBytes() = default;
    explicit SharedBytes(std::shared_ptr<const Backing> backing) noexcept
        : backing_(std::move(backing)) {}

    // Mint from an owned string (the default backing); moves the bytes in, no copy.
    [[nodiscard]] static SharedBytes owning(std::string bytes);

    [[nodiscard]] const char* data() const noexcept {
        return backing_ ? backing_->data() : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return backing_ ? backing_->size() : 0;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] std::string_view view() const noexcept {
        return {data(), size()};
    }

private:
    std::shared_ptr<const Backing> backing_;
};

}  // namespace ssg
