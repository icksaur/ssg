#pragma once

// The UI-VM tree schema: the medium-agnostic superset structure the library
// publishes. A schema is ONE root node whose tree spans the screen. A node is
// either a CONTAINER (an axis, an inset, and children, laid out by the
// medium-agnostic constraints in LayoutConstraints.h) or a LEAF (a
// WidgetDescriptor). Every node carries a UiNodeId unique within the schema;
// patches, dynamic node state, triggers, and focus entries all address nodes by
// this id, so uniqueness is a precondition the validator enforces before any
// consumer walks the tree. Placement is a property of tree structure and
// well-known node ids, never an out-of-band region enum.
//
// A structural change is a full-tree replacement. Each node carries its own
// direct `visible` flag and, once resolved for publication, its `resolved`
// semantic leaf state -- there is no side record joined back to the tree by
// UiNodeId. The schema itself carries the authoritative `focusPath` alongside
// its root.

#include <ssg/LayoutConstraints.h>   // Axis, Size, Inset
#include <ssg/Theme.h>              // SemanticRole
#include <ssg/UiWidget.h>           // WidgetDescriptor
#include <ssg/focus.h>              // FocusTarget

#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ssg {

// A node's identity, unique within a schema. A strong type over the authored
// id string so a raw widget/layout id (which is not guaranteed unique) can never
// be passed where a node identity is required.
class UiNodeId {
public:
    UiNodeId() = default;
    explicit UiNodeId(std::string value) : value_{std::move(value)} {}

    [[nodiscard]] std::string const& value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

    auto operator<=>(UiNodeId const&) const = default;

private:
    std::string value_;
};

struct UiNode;

// A container arranges its children on an axis, after reserving its inset. `gap`
// is the generic spacing between adjacent children on the axis.
struct UiContainer {
    Axis axis = Axis::Column;
    Inset inset;
    Gap gap;
    std::vector<UiNode> children;
    // Whether this container is an independent scroll viewport (default None). A
    // client derives scroll behavior from this, never from hard-coded per-region
    // knowledge; the panel and content containers are the two viewports. Only a
    // container can be a viewport -- a leaf renders one widget and has no
    // independent scroll region -- so the property lives here, not on UiNode,
    // making a leaf viewport unrepresentable. Declared last so the {axis, inset,
    // gap, children} aggregate initializers stay valid.
    ScrollAxis scroll = ScrollAxis::None;

    friend bool operator==(const UiContainer&, const UiContainer&) = default;
};

// A leaf renders one widget.
struct UiLeaf {
    WidgetDescriptor widget;

    friend bool operator==(const UiLeaf&, const UiLeaf&) = default;
};

struct ResolvedUiNodeStyle {
    std::optional<SemanticRole> foreground;
    std::optional<SemanticRole> background;

    friend bool operator==(const ResolvedUiNodeStyle&,
                           const ResolvedUiNodeStyle&) = default;
};

// CONTRACT: UI nodes carry semantic role identities only. ThemeSnapshot remains
// the sole owner of concrete colors. Each channel resolves independently over
// inherited style, so clients consume rather than reproduce cascade semantics.
struct UiNodeStyle {
    std::optional<SemanticRole> foreground;
    std::optional<SemanticRole> background;

    [[nodiscard]] ResolvedUiNodeStyle resolve(
        ResolvedUiNodeStyle inherited) const noexcept {
        if (foreground) inherited.foreground = foreground;
        if (background) inherited.background = background;
        return inherited;
    }

    friend bool operator==(const UiNodeStyle&, const UiNodeStyle&) = default;
};

// The resolved semantic state of a leaf a client renders. `value` is the caption
// or field text; `label` the accessible label; `command` the click target;
// `checked` is present only for a checkbox; `role` is the effective SemanticRole
// the library resolved (the widget's own role, or the region's default), so a
// client colors the widget by a semantic role ordinal and never re-derives role
// names. `active` is present only for a stateful TextInput; its absence keeps
// client-local inputs entirely client-owned.
struct UiLeafState {
    std::string value;
    std::string label;
    std::optional<std::string> command;
    std::optional<bool> checked;
    SemanticRole role = SemanticRole::Text;
    std::optional<bool> active;

    friend bool operator==(const UiLeafState&, const UiLeafState&) = default;
};

