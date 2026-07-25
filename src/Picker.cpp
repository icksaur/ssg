#include <ssg/Picker.h>

#include <algorithm>

namespace ssg {

PickerDescriptor const* PickerCatalog::find(PickerKind kind) const noexcept {
    auto const found = std::find_if(
        kPickerDescriptors.begin(), kPickerDescriptors.end(),
        [&](PickerDescriptor const& descriptor) { return descriptor.kind == kind; });
    return found == kPickerDescriptors.end() ? nullptr : &*found;
}

PickerCatalog const& pickerCatalog() noexcept {
    static PickerCatalog const catalog{};
    return catalog;
}

}  // namespace ssg
