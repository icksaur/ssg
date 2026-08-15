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
    UiPresenceSection section;
    section.nodes.push_back(
        UiPresenceRecord{UiNodeId{std::string{kRootNodeId}}, true});
    return section;
}

}  // namespace ssg
