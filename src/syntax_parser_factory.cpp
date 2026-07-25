#include <ssg/EditorRuntime.h>

#include "TreeSitterParser.h"

namespace ssg {

std::shared_ptr<SyntaxParser> defaultSyntaxParser() {
    return std::make_shared<TreeSitterParser>();
}

}  // namespace ssg
