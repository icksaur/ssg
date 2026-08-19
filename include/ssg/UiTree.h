#pragma once

// The UI-VM tree schema: the medium-agnostic superset structure the library
// publishes once per generation. A schema is ONE root node whose tree spans the
// screen. A node is either a CONTAINER (an axis, an inset, and children, laid out
// by the medium-agnostic constraints in LayoutConstraints.h) or a LEAF (a
// WidgetDescriptor). Every node carries a UiNodeId unique within the schema's
// generation; patches, dynamic node state, triggers, and focus entries all address
// nodes by this id, so uniqueness is a precondition the validator enforces before
// any consumer walks the tree. Placement is a property of tree structure and
// well-known node ids, never an out-of-band region enum.
//
// The schema is immutable within a generation: a structural change is a full-tree
// replacement stamped with a new Generation. Values the tree DISPLAYS (resolved
// provider text, labels) and each node's present flag are NOT here -- they are
// generation-scoped dynamic state (a later sub-step). The schema carries value
// SOURCES (WidgetDescriptor's ValueSource), never resolved values, which is what
// keeps it immutable within a generation.

#include <ssg/LayoutConstraints.h>   // Axis, Size, Inset
#include <ssg/UiWidget.h>            // WidgetDescriptor, ValueSource

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

// Stamps a schema. A structural change bumps this; a mutation or dynamic-state
// delta names the generation it targets so a consumer on an old schema rejects
// rather than applying to the wrong tree.
struct Generation {
    explicit constexpr Generation(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(Generation const&) const noexcept = default;

private:
    std::uint64_t value_;
};

// A node's identity, unique within a generation. A strong type over the authored
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
// is the generic spacing between adjacent children on the axis (flexbox's gap):
// a grid client renders it as separator cells, a DOM client as a gap.
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

// One node: an identity, a size within its parent, and either a container or a
// leaf. The variant makes "a node is exactly one of container/leaf" a type fact,
// not a pair of optionals that could both be set or both be empty.
struct UiNode {
    UiNodeId id;
    Size size;
    std::variant<UiContainer, UiLeaf> content;

    [[nodiscard]] bool isContainer() const noexcept {
        return std::holds_alternative<UiContainer>(content);
    }
    [[nodiscard]] bool isLeaf() const noexcept {
        return std::holds_alternative<UiLeaf>(content);
    }

    friend bool operator==(const UiNode&, const UiNode&) = default;
};

// The well-known node ids the whole-screen tree is built from. Placement is a
// property of tree structure + these ids, not an out-of-band region enum: a client
// finds a well-known area by id. Header/footer are the chrome subtrees; the root is
// their column parent.
inline constexpr std::string_view kRootNodeId = "root";
inline constexpr std::string_view kHeaderNodeId = "header";
inline constexpr std::string_view kFooterNodeId = "footer";
// The whole-screen body and its two columns, and the view-leaf surfaces they
// hold. Placement is these ids plus tree structure; a client keys off an id. These
// name nodes the whole-screen assembly builds.
inline constexpr std::string_view kBodyNodeId = "body";
inline constexpr std::string_view kPanelNodeId = "panel";
inline constexpr std::string_view kContentNodeId = "content";
inline constexpr std::string_view kFileTreeNodeId = "filetree";
inline constexpr std::string_view kGitStatusNodeId = "gitstatus";
inline constexpr std::string_view kSymbolsNodeId = "symbols";
inline constexpr std::string_view kTabViewNodeId = "tabview";
inline constexpr std::string_view kFindResultsNodeId = "findresults";
// The header's single-line prompt input (command palette, file finder, ...). An
// always-assembled TextInput leaf, trailing the header's status groups, hidden by
// presence unless a header-region prompt is open. The grid host derives the caret
// from its emitted geometry; a client owns the query prediction locally.
inline constexpr std::string_view kHeaderPromptInputNodeId = "input_line";
// The footer-region prompt's semantic surface: a View leaf naming
// ViewSurface::FooterPrompt, placed between the body and the footer, hidden by
// presence unless a footer-region prompt (find/replace/goto/save-path/settings)
// is open. A native client renders and drives the prompt from the semantic
// PromptView section; the grid host ignores it and renders the parallel
// PresentationSnapshot::prompt with rects.
inline constexpr std::string_view kFooterPromptNodeId = "footer.prompt";
// The draft-conflict notice's semantic surface: a View leaf naming
// ViewSurface::Notice, placed between the header and the body (one reserved chrome
// row above the document, M15), hidden by presence unless the active document has an
// unresolved draft conflict. A native client renders the notice bar from the
// semantic NoticeView section; the grid host ignores it and renders the parallel
// ShellNotice with rects.
inline constexpr std::string_view kNoticeNodeId = "notice";

// The external-modification bar's presence-gated node, placed between the notice
// and the body, adjacent to the notice. In 5b-1 it is a bare presence-gated
// CONTAINER with no rendered View leaf: it exists so the transient external-focus
// capture has a node to anchor on and reconcile against (KeyboardFocus pops the
// capture when this node stops being present). Hidden by presence unless a file is
// externally changed, so no client renders anything for it yet and grid goldens
// stay byte-identical; its rendered View leaf is added in 5b-2.
inline constexpr std::string_view kExternalModNodeId = "externalmod";

// The typed well-known areas: a closed set a native client may key off to hand a
// subtree to its own toolkit. A raw id string is not a placement contract; this
// typed identity, plus the structural validation validateUiSchema performs for it
// (required node kind and ancestry), is.
enum class WellKnownArea : std::uint8_t {
    Root,
    Header,
    Body,
    Panel,
    Content,
    Footer,
    FooterPrompt,
    Notice,
    ExternalModification,
};

inline constexpr std::string_view wellKnownAreaId(WellKnownArea area) {
    switch (area) {
    case WellKnownArea::Root: return kRootNodeId;
    case WellKnownArea::Header: return kHeaderNodeId;
    case WellKnownArea::Body: return kBodyNodeId;
    case WellKnownArea::Panel: return kPanelNodeId;
    case WellKnownArea::Content: return kContentNodeId;
    case WellKnownArea::Footer: return kFooterNodeId;
    case WellKnownArea::FooterPrompt: return kFooterPromptNodeId;
    case WellKnownArea::Notice: return kNoticeNodeId;
    case WellKnownArea::ExternalModification: return kExternalModNodeId;
    }
    throw std::invalid_argument("wellKnownAreaId: unrecognized WellKnownArea");
}

// A well-formed empty root (id "root", an empty Column): the "no composed chrome"
// tree. A default-constructed UiNode has an empty id, which fails validation, so
// this is the default for UiSchema/UiComposition and the absent-UI schema.
[[nodiscard]] UiNode emptyUiRoot();

// A full schema for one generation: one root node whose tree spans the screen.
// Placement comes from tree structure and well-known node ids, never a region enum.
struct UiSchema {
    Generation generation{0};
    UiNode root = emptyUiRoot();

