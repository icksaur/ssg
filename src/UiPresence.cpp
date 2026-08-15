#include <ssg/UiPresence.h>

#include <stdexcept>
#include <string>
#include <variant>

namespace ssg {

namespace {

void collect(const UiNode& node, const PresenceConfig& presence,
             std::vector<UiPresenceRecord>& out) {
    out.push_back(UiPresenceRecord{node.id, presence.isPresent(node.id)});
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children)
            collect(child, presence, out);
    }
}

}  // namespace

UiPresenceSection buildPresenceSection(const ValidatedSchema& schema,
                                       const PresenceConfig& presence) {
    if (presence.generation() != schema.generation()) {
        throw std::invalid_argument{
            "buildPresenceSection: PresenceConfig generation does not match the "
            "schema it is built against"};
    }
    UiPresenceSection section;
    section.generation = presence.generation();
    section.basis = presence.basis();
    collect(schema.schema().root, presence, section.nodes);
    return section;
}

UiPresenceSection defaultUiPresence() {
    // Derive from the same empty root the default schema uses, so a change to the
    // default tree cannot silently leave the default schema/presence pair mismatched.
    auto validated = ValidatedSchema::validate(UiSchema{}).takeSchema();
    return buildPresenceSection(validated, PresenceConfig::allPresent(validated));
}

}  // namespace ssg
