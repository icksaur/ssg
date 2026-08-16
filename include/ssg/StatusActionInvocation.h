#pragma once

#include <cstdint>
#include <string>

namespace ssg {

class StatusId {
public:
    explicit constexpr StatusId(std::uint64_t value = 0) noexcept : value_(value) {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    constexpr auto operator<=>(const StatusId&) const = default;

private:
    std::uint64_t value_;
};

struct StatusActionInvocation {
    StatusId statusId;
    std::string actionId;
    std::uint64_t generation = 0;
    friend bool operator==(const StatusActionInvocation&,
                           const StatusActionInvocation&) = default;
};

}  // namespace ssg
