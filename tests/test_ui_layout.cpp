#include "ssg/ShellState.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ssg;

bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

void assertRect(Rect actual, Rect expected) {
    ASSERT_EQ(actual, expected);
}

ShellLayoutRequest request(int columns, int rows) {
    ShellLayoutRequest value;
    value.viewport = {columns, rows};
    value.headerFields = {
        {"active_command", "Active command", "INSERT", 0},
        {"current_path", "Current path", "src/main.cpp", 1},
        {"mode", "Editor mode", "edit", 2},
    };
    value.footerFields = {
        {"actionable_status", "Status message", "Saved", 0},
        {"follow_state", "Follow edits state and resume binding", "following", 1},
        {"background_activity", "Background activity", "idle", 2},
        {"encoding", "Text encoding", "UTF-8", 3},
        {"line_ending", "Line ending", "LF", 4},
        {"git_branch", "Git branch", "main", 5},
        {"git_repository", "Git repository", "ssg", 6},
        {"file_type", "File type", "C++", 7},
        {"file_size", "File size", "1 KiB", 8},
    };
    value.tabs = {{"main.cpp", "main.cpp tab", true}};
    value.footerActions = {{"status.retry", "Retry status action"}};
    value.panelProviderLabel = "Files";
    return value;
}

TEST(handAuthoredGeometryGoldens) {
    ShellState state;

    auto minimum = request(20, 4);
    state.togglePanel();
    auto minimumResult = computeShellLayout(minimum, state);
    ASSERT_TRUE(minimumResult.accepted());
    ASSERT_FALSE(minimumResult.view->panel.has_value());
    assertRect(*minimumResult.view->header, {0, 0, 20, 1});
    assertRect(*minimumResult.view->tabBar, {0, 1, 20, 1});
    assertRect(minimumResult.view->panes[0].content, {0, 2, 19, 1});
    assertRect(minimumResult.view->panes[0].scrollbar, {19, 2, 1, 1});
    assertRect(*minimumResult.view->footer, {0, 3, 20, 1});
    const auto hasHeaderField = [&](std::string_view id) {
        return std::ranges::any_of(
            minimumResult.view->accessibilityNodes, [&](const auto& node) {
                return node.kind == ShellNodeKind::HeaderField &&
                       node.id == id;
            });
    };
    ASSERT_TRUE(hasHeaderField("active_command"));
    ASSERT_FALSE(hasHeaderField("current_path"));
    ASSERT_FALSE(hasHeaderField("mode"));

    auto wide = request(80, 12);
    wide.reservedPromptRows = 2;
    auto wideResult = computeShellLayout(wide, state);
    ASSERT_TRUE(wideResult.accepted());
    assertRect(*wideResult.view->header, {0, 0, 80, 1});
    assertRect(*wideResult.view->panel, {0, 1, 24, 10});
    assertRect(*wideResult.view->tabBar, {24, 1, 56, 1});
    // The prompt reserves the bottom rows FULL WIDTH (the footer-region width,
    // not the editor width), which is the single source both the shell and the
    // prompt-status view consume, and where the controls actually render
    assertRect(*wideResult.view->prompt, {0, 10, 80, 2});
    // The pane content stays anchored at the top (y=2, directly below the tab
    // bar) and only loses height -- opening a prompt must never push it down.
    assertRect(wideResult.view->panes[0].content, {24, 2, 55, 8});
    assertRect(wideResult.view->panes[0].scrollbar, {79, 2, 1, 8});
    assertRect(*wideResult.view->footer, {0, 11, 80, 1});

    // Regression proof: the pane's top does not move between no-prompt and
    // prompt-open layouts -- the footer/prompt only take rows from the bottom.
    auto noPrompt = request(80, 12);
    auto noPromptResult = computeShellLayout(noPrompt, state);
    ASSERT_TRUE(noPromptResult.accepted());
    ASSERT_EQ(noPromptResult.view->panes[0].content.y,
              wideResult.view->panes[0].content.y);

    auto focused = request(20, 4);
    state.toggleDistractionFree();
    auto focusedResult = computeShellLayout(focused, state);
    ASSERT_TRUE(focusedResult.accepted());
    ASSERT_FALSE(focusedResult.view->header.has_value());
    ASSERT_FALSE(focusedResult.view->footer.has_value());
    ASSERT_FALSE(focusedResult.view->tabBar.has_value());
    assertRect(focusedResult.view->panes[0].content, {0, 0, 19, 4});
    assertRect(focusedResult.view->panes[0].scrollbar, {19, 0, 1, 4});
}

TEST(viewportAndPromptErrorsAreTyped) {
    ShellState state;
    auto narrow = computeShellLayout(request(19, 4), state);
    ASSERT_FALSE(narrow.accepted());
    ASSERT_EQ(narrow.error->code, ShellLayoutErrorCode::ViewportTooSmall);
    ASSERT_FALSE(narrow.view.has_value());

    auto shortView = computeShellLayout(request(20, 3), state);
    ASSERT_FALSE(shortView.accepted());
    ASSERT_EQ(shortView.error->code, ShellLayoutErrorCode::ViewportTooSmall);

    auto invalidPrompt = request(80, 12);
    invalidPrompt.reservedPromptRows = 4;
    auto invalid = computeShellLayout(invalidPrompt, state);
    ASSERT_FALSE(invalid.accepted());
    ASSERT_EQ(invalid.error->code, ShellLayoutErrorCode::InvalidPromptRows);

    auto noRoom = request(20, 4);
    noRoom.reservedPromptRows = 2;
    auto noRoomResult = computeShellLayout(noRoom, state);
    ASSERT_FALSE(noRoomResult.accepted());
    ASSERT_EQ(noRoomResult.error->code, ShellLayoutErrorCode::ViewportTooSmall);
}

TEST(paneCommandsPreserveTopologyAndOrder) {
    ShellState state;
    const auto first = state.activePane();
    const auto second = state.splitActive(SplitAxis::Vertical);
    const auto third = state.splitActive(SplitAxis::Horizontal);
    ASSERT_EQ(state.paneCount(), std::size_t{3});
    ASSERT_EQ(state.activePane(), third);

    state.previousPane();
    ASSERT_EQ(state.activePane(), second);
    state.previousPane();
    ASSERT_EQ(state.activePane(), first);
    state.nextPane();
    ASSERT_EQ(state.activePane(), second);

    auto view = computeShellLayout(request(80, 20), state);
    ASSERT_TRUE(view.accepted());
    ASSERT_EQ(view.view->panes.size(), std::size_t{3});
    ASSERT_TRUE(state.focusPane(PaneDirection::Down, *view.view));
    ASSERT_NE(state.activePane(), first);
    ASSERT_TRUE(state.closeActivePane());
    ASSERT_EQ(state.paneCount(), std::size_t{2});
    ASSERT_TRUE(state.closeActivePane());
    ASSERT_FALSE(state.closeActivePane());
}

