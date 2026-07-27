#include "ssg/ShellState.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <fstream>
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
    assertRect(*wideResult.view->prompt, {24, 2, 56, 2});
    assertRect(wideResult.view->panes[0].content, {24, 4, 55, 7});
    assertRect(wideResult.view->panes[0].scrollbar, {79, 4, 1, 7});
    assertRect(*wideResult.view->footer, {0, 11, 80, 1});

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

TEST(exactOwnedCommandSet) {
    constexpr std::array expected{
        std::string_view{"pane.split_horizontal"},
        std::string_view{"pane.split_vertical"},
        std::string_view{"pane.close"},
        std::string_view{"pane.next"},
        std::string_view{"pane.previous"},
        std::string_view{"pane.focus_left"},
        std::string_view{"pane.focus_right"},
        std::string_view{"pane.focus_up"},
        std::string_view{"pane.focus_down"},
        std::string_view{"panel.toggle"},
        std::string_view{"panel.focus"},
        std::string_view{"panel.show_files"},
        std::string_view{"panel.show_git_status"},
        std::string_view{"panel.next_provider"},
        std::string_view{"panel.previous_provider"},
        std::string_view{"view.toggle_distraction_free"},
    };
    constexpr ShellCommandSet commands;
    ASSERT_EQ(commands.descriptors.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(commands.descriptors[i].id, expected[i]);
    }
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

    std::ostringstream actual;
    for (const auto& node : nodes) {
        actual << static_cast<int>(node.kind) << '|' << node.id << '|'
               << node.label << '|' << node.rect.x << ',' << node.rect.y << ','
               << node.rect.width << ',' << node.rect.height << '|'
               << static_cast<int>(node.role) << '\n';
    }
    std::ifstream golden(std::string{SSG_SOURCE_DIR} +
                         "/tests/fixtures/ui_layout/accessibility.txt");
    const std::string expected((std::istreambuf_iterator<char>(golden)),
                               std::istreambuf_iterator<char>());
    if (actual.str() != expected) {
        std::cerr << "  accessibility actual:\n" << actual.str()
                  << "  accessibility expected:\n" << expected;
    }
    ASSERT_EQ(actual.str(), expected);
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
// The header input line (doc/spec-input-line.md).

const AccessibilityNode* findNode(const ShellViewState& view, std::string_view id) {
    for (const auto& node : view.accessibilityNodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

// The reason this change exists: typing into a picker must not move the status
// fields.  Before the fix the query was laid out first and pushed them right,
// so this fails against the old order.
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

// A focused text input that accepts keys while being invisible is worse than a
// field that moved, so at a narrow header the fields yield instead.
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

// The leader hint occupies the same slot, so it must follow the input line's
// position rather than staying at the old left edge -- otherwise the header
// jumps between two layouts depending on which is active.
TEST(theLeaderHintSitsWhereTheInputLineWould) {
    auto withHint = request(120, 12);
    withHint.leaderHint = "leader: Escape";
    auto withLine = request(120, 12);
    withLine.inputLineActive = true;
    ShellState state;
    auto hintResult = computeShellLayout(withHint, state);
    auto lineResult = computeShellLayout(withLine, state);
    ASSERT_TRUE(hintResult.accepted() && lineResult.accepted());
    if (!hintResult.accepted() || !lineResult.accepted()) return;
    const auto* hint = findNode(*hintResult.view, "leader");
    const auto* line = findNode(*lineResult.view, "input_line.query");
    ASSERT_TRUE(hint != nullptr && line != nullptr);
    if (hint && line) ASSERT_EQ(hint->rect.x, line->rect.x);
}

TEST(leaderHintRendersInTheHeaderWhenPresent) {
    auto value = request(80, 12);
    value.leaderHint = "leader: Escape";
    ShellState state;
    auto result = computeShellLayout(value, state);
    ASSERT_TRUE(result.accepted());
    const AccessibilityNode* leader = nullptr;
    for (const auto& node : result.view->accessibilityNodes) {
        if (node.kind == ShellNodeKind::HeaderField && node.id == "leader") {
            leader = &node;
        }
    }
    ASSERT_TRUE(leader != nullptr);
    if (leader) {
        ASSERT_EQ(leader->content, std::string{"leader: Escape"});
        ASSERT_TRUE(leader->role == SemanticRole::Prompt);
        ASSERT_EQ(leader->rect.y, 0);
    }
    // No hint node when the request carries no leader sequence.
    auto plain = computeShellLayout(request(80, 12), state);
    ASSERT_TRUE(plain.accepted());
    const bool hasLeader = std::ranges::any_of(
        plain.view->accessibilityNodes,
        [](const auto& node) { return node.id == "leader"; });
    ASSERT_FALSE(hasLeader);
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

int main() {
    RUN(handAuthoredGeometryGoldens);
    RUN(viewportAndPromptErrorsAreTyped);
    RUN(paneCommandsPreserveTopologyAndOrder);
    RUN(panelCommandsPreserveProviderStateWhenHidden);
    RUN(exactOwnedCommandSet);
    RUN(accessibilityNodesHaveLabelsAndRoles);
    RUN(accessibilityLeafNodesCarryDisplayContent);
    RUN(dirtyTabContentShowsMarker);
    RUN(focusTransitionsFollowTheNavigationTable);
    RUN(hidingAnUnfocusedPanelLeavesFocusUntouched);
    RUN(leaderHintRendersInTheHeaderWhenPresent);
    RUN(typingInTheInputLineNeverMovesTheStatusFields);
    RUN(fieldsAreStableEvenWhenTheHeaderIsTight);
    RUN(headerNodesNeverOverlap);
    RUN(anOverlongQueryScrollsItsOwnTextAndLeavesFieldsAlone);
    RUN(theScrolledQueryIsCutOnCharacterBoundaries);
    RUN(aNarrowHeaderStillGivesTheInputLineRoom);
    RUN(theLeaderHintSitsWhereTheInputLineWould);
    RUN(nonOverlapAndCardinalityProperties);
    RUN(statusFieldManifestHasExactOrderAndLabels);
    RUN(shellLayoutTakesItsDimensionsAndSigilFromStyle);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
