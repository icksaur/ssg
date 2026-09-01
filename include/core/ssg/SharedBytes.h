#pragma once

// SharedBytes: an immutable,
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
    // CONTRACT
    // SharedBytes::Backing: an implementation must return a pointer from data()
    //   that stays valid and unmoved for the whole lifetime of the backing,
    //   because holders index into it directly; a future relocating or lazily-
    //   faulting backing must preserve this or it is not a valid backing.
    // Anything that owns a contiguous immutable byte range.
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