TEST(panelCommandsPreserveProviderStateWhenHidden) {
    ShellState state({"Files", "Git", "Symbols"});
    ASSERT_FALSE(state.panelRequested());
    state.togglePanel();
    ASSERT_TRUE(state.panelRequested());
    ASSERT_TRUE(state.focusPanel());
    ASSERT_EQ(state.activePanelProvider(), std::string_view{"Files"});
    state.nextPanelProvider();
    ASSERT_EQ(state.activePanelProvider(), std::string_view{"Git"});
    state.previousPanelProvider();
    ASSERT_EQ(state.activePanelProvider(), std::string_view{"Files"});
    state.togglePanel();
    ASSERT_FALSE(state.panelRequested());
    ASSERT_FALSE(state.panelFocused());
    ASSERT_EQ(state.activePanelProvider(), std::string_view{"Files"});
    state.togglePanel();
    ASSERT_TRUE(state.focusPanel());
    state.toggleDistractionFree();
    state.toggleDistractionFree();
    ASSERT_TRUE(state.panelFocused());
}


TEST(accessibilityNodesHaveLabelsAndRoles) {
    ShellState state({"Files"});
    auto input = request(80, 12);
    state.togglePanel();
    input.reservedPromptRows = 1;
    input.emptyState = true;
    auto result = computeShellLayout(input, state);
    ASSERT_TRUE(result.accepted());

    const auto& nodes = result.view->accessibilityNodes;
    ASSERT_FALSE(nodes.empty());
    for (const auto& node : nodes) {
        ASSERT_FALSE(node.label.empty());
        ASSERT_TRUE(node.rect.width > 0);
        ASSERT_TRUE(node.rect.height > 0);
    }
    const auto hasKind = [&](ShellNodeKind kind) {
        return std::ranges::any_of(nodes, [=](const auto& node) {
            return node.kind == kind;
        });
    };
    ASSERT_TRUE(hasKind(ShellNodeKind::Header));
    ASSERT_TRUE(hasKind(ShellNodeKind::Footer));
    ASSERT_TRUE(hasKind(ShellNodeKind::FooterAction));
    ASSERT_TRUE(hasKind(ShellNodeKind::Tab));
    ASSERT_TRUE(hasKind(ShellNodeKind::PanelProvider));
    ASSERT_TRUE(hasKind(ShellNodeKind::Pane));
    ASSERT_TRUE(hasKind(ShellNodeKind::Scrollbar));
    ASSERT_TRUE(hasKind(ShellNodeKind::PromptReservation));
    ASSERT_TRUE(hasKind(ShellNodeKind::EmptyState));
    for (const auto& field : nodes) {
        if (field.kind != ShellNodeKind::FooterField) continue;
        for (const auto& action : nodes) {
            if (action.kind == ShellNodeKind::FooterAction) {
                ASSERT_FALSE(overlaps(field.rect, action.rect));
            }
        }
    }
}

TEST(nonOverlapAndCardinalityProperties) {
    for (int columns = 20; columns <= 100; ++columns) {
        for (int rows = 4; rows <= 20; ++rows) {
            ShellState state;
            state.splitActive(SplitAxis::Vertical);
            state.splitActive(SplitAxis::Horizontal);
            state.togglePanel();
            auto input = request(columns, rows);
            auto result = computeShellLayout(input, state);
            ASSERT_TRUE(result.accepted());
            const auto& view = *result.view;
            ASSERT_TRUE(view.panes.size() == 1 || view.panes.size() == 3);
            ASSERT_EQ(view.scrollbarCount(), view.panes.size());

            std::vector<Rect> leaves;
            leaves.push_back(*view.header);
            leaves.push_back(*view.footer);
            leaves.push_back(*view.tabBar);
            if (view.panel) leaves.push_back(*view.panel);
            for (const auto& pane : view.panes) {
                leaves.push_back(pane.content);
                leaves.push_back(pane.scrollbar);
            }
            for (std::size_t i = 0; i < leaves.size(); ++i) {
                ASSERT_TRUE(leaves[i].x >= 0 && leaves[i].y >= 0);
                ASSERT_TRUE(leaves[i].right() <= columns);
                ASSERT_TRUE(leaves[i].bottom() <= rows);
                for (std::size_t j = i + 1; j < leaves.size(); ++j) {
                    ASSERT_FALSE(overlaps(leaves[i], leaves[j]));
                }
            }
        }
    }
}

TEST(statusFieldManifestHasExactOrderAndLabels) {
    std::ifstream input(std::string{SSG_SOURCE_DIR} + "/data/ui/status_fields.json");
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::array ids{
        "path", "branch", "status", "follow",
    };
    std::size_t position = 0;
    for (const auto* id : ids) {
        const auto found = json.find(std::string{"\"id\":\""} + id + '"', position);
        ASSERT_NE(found, std::string::npos);
        position = found + 1;
    }
    ASSERT_EQ(std::count(json.begin(), json.end(), '{'), ids.size());
    ASSERT_EQ(json.find("\"accessible_label\":\"\""), std::string::npos);
    ASSERT_EQ(json.find("\"id\":\"git_branch\""), std::string::npos);
    ASSERT_EQ(json.find("\"id\":\"git_repository\""), std::string::npos);
}

} // namespace

TEST(accessibilityLeafNodesCarryDisplayContent) {
    ShellState state;
    state.togglePanel();
    auto result = computeShellLayout(request(80, 12), state);
    ASSERT_TRUE(result.accepted());
    const auto find = [&](ShellNodeKind kind,
                          std::string_view id) -> const AccessibilityNode* {
        for (const auto& node : result.view->accessibilityNodes) {
            if (node.kind == kind && node.id == id) return &node;
        }
        return nullptr;
    };
    const auto* command = find(ShellNodeKind::HeaderField, "active_command");
    ASSERT_TRUE(command != nullptr);
    if (command) ASSERT_EQ(command->content, std::string{"INSERT"});
    const auto* tab = find(ShellNodeKind::Tab, "tab.0");
    ASSERT_TRUE(tab != nullptr);
    if (tab) ASSERT_EQ(tab->content, std::string{"main.cpp"});
    const auto* provider = find(ShellNodeKind::PanelProvider, "panel.provider");
    ASSERT_TRUE(provider != nullptr);
    if (provider) ASSERT_EQ(provider->content, std::string{"Files"});
    // Container nodes carry no display text.
    const auto* header = find(ShellNodeKind::Header, "header");
    ASSERT_TRUE(header != nullptr);
    if (header) ASSERT_TRUE(header->content.empty());
}