    friend bool operator==(const UiSchema&, const UiSchema&) = default;
};

// A generationless root tree: what the chrome decoder produces and the runtime OWNS
// as composed input. It carries no Generation because a generation belongs to one
// PUBLISHED schema; the runtime stamps the current generation when it publishes a
// composition as a UiSchema, so authorship (decode) never fixes a generation and the
// runtime stays the sole generation authority.
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
// non-empty and unique across the whole generation, and each leaf's per-kind shape
// (a View leaf names a valid surface and is Exact/Flex sized and carries no other
// field; a non-View leaf carries no surface). Fails loud with a path-qualified
// message. Pure.
[[nodiscard]] UiSchemaValidation validateUiSchema(const UiSchema& schema);

// Validate the whole-screen well-known-area contract: the schema root is the typed
// "root" area and the complete canonical topology is present with its required
// containers, View leaves, surfaces, parentage, and sibling order. Separate from
// validateUiSchema because the generic validator serves any tree the presence/focus
// machinery builds, while this contract binds a PUBLISHED whole-screen schema
// (enforced at the wire boundary). Pure.
[[nodiscard]] UiSchemaValidation validateWellKnownAreas(const UiSchema& schema);

// The set of every node id in a schema. Meaningful only for a schema whose ids are
// unique; used by ValidatedSchema.
[[nodiscard]] std::set<UiNodeId> uiSchemaNodeIds(const UiSchema& schema);

class ValidatedSchema;

struct ValidatedSchemaResult;

// A schema that has passed validateUiSchema. The only construction path is
// ValidatedSchema::validate, so a consumer that requires a ValidatedSchema (the
// mutation interpreter, the interaction state) cannot be handed a schema with
// duplicate or empty node ids -- the uniqueness the id-based rules depend on is a
// type fact at that boundary, not a runtime hope. It also caches the schema's node
// id set, so a capture or reference can be checked for schema membership.
class ValidatedSchema {
public:
    [[nodiscard]] static ValidatedSchemaResult validate(UiSchema schema);

    [[nodiscard]] const UiSchema& schema() const noexcept { return schema_; }
    [[nodiscard]] Generation generation() const noexcept {
        return schema_.generation;
    }
    [[nodiscard]] const std::set<UiNodeId>& nodeIds() const noexcept {
        return nodeIds_;
    }
    [[nodiscard]] bool contains(const UiNodeId& id) const {
        return nodeIds_.contains(id);
    }

private:
    explicit ValidatedSchema(UiSchema schema);

    UiSchema schema_;
    std::set<UiNodeId> nodeIds_;
};

// The outcome of validating a schema into a ValidatedSchema: exactly one of a
// validated schema or the path-qualified error, encoded as a variant so "both or
// neither" is unrepresentable. Defined after ValidatedSchema so the variant holds
// a complete type.
class ValidatedSchemaResult {
public:
    explicit ValidatedSchemaResult(ValidatedSchema schema)
        : value_{std::move(schema)} {}
    explicit ValidatedSchemaResult(std::string error)
        : value_{std::move(error)} {}

    [[nodiscard]] bool ok() const {
        return std::holds_alternative<ValidatedSchema>(value_);
    }
    [[nodiscard]] const ValidatedSchema& schema() const {
        return std::get<ValidatedSchema>(value_);
    }
    [[nodiscard]] ValidatedSchema takeSchema() {
        return std::get<ValidatedSchema>(std::move(value_));
    }
    [[nodiscard]] const std::string& error() const {
        return std::get<std::string>(value_);
    }

private:
    std::variant<ValidatedSchema, std::string> value_;
};

}  // namespace ssg
