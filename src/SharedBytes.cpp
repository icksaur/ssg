#include <ssg/SharedBytes.h>

#include <utility>

namespace ssg {
namespace {

class StringBacking final : public SharedBytes::Backing {
public:
    explicit StringBacking(std::string bytes) : bytes_(std::move(bytes)) {}
    [[nodiscard]] const char* data() const noexcept override {
        return bytes_.data();
    }
    [[nodiscard]] std::size_t size() const noexcept override {
        return bytes_.size();
    }

private:
    std::string bytes_;
};

}  // namespace

SharedBytes SharedBytes::owning(std::string bytes) {
    return SharedBytes{std::make_shared<const StringBacking>(std::move(bytes))};
}

}  // namespace ssg