TEST(dirtyTabContentShowsMarker) {
    auto value = request(80, 12);
    value.tabs = {{"a.cpp", "a.cpp tab", true, true}};
    ShellState state;
    auto result = computeShellLayout(value, state);
    ASSERT_TRUE(result.accepted());
    const AccessibilityNode* tab = nullptr;
    for (const auto& node : result.view->accessibilityNodes) {
        if (node.kind == ShellNodeKind::Tab) tab = &node;
    }
    ASSERT_TRUE(tab != nullptr);
    if (tab) ASSERT_EQ(tab->content, std::string{"a.cpp *"});
}

TEST(focusTransitionsFollowTheNavigationTable) {
    ShellState state{{"filesystem"}};
    ASSERT_TRUE(state.focus() == FocusTarget::Editor);
    // The panel cannot be focused while hidden.
    ASSERT_FALSE(state.focusPanel());
    ASSERT_TRUE(state.focus() == FocusTarget::Editor);
    // Showing the panel focuses it (no explicit focus_panel needed).
    state.togglePanel();  // show
    ASSERT_TRUE(state.focus() == FocusTarget::Panel);
    ASSERT_TRUE(state.panelFocused());
    // A prompt pushes the current focus and restores it on close.
    state.enterPromptFocus();
    ASSERT_TRUE(state.focus() == FocusTarget::Prompt);
    state.exitPromptFocus();
    ASSERT_TRUE(state.focus() == FocusTarget::Panel);
    // Hiding the focused panel restores the focus present when it was shown (the
    // editor here).
    state.togglePanel();  // hide
    ASSERT_TRUE(state.focus() == FocusTarget::Editor);
    // A prompt over a panel that is hidden before close restores to editor.
    state.togglePanel();  // show (focuses the panel)
    ASSERT_TRUE(state.focus() == FocusTarget::Panel);
    state.enterPromptFocus();
    state.togglePanel();  // hide the panel while the prompt is focused
    state.exitPromptFocus();
    ASSERT_TRUE(state.focus() == FocusTarget::Editor);
}

TEST(hidingAnUnfocusedPanelLeavesFocusUntouched) {
    ShellState state{{"filesystem"}};
    // Show (focuses the panel), then move focus to the editor while the panel is
    // still shown; hiding it must NOT yank focus (it isn't the focused surface).
    state.togglePanel();  // show -> panel focused
    ASSERT_TRUE(state.focus() == FocusTarget::Panel);
    state.focusEditor();
    ASSERT_TRUE(state.focus() == FocusTarget::Editor);
    state.togglePanel();  // hide while editor-focused
    ASSERT_TRUE(state.focus() == FocusTarget::Editor);
    // Re-showing focuses the panel again; hiding restores the editor.
    state.togglePanel();  // show
    ASSERT_TRUE(state.focus() == FocusTarget::Panel);
    state.togglePanel();  // hide
    ASSERT_TRUE(state.focus() == FocusTarget::Editor);
}


// ---------------------------------------------------------------------------
// The header input line.

