#pragma once

// The canonical chrome region shape, as ONE builder both the chrome decoder
// (ChromeDecode) and the built-in whole-screen synthesis (WholeScreenAssembly) call.
// A region is base>[base.left(Auto group), base.middle(Flex, optional center leaf),
// base.right(Auto group)]: the Auto end groups size to content and the Flex middle
// absorbs the slack, so left-flush/right-flush packing is encoded in SIZING, not
// positional convention. Sharing the builder means every header/footer region --
// composed OR built-in -- has the one shape lowerUiChromeRegion and the web
// interpreter read; there is not a second copy of "the chrome region shape" to drift.

#include <ssg/UiTree.h>    // UiNode
#include <ssg/UiWidget.h>  // WidgetDescriptor
#include <ssg/Widget.h>    // CenterWidth

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

// A group container (id `id`) holding `widgets` as Auto-sized leaves on a Row, with
// `gap` between items. The group itself is Auto-sized (content, never fills).
[[nodiscard]] UiNode chromeGroup(std::string id,
                                 const std::vector<WidgetDescriptor>& widgets,
                                 int gap);

// The canonical region rooted at `base`: [ left(Auto), middle(Flex, holding `center`
// at the given width policy when present), right(Auto) ]. `separator` is the left
// group's inter-item gap.
[[nodiscard]] UiNode chromeRegion(std::string_view base,
                                  const std::vector<WidgetDescriptor>& left,
                                  const std::vector<WidgetDescriptor>& right,
                                  const std::optional<WidgetDescriptor>& center,
                                  CenterWidth centerWidth, int centerFixed,
                                  int separator);

}  // namespace ssg
