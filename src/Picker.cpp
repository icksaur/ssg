#include <ssg/Picker.h>

#include <algorithm>

namespace ssg {

PickerDescriptor const* PickerCatalog::find(PickerKind kind) const noexcept {
    auto const found = std::find_if(
        descriptors_.begin(), descriptors_.end(),
        [&](PickerDescriptor const& descriptor) { return descriptor.kind == kind; });
    return found == descriptors_.end() ? nullptr : &*found;
}

PickerCatalog const& pickerCatalog() noexcept {
    static PickerCatalog const catalog{};
    return catalog;
}

}  // namespace ssg