const AccessibilityNode* findNode(const ShellViewState& view, std::string_view id) {
    for (const auto& node : view.accessibilityNodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

// The reason this change exists: typing into a picker must not move the status
// fields.  Before the fix the query was laid out first and pushed them right,
// so this fails against the old order.
// A picker (palette / file find) is not a document, so it covers the tab bar:
// with a picker open there is no tab bar node, no tab hits, and the document
// content reclaims the tab bar's row (removing the confusing visible tabs and
// the one-row gap between the input line and the results).
TEST(anOpenPickerHidesTheTabBarAndReclaimsItsRow) {
    auto closed = request(80, 24);
    closed.inputLineActive = false;
    ShellState closedState;
    auto closedResult = computeShellLayout(closed, closedState);
    ASSERT_TRUE(closedResult.accepted());
    if (!closedResult.accepted()) return;
    ASSERT_TRUE(closedResult.view->tabBar.has_value());
    ASSERT_TRUE(findNode(*closedResult.view, "tabs") != nullptr);
    ASSERT_FALSE(closedResult.view->tabHits.empty());
    ASSERT_FALSE(closedResult.view->panes.empty());
    int const contentTopWithTabs = closedResult.view->panes.front().content.y;

    auto open = request(80, 24);
    open.inputLineActive = true;
    ShellState openState;
    auto openResult = computeShellLayout(open, openState);
    ASSERT_TRUE(openResult.accepted());
    if (!openResult.accepted()) return;
    ASSERT_FALSE(openResult.view->tabBar.has_value());
    ASSERT_TRUE(findNode(*openResult.view, "tabs") == nullptr);
    ASSERT_TRUE(findNode(*openResult.view, "tab.0") == nullptr);
    ASSERT_TRUE(openResult.view->tabHits.empty());
    ASSERT_FALSE(openResult.view->panes.empty());
    if (!openResult.view->panes.empty()) {
        ASSERT_EQ(openResult.view->panes.front().content.y,
                  contentTopWithTabs - 1);
        ASSERT_EQ(openResult.view->panes.front().content.y,
                  openResult.view->header->y + openResult.view->header->height);
    }
}

TEST(typingInTheInputLineNeverMovesTheStatusFields) {
    std::optional<Rect> firstPathRect;
    std::optional<Rect> firstQueryRect;
    for (const auto& query : {std::string{}, std::string{"a"},
                              std::string{"abcdefgh"},
                              std::string(40, 'x')}) {
        auto value = request(120, 12);
        value.inputLineActive = true;
        value.inputLineQuery = query;
        ShellState state;
        auto result = computeShellLayout(value, state);
        ASSERT_TRUE(result.accepted());
        if (!result.accepted()) continue;

        const auto* path = findNode(*result.view, "current_path");
        const auto* command = findNode(*result.view, "active_command");
        const auto* line = findNode(*result.view, "input_line.query");
        ASSERT_TRUE(path != nullptr);
        ASSERT_TRUE(command != nullptr);
        ASSERT_TRUE(line != nullptr);
        if (!path || !command || !line) continue;

        // Every field keeps its exact rectangle regardless of query length...
        if (!firstPathRect) firstPathRect = path->rect;
        ASSERT_EQ(path->rect, *firstPathRect);
        // ...and the input line's own start column is stable too, so the text
        // does not slide under the user as they type.
        if (!firstQueryRect) firstQueryRect = line->rect;
        ASSERT_EQ(line->rect.x, firstQueryRect->x);
        // The input line sits AFTER the fields, which is the requested order.
        ASSERT_TRUE(line->rect.x > path->rect.x);
    }
}

// A header field whose value contains a wide/multibyte glyph (as a configured
// cwd_prefix produces) must be sized in DISPLAY CELLS, not bytes: a CJK glyph is
// 3 bytes but 2 cells, so a byte-width would oversize the node rect (and its
// click region) and drop later fields too early.
TEST(headerFieldWidthIsMeasuredInCellsNotBytes) {
    auto asciiReq = request(120, 12);
    asciiReq.headerFields = {{"current_path", "Current path", "xx", 1}};
    ShellState asciiState;
    auto asciiResult = computeShellLayout(asciiReq, asciiState);
    ASSERT_TRUE(asciiResult.accepted());
    if (!asciiResult.accepted()) return;
    const auto* asciiPath = findNode(*asciiResult.view, "current_path");
    ASSERT_TRUE(asciiPath != nullptr);

    // "\xE4\xB8\xAD" (U+4E2D) is a fullwidth CJK glyph: 3 bytes, 2 cells -- the
    // same visible width as "xx" but a different byte count.
    auto wideReq = request(120, 12);
    wideReq.headerFields = {
        {"current_path", "Current path", "\xE4\xB8\xAD", 1}};
    ShellState wideState;
    auto wideResult = computeShellLayout(wideReq, wideState);
    ASSERT_TRUE(wideResult.accepted());
    if (!wideResult.accepted()) return;
    const auto* widePath = findNode(*wideResult.view, "current_path");
    ASSERT_TRUE(widePath != nullptr);

    // Equal cell width -> equal node rect width (a byte measure would make the
    // wide one one column wider: 3 bytes vs 2).
    if (asciiPath && widePath) {
        ASSERT_EQ(widePath->rect.width, asciiPath->rect.width);
    }
}
// The narrow case is where a query-length-dependent reservation would betray
// the whole point: if the reservation grows with the query, the fields shrink
// as the user types and start collapsing again. Assert stability at a width
// where the reservation actually binds, not just at a roomy one.
TEST(fieldsAreStableEvenWhenTheHeaderIsTight) {
    std::optional<Rect> firstPathRect;
    std::optional<std::size_t> firstFieldCount;
    for (const auto& query : {std::string{}, std::string{"ab"},
                              std::string{"abcdefghij"},
                              std::string(30, 'z')}) {
        auto value = request(48, 12);
        value.inputLineActive = true;
        value.inputLineQuery = query;
        ShellState state;
        auto result = computeShellLayout(value, state);
        ASSERT_TRUE(result.accepted());
        if (!result.accepted()) continue;

        std::size_t fieldCount = 0;
        for (const auto& node : result.view->accessibilityNodes) {
            if (node.kind == ShellNodeKind::HeaderField &&
                node.id.rfind("input_line", 0) != 0) {
                ++fieldCount;
            }
        }
        if (!firstFieldCount) firstFieldCount = fieldCount;
        // The same fields survive, in the same places, at every query length.
        ASSERT_EQ(fieldCount, *firstFieldCount);
        const auto* path = findNode(*result.view, "active_command");
        ASSERT_TRUE(path != nullptr);
        if (path) {
            if (!firstPathRect) firstPathRect = path->rect;
            ASSERT_EQ(path->rect, *firstPathRect);
        }
    }
}

// Header nodes must not overlap: hit-testing returns the FIRST node containing
// a cell, so an input line drawn on top of a field would render the query but
// dispatch the field's command on click.
TEST(headerNodesNeverOverlap) {
    for (int columns : {30, 48, 80, 120}) {
        auto value = request(columns, 12);
        value.inputLineActive = true;
        value.inputLineQuery = std::string(20, 'q');
        value.inputLineGhost = "ghost";
        ShellState state;
        auto result = computeShellLayout(value, state);
        ASSERT_TRUE(result.accepted());
        if (!result.accepted()) continue;
        std::vector<const AccessibilityNode*> header;
        for (const auto& node : result.view->accessibilityNodes) {
            if (node.kind == ShellNodeKind::HeaderField) header.push_back(&node);
        }
        for (std::size_t i = 0; i < header.size(); ++i) {
            for (std::size_t j = i + 1; j < header.size(); ++j) {
                const auto& a = header[i]->rect;
                const auto& b = header[j]->rect;
                const bool disjoint =
                    a.x + a.width <= b.x || b.x + b.width <= a.x;
                ASSERT_TRUE(disjoint);
            }
        }
    }
}

// An over-long query must keep its END visible rather than clipping it, and it
// must NOT take space back from the fields to do so.
TEST(anOverlongQueryScrollsItsOwnTextAndLeavesFieldsAlone) {
    auto shortQuery = request(60, 12);
    shortQuery.inputLineActive = true;
    shortQuery.inputLineQuery = "ab";
    auto longQuery = request(60, 12);
    longQuery.inputLineActive = true;
    // Distinct tail so the visible window is identifiable.
    longQuery.inputLineQuery = std::string(200, 'x') + "TAIL";

    ShellState state;
    auto shortResult = computeShellLayout(shortQuery, state);
    auto longResult = computeShellLayout(longQuery, state);
    ASSERT_TRUE(shortResult.accepted() && longResult.accepted());
    if (!shortResult.accepted() || !longResult.accepted()) return;

    const auto* shortPath = findNode(*shortResult.view, "active_command");
    const auto* longPath = findNode(*longResult.view, "active_command");
    ASSERT_TRUE(shortPath != nullptr && longPath != nullptr);
    // The fields did not move or shrink to make room.
    if (shortPath && longPath) ASSERT_EQ(shortPath->rect, longPath->rect);

    const auto* line = findNode(*longResult.view, "input_line.query");
    ASSERT_TRUE(line != nullptr);
    if (!line) return;
    // Stays inside the header...
    ASSERT_TRUE(line->rect.x + line->rect.width <= 60);
    // ...keeps the sigil...
    ASSERT_TRUE(line->content.rfind("> ", 0) == 0);
    // ...and shows the END of the query, not the beginning.
    ASSERT_TRUE(line->content.find("TAIL") != std::string::npos);
    ASSERT_TRUE(line->content.find(std::string(50, 'x')) == std::string::npos);
}

// Slicing must respect grapheme boundaries: a byte-wise cut would emit half a
// multi-byte character.
TEST(theScrolledQueryIsCutOnCharacterBoundaries) {
    auto value = request(40, 12);
    value.inputLineActive = true;
    // 60 two-byte characters; any byte-wise slice lands mid-character.
    std::string query;
    for (int i = 0; i < 60; ++i) query += "\u00e9";
    value.inputLineQuery = query;
    ShellState state;
    auto result = computeShellLayout(value, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;
    const auto* line = findNode(*result.view, "input_line.query");
    ASSERT_TRUE(line != nullptr);
    if (!line) return;
    // Every byte after the "> " sigil belongs to a whole two-byte character.
    const auto text = line->content.substr(2);
    ASSERT_TRUE(text.size() % 2 == 0);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        ASSERT_EQ(static_cast<unsigned char>(text[i]), 0xC3u);
    }
}

TEST(aNarrowHeaderStillGivesTheInputLineRoom) {
    auto value = request(30, 12);
    value.inputLineActive = true;
    value.inputLineQuery = "query";
    ShellState state;
    auto result = computeShellLayout(value, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;
    const auto* line = findNode(*result.view, "input_line.query");
    ASSERT_TRUE(line != nullptr);
    if (line) {
        ASSERT_TRUE(line->rect.width > 0);
        // And it stays inside the header row.
        ASSERT_TRUE(line->rect.x + line->rect.width <= 30);
    }
}

// The layout counterpart to the renderer's routing proof: dimensions and the
// sigil must come from the request's Style, not from constants that used to
// live in ShellState.cpp.
TEST(shellLayoutTakesItsDimensionsAndSigilFromStyle) {
    ShellState state;
    state.togglePanel();

    auto narrow = request(100, 24);
    narrow.style.dimensions.panelTargetWidth = 24;
    auto narrowResult = computeShellLayout(narrow, state);
    ASSERT_TRUE(narrowResult.accepted());
    ASSERT_TRUE(narrowResult.view->panel.has_value());
    if (!narrowResult.view->panel) return;

    auto wide = request(100, 24);
    wide.style.dimensions.panelTargetWidth = 31;
    auto wideResult = computeShellLayout(wide, state);
    ASSERT_TRUE(wideResult.accepted());
    ASSERT_TRUE(wideResult.view->panel.has_value());
    if (!wideResult.view->panel) return;

    ASSERT_EQ(narrowResult.view->panel->width, 24);
    ASSERT_EQ(wideResult.view->panel->width, 31);

    // The minimum viewport is a style dimension too: raising it must make a
    // previously-accepted viewport too small.
    auto demanding = request(20, 4);
    demanding.style.dimensions.minimumColumns = 40;
    ASSERT_FALSE(computeShellLayout(demanding, state).accepted());

    // The input line renders the configured sigil rather than a literal "> ".
    auto styled = request(100, 24);
    styled.inputLineActive = true;
    styled.inputLineQuery = "abc";
    styled.style.inputLineSigil = ":: ";
    auto styledResult = computeShellLayout(styled, state);
    ASSERT_TRUE(styledResult.accepted());
    if (!styledResult.accepted()) return;
    bool sawStyledSigil = false;
    for (auto const& node : styledResult.view->accessibilityNodes) {
        if (node.id == "input_line.query") {
            sawStyledSigil = node.content.rfind(":: ", 0) == 0;
        }
    }
    ASSERT_TRUE(sawStyledSigil);
}

// The chrome heights and the gutter width were exposed as style dimensions but
// silently ignored -- configurable in appearance only.  These pin them to real
// geometry so the struct cannot drift back into advertising fields it drops.
TEST(chromeHeightsAndGutterWidthAreHonoured) {
    ShellState state;
    state.togglePanel();

    auto tall = request(100, 24);
    tall.style.dimensions.headerHeight = 2;
    tall.style.dimensions.footerHeight = 3;
    tall.style.dimensions.tabBarHeight = 2;
    auto tallResult = computeShellLayout(tall, state);
    ASSERT_TRUE(tallResult.accepted());
    if (!tallResult.accepted()) return;
    assertRect(*tallResult.view->header, {0, 0, 100, 2});
    assertRect(*tallResult.view->footer, {0, 21, 100, 3});
    // The tab bar starts below the header, and the panel spans what is left
    // between header and footer.
    ASSERT_EQ(tallResult.view->tabBar->y, 2);
    ASSERT_EQ(tallResult.view->tabBar->height, 2);
    ASSERT_EQ(tallResult.view->panel->y, 2);
    ASSERT_EQ(tallResult.view->panel->height, 19);
    // Content begins after header + tab bar.
    ASSERT_EQ(tallResult.view->panes.front().content.y, 4);

    auto wideGutter = request(100, 24);
    wideGutter.style.dimensions.scrollbarGutterWidth = 3;
    auto gutterResult = computeShellLayout(wideGutter, state);
    ASSERT_TRUE(gutterResult.accepted());
    if (!gutterResult.accepted()) return;
    auto const& pane = gutterResult.view->panes.front();
    ASSERT_EQ(pane.scrollbar.width, 3);
    ASSERT_EQ(pane.content.width, pane.frame.width - 3);
    ASSERT_EQ(gutterResult.view->panelScrollbar->width, 3);
}

// Layout budgets are cell counts, so a label's width must be its display width.
// Byte counts over-measure every non-ASCII label -- and style glyphs are now
// configurable, so they can be non-ASCII too.
TEST(labelWidthsAreMeasuredInCellsNotBytes) {
    ShellState state;

    auto ascii = request(100, 24);
    ascii.tabs = {{"ab", "ab tab", true}};
    auto asciiResult = computeShellLayout(ascii, state);
    ASSERT_TRUE(asciiResult.accepted());
    if (!asciiResult.accepted()) return;

    // Two fullwidth characters: 4 bytes each, 2 cells each.  Measured in cells
    // this tab is 4 wide; measured in bytes it would be 8.  Tabs are tight by
    // default (empty edge glyphs, no padding), so the hit rect is the label.
    auto wide = request(100, 24);
    wide.tabs = {{"\xef\xbc\xa1\xef\xbc\xa2", "wide tab", true}};
    auto wideResult = computeShellLayout(wide, state);
    ASSERT_TRUE(wideResult.accepted());
    if (!wideResult.accepted()) return;

    ASSERT_EQ(asciiResult.view->tabHits.front().rect.width, 2);
    ASSERT_EQ(wideResult.view->tabHits.front().rect.width, 4);
}

// Default tabs are tight: no trailing padding, exactly one separator cell
// between adjacent tabs, emitted as its own TabSeparator node.
TEST(defaultTabsAreTightWithOneSeparatorCellBetween) {
    ShellState state;
    auto req = request(100, 24);
    req.tabs = {{"aa", "aa tab", true}, {"bb", "bb tab", false}};
    auto result = computeShellLayout(req, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;

    auto const& hits = result.view->tabHits;
    ASSERT_EQ(hits.size(), std::size_t{2});
    ASSERT_EQ(hits[0].rect.width, 2);
    ASSERT_EQ(hits[1].rect.width, 2);
    // One separator cell sits in the gap: second tab starts one past the first.
    ASSERT_EQ(hits[1].rect.x, hits[0].rect.right() + 1);

    std::size_t separators = 0;
    for (auto const& node : result.view->accessibilityNodes) {
        if (node.kind == ShellNodeKind::TabSeparator) {
            ++separators;
            ASSERT_EQ(node.rect.width, 1);
            ASSERT_EQ(node.rect.x, hits[0].rect.right());
        }
    }
    ASSERT_EQ(separators, std::size_t{1});
}

// Configured edge glyphs widen each tab; a multi-cell separator widens the gap
// and its glyph is carried on the TabSeparator node.
TEST(configuredTabEdgeAndSeparatorGlyphsChangeGeometry) {
    ShellState state;
    auto req = request(100, 24);
    req.style.tab.leftEdge = "[";
    req.style.tab.rightEdge = "]";
    req.style.tab.separator = " | ";
    req.tabs = {{"aa", "aa tab", false}, {"bb", "bb tab", false}};
    auto result = computeShellLayout(req, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;

    auto const& hits = result.view->tabHits;
    ASSERT_EQ(hits.size(), std::size_t{2});
    // "[aa]" = 4 cells.
    ASSERT_EQ(hits[0].rect.width, 4);
    ASSERT_EQ(hits[1].rect.width, 4);
    // " | " = 3 cells between the tabs.
    ASSERT_EQ(hits[1].rect.x, hits[0].rect.right() + 3);

    bool sawSeparator = false;
    for (auto const& node : result.view->accessibilityNodes) {
        if (node.kind == ShellNodeKind::TabSeparator) {
            sawSeparator = true;
            ASSERT_EQ(node.rect.width, 3);
            ASSERT_EQ(node.content, std::string{" | "});
        }
    }
    ASSERT_TRUE(sawSeparator);
}

// The active tab stays visible when separators and edge glyphs push earlier
// tabs off the left -- the firstTab window must count separator width, not just
// the tabs.  Uses a multi-cell separator and non-empty edges in a tight bar.
TEST(activeTabStaysVisibleWhenSeparatorsPushEarlierTabsOff) {
    ShellState state;
    auto req = request(24, 24);  // narrow bar
    req.style.tab.leftEdge = "[";
    req.style.tab.rightEdge = "]";
    req.style.tab.separator = " | ";
    req.tabs = {{"one", "one tab", false}, {"two", "two tab", false},
                {"three", "three tab", false}, {"four", "four tab", true}};
    auto result = computeShellLayout(req, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;

    bool activeVisible = false;
    for (auto const& hit : result.view->tabHits) {
        if (hit.index == 3) activeVisible = true;
    }
    ASSERT_TRUE(activeVisible);
}

// A separator is never emitted unless a following tab is actually placed: when
// the row has room for the gap but not the next chip, no dangling separator is
// left past the last visible tab.  The separator count equals gaps between the
// tabs that landed (placed - 1).
TEST(noTabSeparatorDanglesPastTheLastPlacedTab) {
    ShellState state;
    auto req = request(14, 24);
    req.style.dimensions.minimumColumns = 8;
    req.style.tab.separator = " || ";  // 4 cells
    req.tabs = {{"alpha", "alpha tab", true}, {"bravo", "bravo tab", false},
                {"charlie", "charlie tab", false}};
    auto result = computeShellLayout(req, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;

    std::size_t separators = 0;
    for (auto const& node : result.view->accessibilityNodes) {
        if (node.kind == ShellNodeKind::TabSeparator) ++separators;
    }
    ASSERT_TRUE(result.view->tabHits.size() >= std::size_t{1});
    ASSERT_EQ(separators, result.view->tabHits.size() - 1);
}

// --- composed-chrome routing ---

WidgetDescriptor literalField(std::string id, std::string text, int rank) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Field;
    w.id = std::move(id);
    ValueSource v;
    v.literal = std::move(text);
    w.value = v;
    w.rank = rank;
    return w;
}

WidgetDescriptor providerField(std::string id, std::string provider, int rank) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Field;
    w.id = std::move(id);
    ValueSource v;
    v.isProvider = true;
    v.provider = std::move(provider);
    w.value = v;
    w.rank = rank;
    return w;
}

// A composed header OWNS the header's status fields (built-in fields are gone,
// composed ids appear) and a composed footer OWNS the whole footer row (fields,
// hint, and actions are gone).
TEST(composedChromeReplacesBuiltinHeaderAndFooter) {
    ShellState state;
    auto value = request(100, 24);
    ChromeComposition comp;
    RowDescriptor header;
    header.left.push_back(literalField("custom.header", "HELLO", 0));
    comp.header = header;
    RowDescriptor footer;
    footer.left.push_back(literalField("custom.footer", "WORLD", 0));
    comp.footer = footer;
    value.composedChrome = comp;

    auto result = computeShellLayout(value, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;

    // Composed nodes present.
    const auto* customHeader = findNode(*result.view, "custom.header");
    const auto* customFooter = findNode(*result.view, "custom.footer");
    ASSERT_TRUE(customHeader != nullptr);
    ASSERT_TRUE(customFooter != nullptr);
    if (customHeader) {
        ASSERT_EQ(customHeader->kind, ShellNodeKind::HeaderField);
        ASSERT_EQ(customHeader->content, std::string{"HELLO"});
    }
    if (customFooter) {
        ASSERT_EQ(customFooter->kind, ShellNodeKind::FooterField);
        ASSERT_EQ(customFooter->content, std::string{"WORLD"});
    }
    // Every built-in field, action, and hint is gone -- replace, not merge.
    ASSERT_TRUE(findNode(*result.view, "active_command") == nullptr);
    ASSERT_TRUE(findNode(*result.view, "current_path") == nullptr);
    ASSERT_TRUE(findNode(*result.view, "git_branch") == nullptr);
    ASSERT_TRUE(findNode(*result.view, "status.retry") == nullptr);
}

// An uncomposed region keeps its built-in chrome even when the OTHER region is
// composed: composing only the footer must not disturb the header, and vice
// versa (byte-identical default path).
TEST(composingOneRegionLeavesTheOtherBuiltin) {
    ShellState state;
    auto footerOnly = request(100, 24);
    ChromeComposition comp;
    RowDescriptor footer;
    footer.left.push_back(literalField("custom.footer", "WORLD", 0));
    comp.footer = footer;
    footerOnly.composedChrome = comp;

    auto result = computeShellLayout(footerOnly, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;
    // Header is untouched built-in; footer is composed.
    ASSERT_TRUE(findNode(*result.view, "active_command") != nullptr);
    ASSERT_TRUE(findNode(*result.view, "custom.footer") != nullptr);
    ASSERT_TRUE(findNode(*result.view, "git_branch") == nullptr);
}

// A composed header's `provider` widget resolves live (value, label, command)
// through the request's resolver, and the input line still follows the composed
// left group when a picker is open (the header stays left-group only).
TEST(composedHeaderResolvesProvidersAndKeepsTheInputLine) {
    ShellState state;
    auto value = request(100, 24);
    value.inputLineActive = true;
    value.inputLineQuery = "abc";
    ChromeComposition comp;
    RowDescriptor header;
    header.left.push_back(providerField("live.path", "path", 0));
    comp.header = header;
    value.composedChrome = comp;
    value.chromeProviderResolver =
        [](std::string_view id) -> std::optional<ResolvedProvider> {
        if (id == "path")
            return ResolvedProvider{"src/main.cpp", "Current path",
                                    std::string{"panel.show_files"}};
        return std::nullopt;
    };

    auto result = computeShellLayout(value, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;
    const auto* live = findNode(*result.view, "live.path");
    ASSERT_TRUE(live != nullptr);
    if (live) {
        ASSERT_EQ(live->content, std::string{"src/main.cpp"});
        ASSERT_EQ(live->label, std::string{"Current path"});
        ASSERT_TRUE(live->commandId.has_value());
        ASSERT_EQ(*live->commandId, std::string{"panel.show_files"});
    }
    // The picker input line coexists after the composed left group.
    const auto* line = findNode(*result.view, "input_line.query");
    ASSERT_TRUE(line != nullptr);
    if (live && line) ASSERT_TRUE(line->rect.x >= live->rect.right());
}

// A node-less left `Spacer` still consumes header cells: the input line must
// follow the WHOLE composed group (spacer included), not just the last emitted
// node -- else the query would overlap the spacer's cells.
TEST(composedHeaderSpacerPushesTheInputLinePastItsCells) {
    ShellState state;
    auto value = request(100, 24);
    value.inputLineActive = true;
    value.inputLineQuery = "abc";
    ChromeComposition comp;
    RowDescriptor header;
    header.left.push_back(literalField("h.field", "X", 0));
    WidgetDescriptor spacer;
    spacer.kind = WidgetKind::Spacer;
    spacer.id = "h.spacer";
    spacer.width = 20;
    header.left.push_back(spacer);
    comp.header = header;
    value.composedChrome = comp;

    auto result = computeShellLayout(value, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;
    const auto* field = findNode(*result.view, "h.field");
    const auto* line = findNode(*result.view, "input_line.query");
    ASSERT_TRUE(field != nullptr);
    ASSERT_TRUE(line != nullptr);
    // The spacer (no node) sits after the field; the input line clears both.
    if (field && line) {
        const int spacerRight = field->rect.right() + 1 + 20;  // sep + width
        ASSERT_TRUE(line->rect.x >= spacerRight);
    }
}

// ---------------------------------------------------------------------------
// computeShellLayout's full ShellViewState across a broad input matrix, captured
// from the CURRENT code and committed. The layout-engine rearchitecture must
// reproduce this byte-for-byte. The palette overlay is added downstream in
// snapshot.cpp from panes.front().content, which is in this dump, so pinning this
// output pins the palette too; the palette's own influence on THIS function is
// the input-line reservation, covered by the input-line cases below.
// Regenerate (only after an intentional layout change) with SSG_REGEN_GOLDEN=1.

std::string rectStr(const Rect& r) {
    std::ostringstream o;
    o << r.x << ',' << r.y << ',' << r.width << ',' << r.height;
    return o.str();
}

void dumpOptRect(std::ostringstream& o, const char* name,
                 const std::optional<Rect>& r) {
    o << name << '=' << (r ? rectStr(*r) : std::string{"none"}) << '\n';
}

std::string serializeLayout(const ShellLayoutResult& result) {
    std::ostringstream o;
    if (!result.accepted()) {
        o << "ERROR code=" << static_cast<int>(result.error->code)
          << " msg=" << result.error->message << '\n';
        return o.str();
    }
    const auto& v = *result.view;
    o << "viewport=" << v.viewport.columns << 'x' << v.viewport.rows << '\n';
    dumpOptRect(o, "header", v.header);
    dumpOptRect(o, "footer", v.footer);
    dumpOptRect(o, "tabBar", v.tabBar);
    dumpOptRect(o, "panel", v.panel);
    dumpOptRect(o, "panelScrollbar", v.panelScrollbar);
    dumpOptRect(o, "prompt", v.prompt);
    o << "panes:\n";
    for (const auto& p : v.panes) {
        o << "  id=" << p.id.value() << " frame=" << rectStr(p.frame)
          << " content=" << rectStr(p.content) << " scrollbar="
          << rectStr(p.scrollbar) << '\n';
    }
    o << "tabHits:\n";
    for (const auto& t : v.tabHits) {
        o << "  index=" << t.index << " rect=" << rectStr(t.rect) << '\n';
    }
    o << "a11y:\n";
    for (const auto& n : v.accessibilityNodes) {
        o << "  kind=" << static_cast<int>(n.kind) << " role="
          << static_cast<int>(n.role) << " rect=" << rectStr(n.rect)
          << " id=" << n.id << " label=" << n.label << " content=" << n.content
          << " cmd=" << (n.commandId ? *n.commandId : std::string{}) << '\n';
    }
    o << "palette=" << (v.palette ? "present" : "none") << '\n';
    return o.str();
}

// One case: a descriptive name and the request+state it produces. The state is
// mutated by `configure` (panel/splits/focus/distraction-free), so every knob
// that changes the output is represented.
std::string captureGoldenMatrix() {
    std::ostringstream out;
    auto emitCase = [&](const std::string& name, ShellLayoutRequest req,
                        const std::function<void(ShellState&)>& configure) {
        ShellState state;
        if (configure) configure(state);
        out << "=== " << name << " ===\n"
            << serializeLayout(computeShellLayout(req, state)) << '\n';
    };
    auto panelOn = [](ShellState& s) { s.togglePanel(); };

    // Viewport sizes, panel off/on.
    emitCase("min-20x4", request(20, 4), nullptr);
    emitCase("min-20x4-panel", request(20, 4), panelOn);
    emitCase("below-min-19x4", request(19, 4), nullptr);
    emitCase("std-80x24", request(80, 24), nullptr);
    emitCase("std-80x24-panel", request(80, 24), panelOn);
    emitCase("tall-100x40-panel", request(100, 40), panelOn);
    emitCase("odd-33x11-panel", request(33, 11), panelOn);

    // Panel width thresholds (default target 24 / min 12 / editorMin 20).
    for (int cols : {31, 32, 43, 44, 45}) {
        emitCase("panel-threshold-" + std::to_string(cols), request(cols, 24),
                 panelOn);
    }

    // Prompt reservation 0..3, plus the two error paths.
    for (int rows : {0, 1, 2, 3}) {
        auto req = request(80, 24);
        req.reservedPromptRows = static_cast<std::uint8_t>(rows);
        emitCase("prompt-rows-" + std::to_string(rows), req, nullptr);
    }
    {
        auto tooTall = request(80, 5);  // header+tabbar+footer leave 2 editor rows
        tooTall.reservedPromptRows = 3;
        emitCase("prompt-leaves-no-row", tooTall, nullptr);
        auto invalid = request(80, 24);
        invalid.reservedPromptRows = 4;
        emitCase("prompt-rows-invalid-4", invalid, nullptr);
    }

    // Input line (the palette's influence on this function).
    {
        auto req = request(80, 24);
        req.inputLineActive = true;
        req.inputLineQuery = "find";
        req.inputLineGhost = "er";
        emitCase("inputline-query-ghost", req, nullptr);
        auto longQ = request(60, 24);
        longQ.inputLineActive = true;
        longQ.inputLineQuery =
            "a-very-long-query-that-must-scroll-under-the-sigil-xyz";
        emitCase("inputline-long-query", longQ, nullptr);
    }

    // Pane topologies.
    emitCase("split-horizontal", request(80, 24),
             [](ShellState& s) { s.splitActive(SplitAxis::Horizontal); });
    emitCase("split-vertical", request(80, 24),
             [](ShellState& s) { s.splitActive(SplitAxis::Vertical); });
    emitCase("split-nested", request(80, 24), [](ShellState& s) {
        s.splitActive(SplitAxis::Horizontal);
        s.splitActive(SplitAxis::Vertical);
    });

    // Tabs: none, and many (narrow, to exercise the auto-scroll window).
    {
        auto none = request(80, 24);
        none.tabs.clear();
        emitCase("tabs-none", none, nullptr);
        auto many = request(30, 24);
        many.tabs.clear();
        for (int i = 0; i < 8; ++i) {
            many.tabs.push_back({"file" + std::to_string(i) + ".cpp",
                                 "file" + std::to_string(i) + " tab",
                                 i == 6, i % 2 == 0});
        }
        emitCase("tabs-many-narrow", many, nullptr);
        auto multibyte = request(40, 24);
        multibyte.tabs = {{"\xE4\xB8\x80\xE4\xBA\x8C.txt", "cjk tab", true}};
        emitCase("tabs-multibyte", multibyte, nullptr);
    }

    // Focus targets.
    emitCase("focus-panel", request(80, 24), [](ShellState& s) {
        s.togglePanel();
        (void)s.focusPanel();
    });
    emitCase("focus-prompt", request(80, 24),
             [](ShellState& s) { s.enterPromptFocus(); });

    // Empty state.
    {
        auto req = request(80, 24);
        req.emptyState = true;
        emitCase("empty-state", req, nullptr);
    }

    // Distraction-free (with and without a prompt reservation, which it ignores).
    emitCase("distraction-free", request(80, 24),
             [](ShellState& s) { s.toggleDistractionFree(); });
    {
        auto req = request(80, 24);
        req.reservedPromptRows = 2;
        emitCase("distraction-free-with-prompt", req,
                 [](ShellState& s) { s.toggleDistractionFree(); });
    }

    // Non-default Style dimensions.
    {
        auto req = request(80, 24);
        req.style.dimensions.headerHeight = 2;
        req.style.dimensions.footerHeight = 2;
        req.style.dimensions.tabBarHeight = 2;
        req.style.dimensions.scrollbarGutterWidth = 2;
        req.style.dimensions.panelTargetWidth = 30;
        emitCase("non-default-style-panel", req, panelOn);
    }
    return out.str();
}

std::string readGoldenFile(const std::string& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(shellLayoutMatchesTheCommittedGolden) {
    const std::string actual = captureGoldenMatrix();
    const std::string path =
        std::string{SSG_SOURCE_DIR} + "/tests/fixtures/ui_layout/golden.txt";
    if (std::getenv("SSG_REGEN_GOLDEN") != nullptr) {
        std::ofstream{path, std::ios::binary} << actual;
        std::cout << "  regenerated " << path << '\n';
        ++passed;
        return;
    }
    const std::string expected = readGoldenFile(path);
    if (expected.empty()) {
        std::cerr << "  layout golden missing; run SSG_REGEN_GOLDEN=1 to create "
                  << path << '\n';
        ++failed;
        return;
    }
    if (actual != expected) {
        std::cerr << "  layout golden mismatch (byte-for-byte layout changed)\n";
        ++failed;
    } else {
        ++passed;
    }
}

int main() {
    RUN(handAuthoredGeometryGoldens);
    RUN(viewportAndPromptErrorsAreTyped);
    RUN(paneCommandsPreserveTopologyAndOrder);
    RUN(panelCommandsPreserveProviderStateWhenHidden);
    RUN(accessibilityNodesHaveLabelsAndRoles);
    RUN(accessibilityLeafNodesCarryDisplayContent);
    RUN(dirtyTabContentShowsMarker);
    RUN(focusTransitionsFollowTheNavigationTable);
    RUN(hidingAnUnfocusedPanelLeavesFocusUntouched);
    RUN(typingInTheInputLineNeverMovesTheStatusFields);
    RUN(anOpenPickerHidesTheTabBarAndReclaimsItsRow);
    RUN(headerFieldWidthIsMeasuredInCellsNotBytes);
    RUN(fieldsAreStableEvenWhenTheHeaderIsTight);
    RUN(headerNodesNeverOverlap);
    RUN(anOverlongQueryScrollsItsOwnTextAndLeavesFieldsAlone);
    RUN(theScrolledQueryIsCutOnCharacterBoundaries);
    RUN(aNarrowHeaderStillGivesTheInputLineRoom);
    RUN(nonOverlapAndCardinalityProperties);
    RUN(statusFieldManifestHasExactOrderAndLabels);
    RUN(shellLayoutTakesItsDimensionsAndSigilFromStyle);
    RUN(chromeHeightsAndGutterWidthAreHonoured);
    RUN(labelWidthsAreMeasuredInCellsNotBytes);
    RUN(defaultTabsAreTightWithOneSeparatorCellBetween);
    RUN(configuredTabEdgeAndSeparatorGlyphsChangeGeometry);
    RUN(activeTabStaysVisibleWhenSeparatorsPushEarlierTabsOff);
    RUN(noTabSeparatorDanglesPastTheLastPlacedTab);
    RUN(composedChromeReplacesBuiltinHeaderAndFooter);
    RUN(composingOneRegionLeavesTheOtherBuiltin);
    RUN(composedHeaderResolvesProvidersAndKeepsTheInputLine);
    RUN(composedHeaderSpacerPushesTheInputLinePastItsCells);
    RUN(shellLayoutMatchesTheCommittedGolden);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
