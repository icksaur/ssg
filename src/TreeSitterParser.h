#pragma once

#include <ssg/SyntaxModel.h>

namespace ssg {

class TreeSitterParser final : public SyntaxParser {
public:
    [[nodiscard]] bool hasGrammar(const LanguageId& language) const override;
    [[nodiscard]] SyntaxParseOutput parse(
        const SyntaxParseRequest& request) override;
};

} // namespace ssg