// One node: an identity, a size within its parent, and either a container or a
// leaf. The variant makes "a node is exactly one of container/leaf" a type fact,
// not a pair of optionals that could both be set or both be empty.
struct UiNode {
    UiNodeId id;
    Size size;
    std::variant<UiContainer, UiLeaf> content;
    UiNodeStyle style;
    // CONTRACT: A focus-stack entry names a node with this declaration. Clients
    // derive keymap context from the stack endpoint rather than a parallel value.
    std::optional<FocusTarget> focusContext;
    // Optional accessible group label for a container whose children alone do
    // not identify the control group to assistive technology.
    std::optional<std::string> accessibleLabel;
    // This node's own direct visibility (NODE-VISIBILITY). A hidden container
    // suppresses every descendant regardless of the descendant's own flag; see
    // isUiNodeVisible for the ancestor-aware effective query. Set once by
    // interaction projection, never baked ahead of time.
    bool visible = true;
    // A renderable leaf's resolved semantic state, written directly at snapshot
    // publication (DIRECT-VALUE): consumers read it as the widget's already-
    // computed value and never resolve a string key themselves. Absent for a
    // container, a spacer, a client-local TextInput, and an empty-resolved
    // Label/Field.
    std::optional<UiLeafState> resolved;

    [[nodiscard]] bool isContainer() const noexcept {
        return std::holds_alternative<UiContainer>(content);
    }
    [[nodiscard]] bool isLeaf() const noexcept {
        return std::holds_alternative<UiLeaf>(content);
    }

    friend bool operator==(const UiNode&, const UiNode&) = default;
};

struct UiNodeActivationArguments {
    UiNodeId nodeId;

    friend bool operator==(const UiNodeActivationArguments&,
                           const UiNodeActivationArguments&) = default;
};

// The well-known node ids the screen tree is built from. Placement is a
// property of tree structure + these ids, not an out-of-band region enum: a client
// finds a well-known area by id. Header/footer are semantic UI subtrees; the root is
// their column parent.
inline constexpr std::string_view kRootNodeId = "root";
inline constexpr std::string_view kHeaderNodeId = "header";
inline constexpr std::string_view kFooterNodeId = "footer";
// The screen body and its two columns, and the view-leaf surfaces they
// hold. Placement is these ids plus tree structure; a client keys off an id. These
// name nodes the screen layout builds.
inline constexpr std::string_view kBodyNodeId = "body";
inline constexpr std::string_view kPanelNodeId = "panel";
inline constexpr std::string_view kContentNodeId = "content";
inline constexpr std::string_view kEditorNodeId = "editor";
inline constexpr std::string_view kDocumentViewportNodeId = "document.viewport";
inline constexpr std::string_view kFindResultsViewportNodeId = "findresults.viewport";
inline constexpr std::string_view kFileTreeNodeId = "filetree";
inline constexpr std::string_view kGitStatusNodeId = "gitstatus";
inline constexpr std::string_view kSymbolsNodeId = "symbols";
inline constexpr std::string_view kTreeNodeId = "tree";
inline constexpr std::string_view kTabBarNodeId = "tabbar";
inline constexpr std::string_view kDocumentNodeId = "document";
inline constexpr std::string_view kFindResultsNodeId = "findresults";
// The header's single-line prompt input (command palette, file finder, ...). An
// always-assembled TextInput leaf, trailing the header's status groups, hidden by
// presence unless a header-region prompt is open. The grid host derives the caret
// from its emitted geometry; a client owns the query prediction locally.
inline constexpr std::string_view kHeaderPromptInputNodeId = "input_line";
// CONTRACT: Footer prompt axis, grouping, order, and sizing live only in this
// request-derived container subtree. Clients lower these nodes directly and
// never reconstruct prompt layout from PromptKind.
inline constexpr std::string_view kFooterPromptNodeId = "footer.prompt";
inline constexpr std::string_view kFooterPromptOptionsNodeId =
    "footer.prompt.options";
[[nodiscard]] inline UiNodeId footerPromptControlNodeId(
    std::string_view controlId) {
    return UiNodeId{std::string{kFooterPromptNodeId} + ".control." +
                    std::string{controlId}};
}
// The fixed header/footer status-field leaf nodes, in their stable collapse
// order (StatusFields.h names the same fields' semantic ids). These are the
// single identities shared by screen layout and snapshot-publication
// population -- neither derives the other's node id mechanically.
inline constexpr std::string_view kHeaderPathFieldNodeId = "header.left.0";
inline constexpr std::string_view kHeaderBranchFieldNodeId = "header.left.1";
inline constexpr std::string_view kFooterStatusFieldNodeId = "footer.left.0";
inline constexpr std::string_view kFooterFollowFieldNodeId = "footer.left.1";
// The footer help hint node: the footer's right group's sole fixed member (its
// widget's own semantic id is "footer.hint"; this is the group-positional node
// id, matching the id `regionGroup` mechanically assigns it). Its label is the
// keymap-derived hint text and its command is the stable hint command, both
// written at snapshot publication.
inline constexpr std::string_view kFooterHintNodeId = "footer.right.0";
// The footer status-action anchor: a fixed container whose children mirror the
// selected status item's actions, rebuilt in tree order at assembly and given
// their resolved value/command/label at population.
inline constexpr std::string_view kFooterStatusActionsNodeId =
    "footer.status_actions";
