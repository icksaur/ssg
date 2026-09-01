#pragma once

// Lowering a composed chrome region tree to solved items over a rect,
// reusing the shipped `WidgetStack`. The medium-agnostic UiRegion the runtime
// publishes is lowered to the SAME `(kind,id,label,rect,role,content,commandId)`
// nodes the built-in status-field projection emits, so a composed region is a
// transparent replacement.

#include <ssg/ShellState.h>  // Rect
#include <ssg/Style.h>
#include <ssg/UiNodeState.h>  // UiStateSection
#include <ssg/UiTree.h>      // UiRegion
#include <ssg/UiWidget.h>    // ChromeProviderResolver

#include <optional>
#include <string>
#include <vector>

namespace ssg {

// The result of lowering a medium-agnostic chrome region tree: on a malformed
// tree shape, a named error and no nodes emitted (fail-loud, never a plausible
// partial); otherwise the row's consumed right edge -- the absolute right edge
// (`rect.x + consumed width`) of the resolved row, INCLUDING space consumed by
// node-less `Spacer`s, so a caller placing content after the group (the header
// input line) advances past spacer cells, not merely past the last emitted node.
struct UiChromeLowerResult {
    std::optional<std::string> error;
    int rightEdge = 0;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

struct SolvedChromeItem {
    std::string id;
    std::string label;
    Rect rect;
    SemanticRole role = SemanticRole::Text;
    std::string content;
    std::optional<std::string> command;

    friend bool operator==(const SolvedChromeItem&,
                           const SolvedChromeItem&) = default;
};

struct SolvedChromeInput {
    UiNodeId nodeId;
    Rect query;
    std::string queryText;
    std::optional<Rect> ghost;
    std::string ghostText;
    Rect caret;

    friend bool operator==(const SolvedChromeInput&,
                           const SolvedChromeInput&) = default;
};

// CONTRACT: Header and footer use this same solved vocabulary. Footer surfaces
// never carry input; only the present header picker input may do so.
struct SolvedChromeSurface {
    Rect rect;
    std::vector<SolvedChromeItem> items;
    std::optional<SolvedChromeInput> input;

    friend bool operator==(const SolvedChromeSurface&,
                           const SolvedChromeSurface&) = default;
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

// Lower a medium-agnostic chrome region (the canonical tree the chrome decoder
// produces) directly into solved items over `rect`, reading the
// left/center/right groups, the separator (the left group's gap), and the center
// width policy (the center leaf's Size) from the tree itself. The tree must be the
// canonical chrome shape (a root container of exactly three group containers, plus
// the header prompt-input TextInput as a non-group sibling at any position); a
// malformed tree returns a named error and emits nothing. The prompt input is
// extracted by its well-known id (not by position), and when `input` is visible it
// is placed by the reserve/expand rule: its fixed reservation is subtracted from the
// groups' width first, then the input grows across the header's remaining width
// after them.
[[nodiscard]] UiChromeLowerResult lowerUiChromeRegion(
    const UiNode& regionRoot, const Rect& rect, SemanticRole defaultRole,
    const Style& style,
    const ChromeProviderResolver& resolveProvider,
    SolvedChromeSurface& out,
    const StatusViewState* statusView = nullptr,
    const PromptInputProjection* input = nullptr);

// Uses the generation-matched semantic node state as the value source while
// retaining grid-only display conversion at this presentation boundary.
[[nodiscard]] UiChromeLowerResult solveUiChromeRegion(
    const UiNode& regionRoot, const Rect& rect, SemanticRole defaultRole,
    const Style& style, Generation schemaGeneration,
    const UiStateSection& state,
    SolvedChromeSurface& out, const StatusViewState* statusView = nullptr,
    const PromptInputProjection* input = nullptr);

}  // namespace ssg
