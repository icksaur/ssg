#include <ssg/EditorRuntime.h>

#ifdef SSG_TREESITTER
#include "TreeSitterParser.h"
#endif

namespace ssg {

std::shared_ptr<SyntaxParser> defaultSyntaxParser() {
#ifdef SSG_TREESITTER
    return std::make_shared<TreeSitterParser>();
#else
    return nullptr;
#endif
}

}  // namespace ssg