// The draft-conflict notice's semantic surface: a content child after the tab bar
// and before the replaceable document/picker branches, hidden unless the active document has an
// unresolved draft conflict.
inline constexpr std::string_view kNoticeNodeId = "notice";

// The external-modification bar's presence-gated content child, immediately after
// the notice and before the replaceable document/picker branches.
inline constexpr std::string_view kExternalModNodeId = "externalmod";

// A well-formed empty root (id "root", an empty Column). A default-constructed
// UiNode has an empty id, which fails validation, so
// this is the default for UiSchema/UiComposition and the absent-UI schema.
[[nodiscard]] UiNode emptyUiRoot();

// A full schema: one root node whose tree spans the screen. Placement comes
// from tree structure and well-known node ids, never a region enum.
struct UiSchema {
    UiNode root = emptyUiRoot();
    // The authoritative base-to-top keyboard focus path. Empty until
    // interaction projection sets it; a published tree requires it non-empty
    // (see validatePublishedUiTree).
    std::vector<UiNodeId> focusPath;

    friend bool operator==(const UiSchema&, const UiSchema&) = default;
};

// CONTRACT: each channel resolves independently to the nearest
// ancestor-or-self assignment in the static schema. Presence never changes the
// cascade.
[[nodiscard]] std::optional<ResolvedUiNodeStyle> resolveUiNodeStyle(
    const UiSchema& schema, std::string_view nodeId);

// A root tree owned by the runtime, before publication sets its focus path. A
// UiComposition becomes a UiSchema when interaction projection assembles the
// focus path alongside it.
struct UiComposition {
    UiNode root = emptyUiRoot();

    friend bool operator==(const UiComposition&, const UiComposition&) = default;
};

struct UiSchemaValidation {
    // A path-qualified message on failure (e.g. `top/root/header: duplicate node
    // id "header"`); nullopt on success.
    std::optional<std::string> error;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Validate a schema before any consumer walks it. Enforces: every node id is
// non-empty and unique across the whole schema, and each leaf's per-kind shape
// (a View leaf names a valid surface and is Exact/Flex sized and carries no other
// field; a non-View leaf carries no surface). Fails loud with a path-qualified
// message. Pure.
[[nodiscard]] UiSchemaValidation validateUiSchema(const UiSchema& schema);

// The set of every node id in a schema. Meaningful only for a schema whose ids are
// unique.
[[nodiscard]] std::set<UiNodeId> uiSchemaNodeIds(const UiSchema& schema);
[[nodiscard]] const UiNode* findUiNode(const UiSchema& schema,
                                       const UiNodeId& id) noexcept;

// A node's effective visibility (NODE-VISIBILITY): true iff the node exists and it
// and every ancestor up to the root has its direct `visible` flag set. Replaces
// joining a side presence record by UiNodeId; every consumer that gated on
// presence -- activation, layout, hit testing, focus reconciliation -- routes
// through this single ancestor-aware traversal.
[[nodiscard]] bool isUiNodeVisible(const UiSchema& schema,
                                   const UiNodeId& id) noexcept;

// The keymap context of the published focus path's endpoint. The endpoint is
// guaranteed to declare a focusContext by validatePublishedUiTree.
[[nodiscard]] FocusTarget effectiveUiFocus(const UiSchema& schema) noexcept;

// Validate a schema for publication: validateUiSchema and NODE-FOCUS (a
// non-empty focus path naming declared focus hosts, starting at an editor or
// panel host, and ending at an effectively visible node). Pure.
[[nodiscard]] UiSchemaValidation validatePublishedUiTree(const UiSchema& schema);

// Require a published tree: validatePublishedUiTree, throwing
// std::invalid_argument on failure. This is the publication-boundary gate,
// without introducing a wrapper around the tree.
[[nodiscard]] UiSchema requirePublishedUiTree(UiSchema schema);

// Set exactly this node's direct visibility flag (NODE-VISIBILITY); does not
// touch descendants, which read ancestor visibility through isUiNodeVisible at
// query time. Throws std::invalid_argument if `id` is outside the schema.
void setUiNodeVisible(UiSchema& schema, const UiNodeId& id, bool visible);

}  // namespace ssg
