#pragma once

// Projects a published header or footer UiNode subtree into terminal cells,
// reusing the grid's WidgetStack.

#include <ssg/Geometry.h>
#include <ssg/Style.h>
#include <ssg/UiTree.h>      // UiRegion
#include <ssg/UiWidget.h>    // WidgetProviderResolver

#include <optional>
#include <string>
#include <vector>

namespace ssg {

struct StatusViewState;

// The result of projecting a medium-agnostic UI region tree: on a malformed
// tree shape, a named error and no nodes emitted (fail-loud, never a plausible
// partial); otherwise the row's consumed right edge -- the absolute right edge
// (`rect.x + consumed width`) of the resolved row, INCLUDING space consumed by
// node-less `Spacer`s, so a caller placing content after the group (the header
// input line) advances past spacer cells, not merely past the last emitted node.
struct UiRegionProjectionResult {
    std::optional<std::string> error;
    int rightEdge = 0;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

struct SolvedUiItem {
    std::string id;
    UiNodeId nodeId;
    std::string label;
    Rect rect;
    SemanticRole role = SemanticRole::Text;
    std::string content;
    std::optional<std::string> command;

    friend bool operator==(const SolvedUiItem&, const SolvedUiItem&) = default;
};

struct SolvedUiInput {
    UiNodeId nodeId;
    Rect query;
    std::string queryText;
    std::optional<Rect> ghost;
    std::string ghostText;
    Rect caret;

    friend bool operator==(const SolvedUiInput&, const SolvedUiInput&) = default;
};

// CONTRACT: Header and footer use this same solved vocabulary. Footer surfaces
// never carry input; only the present header picker input may do so.
struct SolvedUiRegion {
    Rect rect;
    std::vector<SolvedUiItem> items;
    std::optional<SolvedUiInput> input;

    friend bool operator==(const SolvedUiRegion&,
                           const SolvedUiRegion&) = default;
};

// The header prompt input's grid-only projection: whether a picker is open on this
// client and, when it is, the query text and its ghost completion. The strings
// never enter the node tree (query prediction stays client-local); they are lowered
// to the input_line.query/.ghost nodes only when `visible`. A footer region passes
// no projection.
struct PromptInputProjection {
    bool visible = false;
    std::string query;
    std::string ghost;
};

// Project a medium-agnostic header/footer region directly into solved items
// over `rect`, reading the
// left/center/right groups, the separator (the left group's gap), and the center
// width policy (the center leaf's Size) from the tree itself. The tree must be the
// canonical UI-region shape (a root container of exactly three group containers, plus
// the header prompt-input TextInput as a non-group sibling at any position); a
// malformed tree returns a named error and emits nothing. The prompt input is
// extracted by its well-known id (not by position), and when `input` is visible it
// is placed by the reserve/expand rule: its fixed reservation is subtracted from the
// groups' width first, then the input grows across the header's remaining width
// after them.
[[nodiscard]] UiRegionProjectionResult projectUiRegion(
    const UiNode& regionRoot, const Rect& rect, SemanticRole defaultRole,
    const Style& style,
    const WidgetProviderResolver& resolveProvider,
    SolvedUiRegion& out,
    const StatusViewState* statusView = nullptr,
    const PromptInputProjection* input = nullptr);

// Uses the tree's own resolved leaf state as the value source while retaining
// grid-only display conversion at this presentation boundary.
[[nodiscard]] UiRegionProjectionResult solveUiRegion(
    const UiNode& regionRoot, const Rect& rect, SemanticRole defaultRole,
    const Style& style,
    SolvedUiRegion& out, const StatusViewState* statusView = nullptr,
    const PromptInputProjection* input = nullptr);

}  // namespace ssg
