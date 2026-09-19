
#include "../grid_test_frame.h"
#include "../grid_test_view.h"
#include "../test_helpers.h"
#include "../editor_test_support.h"

#include <ssg/GridPresenter.h>
#include <ssg/Keymap.h>
#include <ssg/UiTree.h>

#include <algorithm>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <type_traits>

#ifndef SSG_SOURCE_SCAN_ROOT
#error "SSG_SOURCE_SCAN_ROOT must name the source tree"
#endif

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = testRuntimePath("runtime_snapshot_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorConfig configFor(const std::filesystem::path& root) {
    return {root / "workspace", root / "recovery", root / "archive"};
}

const ssg::UiNode* nodeById(const ssg::UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    if (const auto* container = std::get_if<ssg::UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (const auto* found = nodeById(child, id)) return found;
        }
    }
    return nullptr;
}

TEST(constructionRejectsInvalidCwd) {
    auto root = uniqueRoot("invalid_cwd");
    auto result = ssg::createEditor(configFor(root / "missing"));
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.message.empty());
}

TEST(sessionProjectionResolvesStatusFieldsAndHint) {
    auto root = uniqueRoot("ui_tree_values");
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    const auto assertProjection = [&](const ssg::GridPresentation& frame) {
        const auto* path = nodeById(
            frame.uiTree.root, ssg::kHeaderPathFieldNodeId);
        ASSERT_TRUE(path != nullptr && path->resolved.has_value());
        if (path && path->resolved) {
            ASSERT_TRUE(path->resolved->command ==
                        std::optional<std::string>{"panel.show_files"});
            ASSERT_TRUE(path->resolved->role == ssg::SemanticRole::Header);
        }
        const auto* hint = nodeById(frame.uiTree.root, ssg::kFooterHintNodeId);
        ASSERT_TRUE(hint != nullptr && hint->resolved.has_value());
        if (hint && hint->resolved) {
            ASSERT_TRUE(hint->resolved->command ==
                        std::optional<std::string>{"help.open"});
            ASSERT_TRUE(hint->resolved->role == ssg::SemanticRole::Footer);
        }
    };

    auto frame = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(frame.has_value());
    if (frame) assertProjection(*frame);

    ASSERT_TRUE(runtime.dispatch("settings.export_workspace").accepted());
    frame = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(frame.has_value());
    if (frame) {
        assertProjection(*frame);
        const auto* status =
            nodeById(frame->uiTree.root, ssg::kFooterStatusFieldNodeId);
        ASSERT_TRUE(status != nullptr && status->resolved.has_value());
        if (status && status->resolved) {
            ASSERT_TRUE(status->resolved->role ==
                        ssg::SemanticRole::StatusInfo);
        }
    }
    std::filesystem::remove_all(root);
}

// commandCaseTableExactlyMatchesP0Catalog is deleted.  It proved the test
// case table listed exactly the catalog's commands, back when the table was
// typed out by hand.  The cases are now derived from the running editor, so it
// compared a thing to itself.

TEST(runtimeSourcesDoNotIncludeFixtureModel) {
    auto root = std::filesystem::path{SSG_SOURCE_SCAN_ROOT};
    bool found = false;
    bool snapshotConstCast = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "src"}) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.rfind("src/", 0) != 0 && relative != "src/Editor.cpp") continue;
        std::ifstream input{entry.path()};
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        found = found || text.find("FixtureModel") != std::string::npos;
        snapshotConstCast =
            snapshotConstCast ||
            text.find("const_cast<Editor*>") != std::string::npos;
    }

    ASSERT_FALSE(found);
    ASSERT_FALSE(snapshotConstCast);
}

TEST(defaultTerminalKeymapIsValid) {
    const auto keymap = ssg::defaultTerminalKeymap();
    ASSERT_FALSE(keymap.bindings.empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.validate().empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.hasGlobalBinding("settings.open"));
}

TEST(defaultTerminalKeymapBindingsAreSingleStroke) {
    const auto keymap = ssg::defaultTerminalKeymap();
    for (const auto& binding : keymap.bindings) {
        ASSERT_EQ(binding.sequence.size(), std::size_t{1});
    }
}

TEST(curatedKeymapBindingsAreArgumentFree) {
    auto root = uniqueRoot("keymap_argfree");
    std::ofstream{root / "workspace" / "doc.txt"} << "alpha\nbeta\n";
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"doc.txt"}).accepted());

    std::set<std::string> commands;
    for (const auto& binding : ssg::defaultTerminalKeymap().bindings) {
        commands.insert(binding.commandId);
    }
    for (const auto& command : commands) {
        auto result = runtime.dispatch(command);
        const bool argumentError =
            result.message.find("requires") != std::string::npos ||
            result.message.find("wrong type") != std::string::npos ||
            result.message.find("payload") != std::string::npos;
        if (argumentError) {
            std::cerr << "  argument-required command bound: " << command
                      << " (" << result.message << ")\n";
        }
        ASSERT_FALSE(argumentError);
    }
    std::filesystem::remove_all(root);
}

TEST(curatedKeymapResolvesPerContext) {
    const auto keymap = ssg::defaultTerminalKeymap();

    const auto down = *ssg::parseKeySequence({"ArrowDown"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "editor").commandId,
              std::string{"cursor.line_down"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "panel").commandId,
              std::string{"tree.select_next"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "prompt").commandId,
              std::string{"prompt.next"});

    const auto save = *ssg::parseKeySequence({"Mod+KeyS"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(save, "editor").commandId,
              std::string{"file.save"});

    const auto panelFocus = *ssg::parseKeySequence({"Mod+KeyO"});
    for (const auto context : {"editor", "panel"}) {
        ASSERT_EQ(
            ssg::KeymapMatcher{keymap}
               .resolveSequence(panelFocus, context)
               .commandId,
            std::string{"panel.toggle_focus"});
    }
    ASSERT_EQ(
        ssg::KeymapMatcher{keymap}
            .resolveSequence(*ssg::parseKeySequence({"Mod+BracketLeft"}),
                             "editor")
            .commandId,
        std::string{"panel.shrink"});
    ASSERT_EQ(
        ssg::KeymapMatcher{keymap}
            .resolveSequence(*ssg::parseKeySequence({"Mod+BracketRight"}),
                             "editor")
            .commandId,
        std::string{"panel.grow"});
    ASSERT_EQ(
        ssg::KeymapMatcher{keymap}
            .resolveSequence(*ssg::parseKeySequence({"Tab"}), "editor")
            .commandId,
        std::string{"text.tab"});

    const auto altHome = *ssg::parseKeySequence({"Mod+Home"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altHome, "editor").commandId,
              std::string{"cursor.document_start"});
    const auto altEnd = *ssg::parseKeySequence({"Mod+End"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altEnd, "editor").commandId,
              std::string{"cursor.document_end"});

    const auto settings = *ssg::parseKeySequence({"Mod+Shift+KeyT"});
    for (const auto context : {"editor", "panel", "prompt"}) {
        ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(settings, context).commandId,
                  std::string{"settings.open"});
    }

    struct ClipboardBinding {
        char const* key;
        char const* command;
    };
    for (auto const& binding : {ClipboardBinding{"Mod+KeyX", "clipboard.cut"},
                                ClipboardBinding{"Mod+KeyC", "clipboard.copy"},
                                ClipboardBinding{"Mod+KeyV", "clipboard.paste"}}) {
        const auto sequence = *ssg::parseKeySequence({binding.key});
        ASSERT_EQ(
            ssg::KeymapMatcher{keymap}.resolveSequence(sequence, "editor").commandId,
            std::string{binding.command});
    }

    for (auto const* const key : {"Mod+KeyX", "Mod+KeyC"}) {
        const auto sequence = *ssg::parseKeySequence({key});
        for (auto const* const context : {"panel", "prompt"}) {
            const auto resolved =
                ssg::KeymapMatcher{keymap}.resolveSequence(sequence, context);
            ASSERT_TRUE(resolved.commandId.rfind("clipboard.", 0) != 0);
        }
    }
    const auto paste = *ssg::parseKeySequence({"Mod+KeyV"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(paste, "prompt").commandId,
              std::string{"clipboard.paste"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(paste, "panel").commandId,
              std::string{"clipboard.paste"});

    const auto escape = *ssg::parseKeySequence({"Escape"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(escape, "prompt").commandId,
              std::string{"prompt.cancel"});

    const auto shiftRight = *ssg::parseKeySequence({"Shift+ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(shiftRight, "editor").commandId,
              std::string{"select.right"});
    const auto shiftUp = *ssg::parseKeySequence({"Shift+ArrowUp"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(shiftUp, "editor").commandId,
              std::string{"select.line_up"});
    const auto plainRight = *ssg::parseKeySequence({"ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(plainRight, "editor").commandId,
              std::string{"cursor.right"});
    const auto addNext = *ssg::parseKeySequence({"Mod+KeyD"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(addNext, "editor").commandId,
              std::string{"select.add_next_occurrence"});
    const auto findOpen = *ssg::parseKeySequence({"Mod+Slash"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(findOpen, "editor").commandId,
              std::string{"find.open"});
    const auto findWord = *ssg::parseKeySequence({"Mod+Digit8"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(findWord, "editor").commandId,
              std::string{"find.word_under_cursor"});
    const auto replaceOpen = *ssg::parseKeySequence({"Mod+KeyR"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(replaceOpen, "editor").commandId,
              std::string{"replace.open"});
    const auto tabNext = *ssg::parseKeySequence({"Mod+Period"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(tabNext, "editor").commandId,
              std::string{"tab.next"});
    const auto tabPrev = *ssg::parseKeySequence({"Mod+Comma"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(tabPrev, "editor").commandId,
              std::string{"tab.previous"});

    const auto del = *ssg::parseKeySequence({"Delete"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(del, "editor").commandId,
              std::string{"text.delete_forward"});
    const auto deleteWord = *ssg::parseKeySequence({"Mod+Backspace"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(deleteWord, "editor").commandId,
              std::string{"text.delete_word_backward"});
    const auto altWordLeft = *ssg::parseKeySequence({"Mod+ArrowLeft"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altWordLeft, "editor").commandId,
              std::string{"cursor.word_left"});
    const auto altWordRight = *ssg::parseKeySequence({"Mod+ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altWordRight, "editor").commandId,
              std::string{"cursor.word_right"});
    const auto altSelectWordLeft =
        *ssg::parseKeySequence({"Mod+Shift+ArrowLeft"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altSelectWordLeft, "editor").commandId,
              std::string{"select.word_left"});
    const auto altSelectWordRight =
        *ssg::parseKeySequence({"Mod+Shift+ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altSelectWordRight, "editor").commandId,
              std::string{"select.word_right"});
}

TEST(everyDocumentLineIsReachableAndTheCaretIsNeverLost) {
    auto root = uniqueRoot("viewport_reach");
    // More lines than fit, so the document genuinely scrolls.
    {
        std::ofstream file{root / "workspace" / "long.txt"};
        for (int line = 1; line <= 60; ++line) file << "line " << line << "\n";
    }
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"long.txt"}).accepted());
    runtime.focusEditor();

    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    // Walk the caret to the very last line.  The viewport must scroll against
    // the rows the editor PAINTS, not the terminal height: sized to the whole
    // terminal it stops short by the header, tab bar and footer, and the final
    // lines can never be shown.  The visible symptom is a caret the renderer
    // cannot place -- in a terminal the hardware cursor then stays wherever
    // painting ended, which is the footer.
    std::uint32_t lastVisibleLine = 0;
    for (int step = 0; step < 80; ++step) {
        auto snapshot = grid.present(runtime);
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        auto const& view = snapshot->viewport;
        auto const caretLine =
            snapshot->selections.primary().active.line.value();
        // The caret's line is always inside the window that is actually painted.
        ASSERT_TRUE(caretLine >= view.firstVisualRow);
        ASSERT_TRUE(caretLine < view.firstVisualRow + view.visibleRows.size());
        lastVisibleLine = std::max<std::uint32_t>(lastVisibleLine, caretLine);
        (void)grid.dispatch(runtime, "cursor.line_down");
    }
    // The last line of the document was reached, not merely approached.
    auto final = grid.present(runtime);
    ASSERT_TRUE(final.has_value());
    if (!final) return;
    ASSERT_EQ(lastVisibleLine, final->viewport.totalVisualRows - 1);

    // Scrolling to the maximum offset shows the final line, so no row is
    // stranded past the end of the scroll range.
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, 500}}).accepted());
    auto bottom = grid.present(runtime);
    ASSERT_TRUE(bottom.has_value());
    if (!bottom) return;
    auto const& view = bottom->viewport;
    ASSERT_EQ(view.firstVisualRow, view.scrollbar.maximumFirstRow);
    ASSERT_EQ(view.firstVisualRow + view.visibleRows.size(),
              static_cast<std::size_t>(view.totalVisualRows));
    std::filesystem::remove_all(root);
}

// A document that fits the terminal but NOT the smaller region the editor
// actually paints is still clipped, so it must still report a scrollbar.  Sized
// against the terminal height instead, the scrollbar silently disappears for
// exactly the documents that most need one.
TEST(aDocumentClippedByTheChromeStillReportsAScrollbar) {
    auto root = uniqueRoot("viewport_scrollbar");
    const ssg::ViewportDimensions dims{80, 24};
    {
        // Fewer lines than the terminal has rows, more than the pane paints.
        std::ofstream file{root / "workspace" / "snug.txt"};
        for (int line = 1; line <= 23; ++line) file << "line " << line << "\n";
    }
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"snug.txt"}).accepted());
    runtime.focusEditor();
    ssg::test::GridTestView grid{dims};
    auto snapshot = grid.present(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& view = snapshot->viewport;
    ASSERT_TRUE(view.totalVisualRows > view.visibleRows.size());
    ASSERT_TRUE(view.scrollbar.maximumFirstRow > 0);
    std::filesystem::remove_all(root);
}

// Every session opens on an empty untitled buffer.  Reporting it unsaved is
// technically true and practically useless: the badge appears before the user
// has done anything, so it stops meaning "you have work to lose".
TEST(anEmptyBlankBufferIsNotUnsavedUntilItHasContent) {
    auto root = uniqueRoot("blank_dirty");
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch("file.new").accepted());
    runtime.focusEditor();

    auto const dirty = [&] {
        auto snapshot = ssg::test::projectGridFrame(runtime);
        auto const& tabs = snapshot->tabs.tabs;
        return !tabs.empty() && tabs.front().dirty;
    };
    ASSERT_FALSE(dirty());
    ASSERT_TRUE(ssg::test::typeText(runtime, "x").accepted());
    ASSERT_TRUE(dirty());
    std::filesystem::remove_all(root);
}

// Opening a file beside the startup blank buffer would otherwise leave a blank
// tab nobody asked for.  It is discarded only when it is the sole tab, untitled,
// and empty -- a buffer with content, or one kept beside others, is never taken.
TEST(openingAFileDiscardsOnlyAnEmptySoleBlankTab) {
    auto root = uniqueRoot("blank_close");
    std::ofstream{root / "workspace" / "alpha.txt"} << "alpha\n";
    std::ofstream{root / "workspace" / "beta.txt"} << "beta\n";
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    auto const tabLabels = [&] {
        auto snapshot = ssg::test::projectGridFrame(runtime);
        std::vector<std::string> labels;
        for (auto const& tab : snapshot->tabs.tabs) {
            labels.push_back(tab.label);
        }
        return labels;
    };
    // Assert on the labels rather than a count: this suite's uniqueRoot places
    // the workspace under the current directory, so a runtime here can pick up a
    // tab from a source file lying around.  Counting tabs would make the test
    // depend on what else the build tree happens to contain.
    auto const hasTab = [&](std::string_view label) {
        auto const labels = tabLabels();
        return std::find(labels.begin(), labels.end(), label) != labels.end();
    };

    ASSERT_TRUE(runtime.dispatch("file.new").accepted());
    ASSERT_TRUE(hasTab("[new buffer]"));
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"alpha.txt"}).accepted());
    // The blank tab went with it rather than lingering blank, and the file --
    // not the blank buffer -- is what remains.
    ASSERT_TRUE(hasTab("alpha.txt"));
    ASSERT_FALSE(hasTab("[new buffer]"));

    // A second open leaves the file already there alone: only the STARTUP
    // blank buffer is disposable, never a real document.  Asserted by opening
    // beta while alpha is the sole tab, which is exactly the shape that would
    // trip a rule checking only "is this untitled and empty".
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"beta.txt"}).accepted());
    ASSERT_TRUE(hasTab("alpha.txt"));
    ASSERT_TRUE(hasTab("beta.txt"));

    // And an EMPTY file is still a file: opening another beside it must not
    // discard it just because it holds no text.
    std::ofstream{root / "workspace" / "empty.txt"};
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"empty.txt"}).accepted());
    ASSERT_TRUE(hasTab("empty.txt"));
    ASSERT_TRUE(hasTab("alpha.txt"));
    ASSERT_TRUE(hasTab("beta.txt"));

    // Only the SOLE tab is disposable.  An empty blank buffer sitting beside
    // other tabs was opened deliberately -- the user asked for it with file.new
    // rather than being handed it at startup -- so it stays.  The untitled and
    // empty checks alone would not preserve it; this is what makes the rule "the
    // startup buffer" rather than "any blank buffer".
    ASSERT_TRUE(runtime.dispatch("file.new").accepted());
    ASSERT_TRUE(hasTab("[new buffer]"));
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"alpha.txt"}).accepted());
    ASSERT_TRUE(hasTab("[new buffer]"));
    std::filesystem::remove_all(root);
}

// A blank buffer the user has typed into holds work, so opening a file beside
// it must keep it.  This is the assertion that makes the feature safe rather
// than merely tidy.
TEST(aBlankBufferWithContentSurvivesOpeningAFile) {
    auto root = uniqueRoot("blank_keep");
    std::ofstream{root / "workspace" / "alpha.txt"} << "alpha\n";
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch("file.new").accepted());
    runtime.focusEditor();
    ASSERT_TRUE(ssg::test::typeText(runtime, "unsaved work").accepted());
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"alpha.txt"}).accepted());
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    bool keptBlank = false;
    bool openedFile = false;
    for (auto const& tab : snapshot->tabs.tabs) {
        if (tab.label == "[new buffer]") keptBlank = true;
        if (tab.label == "alpha.txt") openedFile = true;
    }
    ASSERT_TRUE(keptBlank);
    ASSERT_TRUE(openedFile);
    std::filesystem::remove_all(root);
}

// An EMPTY FILE is still a file.  The disposal rule turns on "untitled", not on
// "has no text", so a real but empty document opened as the sole tab must
// survive opening another beside it.
TEST(anEmptySavedFileIsNeverDiscardedAsBlank) {
    auto root = uniqueRoot("blank_empty_file");
    std::ofstream{root / "workspace" / "blank.txt"};
    std::ofstream{root / "workspace" / "alpha.txt"} << "alpha\n";
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"blank.txt"}).accepted());
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"alpha.txt"}).accepted());
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    bool keptBlank = false;
    for (auto const& tab : snapshot->tabs.tabs) {
        if (tab.label == "blank.txt") keptBlank = true;
    }
    ASSERT_TRUE(keptBlank);
    std::filesystem::remove_all(root);
}

// The same argument as the rows above, in the other axis.  With the sidebar
// open the editor paints a NARROWER region than the client surface, so wrap
// must break against the pane width.  Given the full surface width instead, a
// line that overflows the pane reports as one visual row and its tail is
// painted nowhere.
TEST(wrapBreaksAgainstThePaneWidthNotTheClientSurface) {
    auto root = uniqueRoot("viewport_width");
    const ssg::ViewportDimensions dims{80, 24};
    {
        // One line wider than any plausible pane beside a sidebar, but well
        // inside the 80-column surface.
        std::ofstream file{root / "workspace" / "wide.txt"};
        file << std::string(70, 'a');
    }
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"wide.txt"}).accepted());
    runtime.focusEditor();
    ASSERT_TRUE(runtime.dispatch("view.toggle_word_wrap")
                    .accepted());
    ASSERT_TRUE(runtime.dispatch("panel.show_files")
                    .accepted());
    ssg::test::GridTestView grid{dims};
    (void)grid.present(runtime);
    auto snapshot = grid.present(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& view = snapshot->viewport;
    ASSERT_TRUE(view.totalVisualRows > 1);
    std::filesystem::remove_all(root);
}

TEST(addCursorChordProducesMultipleSelections) {
    auto root = uniqueRoot("multi_cursor");
    std::ofstream{root / "workspace" / "m.txt"} << "alpha\nbeta\n";
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"m.txt"}).accepted());

    // Drive the curated binding through the real input path.
    const auto chord = *ssg::parseKeySequence({"Mod+KeyJ"});
    const auto input = runtime.input({ssg::ClientKeyInput{chord.front(), {}}});
    ASSERT_EQ(input.outcome, ssg::ClientInputOutcome::Dispatched);

    auto after = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->selections.items().size() > std::size_t{1});
}

TEST(settingsOpenFocusesASettingsPrompt) {
    auto root = uniqueRoot("settings_open");
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch("settings.open").accepted());
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The chord actually opens: focus moves to the prompt with a visible input.
    ASSERT_EQ(ssg::effectiveUiFocus(snapshot->uiTree),
              ssg::FocusTarget::Prompt);
}

TEST(gridPresenterOwnsScrollAndRejectsAReusedFrameBasis) {
    auto root = uniqueRoot("grid_presenter_scroll");
    {
        std::ofstream file{root / "workspace" / "long.txt"};
        for (int line = 0; line < 80; ++line) file << "line\n";
    }

    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"long.txt"})
                    .accepted());

    ssg::GridPresenter presenter{};
    auto frame = presenter.project(runtime, {{80, 12}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const ssg::ViewAction noOpAction =
        ssg::ScrollLines{ssg::ScrollTarget::Document, -1};
    auto noOp = presenter.apply(noOpAction, *frame);
    ASSERT_TRUE(noOp.accepted());
    ASSERT_FALSE(presenter.apply(noOpAction, *frame).accepted());
    frame = presenter.project(runtime, {{80, 12}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;

    const ssg::ViewAction scrollAction =
        ssg::ScrollLines{ssg::ScrollTarget::Document, 5};
    auto applied = presenter.apply(scrollAction, *frame);
    ASSERT_TRUE(applied.accepted());
    auto stale = presenter.apply(scrollAction, *frame);
    ASSERT_FALSE(stale.accepted());

    auto scrolled = presenter.project(runtime, {{80, 12}, {}});
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    ASSERT_EQ(scrolled->viewport.firstVisualRow, 5U);

    const auto selectionBefore = ssg::test::projectGridFrame(runtime)->selections;
    auto visual = runtime.dispatch("cursor.page_down");
    ASSERT_TRUE(visual.accepted());
    ASSERT_TRUE(visual.viewAction.has_value());
    auto selectionAfter = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(selectionAfter.has_value());
    if (selectionAfter) {
        ASSERT_EQ(selectionAfter->selections, selectionBefore);
    }
}

TEST(sessionPaneTopologyRespondsToCommandsAndViewActions) {
    auto root = uniqueRoot("grid_presenter_panes");
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ssg::GridPresenter presenter{};
    auto frame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_TRUE(frame->document.has_value());
    if (!frame->document) return;
    ASSERT_EQ(frame->document->panes.size(), std::size_t{1});

    auto split = runtime.dispatch("pane.split_horizontal");
    ASSERT_TRUE(split.completed());
    ASSERT_FALSE(split.viewAction.has_value());

    frame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_TRUE(frame->document.has_value());
    if (!frame->document) return;
    ASSERT_EQ(frame->document->panes.size(), std::size_t{2});
    auto blockedFocus = runtime.dispatch("pane.focus_down");
    ASSERT_TRUE(blockedFocus.viewAction.has_value());
    if (!blockedFocus.viewAction) return;
    auto blockedApplied = presenter.apply(*blockedFocus.viewAction, *frame);
    ASSERT_TRUE(blockedApplied.accepted());
    ASSERT_FALSE(blockedApplied.transition.has_value());

    ASSERT_TRUE(runtime
                    .dispatch("panel.show_files")
                    .accepted());
    frame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto focused = runtime.dispatch("pane.focus_up");
    ASSERT_TRUE(focused.viewAction.has_value());
    if (!focused.viewAction) return;
    auto focusedApplied = presenter.apply(*focused.viewAction, *frame);
    ASSERT_TRUE(focusedApplied.accepted());
    ASSERT_TRUE(
        focusedApplied.transition &&
        std::holds_alternative<ssg::PaneFocusTransition>(
            focusedApplied.transition->transition));
    if (!focusedApplied.transition) return;
    const auto* focusedPane =
        std::get_if<ssg::PaneFocusTransition>(
            &focusedApplied.transition->transition);
    ASSERT_TRUE(focusedPane != nullptr);
    if (!focusedPane) return;
    ASSERT_EQ(focusedPane->pane, ssg::PaneId{1});
    const auto focusResult =
        runtime.input(*focusedApplied.transition);
    ASSERT_EQ(focusResult.outcome, ssg::ClientInputOutcome::Dispatched);
    auto focusedSnapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(focusedSnapshot.has_value());
    if (!focusedSnapshot) return;
    ASSERT_EQ(ssg::effectiveUiFocus(focusedSnapshot->uiTree),
              ssg::FocusTarget::Editor);
    ASSERT_EQ(focusedSnapshot->followMode,
              ssg::FollowMode::Paused);

    frame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto focusedAgain = runtime.dispatch("pane.focus_down");
    ASSERT_TRUE(focusedAgain.viewAction.has_value());
    if (!focusedAgain.viewAction) return;
    auto focusedAgainApplied =
        presenter.apply(*focusedAgain.viewAction, *frame);
    ASSERT_TRUE(focusedAgainApplied.accepted());
    ASSERT_TRUE(
        focusedAgainApplied.transition &&
        std::holds_alternative<ssg::PaneFocusTransition>(
            focusedAgainApplied.transition->transition));
    if (!focusedAgainApplied.transition) return;
    const auto* focusedAgainPane =
        std::get_if<ssg::PaneFocusTransition>(
            &focusedAgainApplied.transition->transition);
    ASSERT_TRUE(focusedAgainPane != nullptr);
    if (!focusedAgainPane) return;
    ASSERT_EQ(focusedAgainPane->pane, ssg::PaneId{2});
    const auto repeatedFocusResult =
        runtime.input(*focusedAgainApplied.transition);
    ASSERT_EQ(repeatedFocusResult.outcome,
              ssg::ClientInputOutcome::Dispatched);
    auto repeatedFocusSnapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(repeatedFocusSnapshot.has_value());
    if (!repeatedFocusSnapshot) return;

    frame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto cycled =
        runtime.dispatch("pane.next");
    ASSERT_TRUE(cycled.completed());
    ASSERT_FALSE(cycled.viewAction.has_value());
    auto cycledFrame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(cycledFrame && cycledFrame->document);
    if (!cycledFrame || !cycledFrame->document) return;
    ASSERT_EQ(cycledFrame->document
                  ->panes[cycledFrame->document->activePaneIndex]
                  .id,
              ssg::PaneId{1});

    frame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto closed =
        runtime.dispatch("pane.close");
    ASSERT_TRUE(closed.completed());
    ASSERT_FALSE(closed.viewAction.has_value());

    auto closedFrame = presenter.project(runtime, {{80, 24}, {}});
    ASSERT_TRUE(closedFrame.has_value());
    if (closedFrame) {
        ASSERT_TRUE(closedFrame->document.has_value());
        if (closedFrame->document) {
            ASSERT_EQ(closedFrame->document->panes.size(), std::size_t{1});
        }
    }
}

TEST(visualLineMovementRequiresPresenterResolution) {
    auto root = uniqueRoot("grid_presenter_visual_selection");
    {
        std::ofstream lines{root / "workspace" / "lines.txt"};
        for (int line = 0; line < 40; ++line) {
            lines << "line " << line << '\n';
        }
    }
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"lines.txt"})
                    .accepted());

    auto moved = runtime.dispatch("cursor.line_down");
    ASSERT_TRUE(moved.accepted());
    ASSERT_TRUE(moved.viewAction.has_value());
    if (!moved.viewAction) return;
    const auto expectedMove = ssg::ViewAction{
        ssg::MoveVisualSelection{
            ssg::VisualSelectionDirection::LineDown, false}};
    ASSERT_EQ(*moved.viewAction, expectedMove);

    ssg::GridPresenter presenter{};
    auto frame = presenter.project(runtime, {{80, 12}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto applied = presenter.apply(*moved.viewAction, *frame);
    ASSERT_TRUE(applied.accepted());
    ASSERT_TRUE(
        applied.transition &&
        std::holds_alternative<ssg::SelectionTransition>(
            applied.transition->transition));
    const auto submitted = runtime.input(*applied.transition);
    ASSERT_EQ(submitted.outcome, ssg::ClientInputOutcome::Dispatched);
    auto confirmed = presenter.project(runtime, {{80, 12}, {}});
    ASSERT_TRUE(confirmed.has_value());
    if (!confirmed) return;
    ASSERT_EQ(confirmed->selections.primary().active.line,
              ssg::LineIndex{1});

    const struct {
        const char* command;
        ssg::VisualSelectionDirection direction;
        bool extend;
    } movements[] = {
        {"cursor.page_down", ssg::VisualSelectionDirection::PageDown, false},
        {"cursor.page_up", ssg::VisualSelectionDirection::PageUp, false},
        {"select.line_up", ssg::VisualSelectionDirection::LineUp, true},
        {"select.line_down", ssg::VisualSelectionDirection::LineDown, true},
        {"select.page_down", ssg::VisualSelectionDirection::PageDown, true},
        {"select.page_up", ssg::VisualSelectionDirection::PageUp, true},
        {"cursor.line_up", ssg::VisualSelectionDirection::LineUp, false},
    };
    for (const auto& movement : movements) {
        const auto before = confirmed->selections;
        auto command = runtime.dispatch(movement.command);
        ASSERT_TRUE(command.accepted());
        ASSERT_TRUE(command.viewAction.has_value());
        if (!command.viewAction) return;
        const auto expected = ssg::ViewAction{
            ssg::MoveVisualSelection{
                movement.direction, movement.extend}};
        ASSERT_EQ(*command.viewAction, expected);
        auto result = presenter.apply(*command.viewAction, *confirmed);
        ASSERT_TRUE(result.transition.has_value());
        if (!result.transition) return;
        ASSERT_EQ(runtime.input(*result.transition).outcome,
                  ssg::ClientInputOutcome::Dispatched);
        confirmed = presenter.project(runtime, {{80, 12}, {}});
        ASSERT_TRUE(confirmed.has_value());
        if (!confirmed) return;
        ASSERT_NE(confirmed->selections, before);
    }
}

TEST(visualMovementUsesActivePaneAcrossSerializedInput) {
    auto root = uniqueRoot("grid_presenter_active_pane_selection");
    {
        std::ofstream lines{root / "workspace" / "lines.txt"};
        for (int line = 0; line < 60; ++line) {
            lines << "line " << line << '\n';
        }
    }
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"lines.txt"})
                    .accepted());

    ssg::GridPresenter presenter{};
    auto frame = presenter.project(runtime, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto split = runtime.dispatch("pane.split_horizontal");
    ASSERT_TRUE(split.completed());
    frame = presenter.project(runtime, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame || !frame->document ||
        frame->document->panes.size() != 2) {
        return;
    }
    const auto activeRows = static_cast<std::uint32_t>(
        frame->document->panes.back().content.height);

    auto page = runtime.dispatch("cursor.page_down");
    ASSERT_TRUE(page.viewAction.has_value());
    if (!page.viewAction) return;
    auto proposed = presenter.apply(*page.viewAction, *frame);
    ASSERT_TRUE(proposed.transition.has_value());
    if (!proposed.transition) return;
    ASSERT_EQ(runtime.input(*proposed.transition).outcome,
              ssg::ClientInputOutcome::Dispatched);
    frame = presenter.project(runtime, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->selections.primary().active.line.value(),
              activeRows);

    auto next = runtime.dispatch("cursor.line_down");
    ASSERT_TRUE(next.viewAction.has_value());
    if (!next.viewAction) return;
    auto proposal = presenter.apply(*next.viewAction, *frame);
    ASSERT_TRUE(proposal.transition.has_value());
    if (!proposal.transition) return;
    ASSERT_EQ(
        runtime
            .input(ssg::ViewTransitionInput{
                       ssg::PaneFocusTransition{ssg::PaneId{1}}})
            .outcome,
        ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(runtime.input(*proposal.transition).outcome,
              ssg::ClientInputOutcome::Dispatched);
    frame = presenter.project(runtime, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->selections.primary().active.line.value(),
              activeRows + 1);
}

} // namespace

SSG_TEST_SUITE(test_session_presentation) {
    RUN(constructionRejectsInvalidCwd);
    RUN(sessionProjectionResolvesStatusFieldsAndHint);
    RUN(runtimeSourcesDoNotIncludeFixtureModel);
    RUN(defaultTerminalKeymapIsValid);
    RUN(defaultTerminalKeymapBindingsAreSingleStroke);
    RUN(curatedKeymapBindingsAreArgumentFree);
    RUN(curatedKeymapResolvesPerContext);
    RUN(everyDocumentLineIsReachableAndTheCaretIsNeverLost);
    RUN(aDocumentClippedByTheChromeStillReportsAScrollbar);
    RUN(anEmptyBlankBufferIsNotUnsavedUntilItHasContent);
    RUN(openingAFileDiscardsOnlyAnEmptySoleBlankTab);
    RUN(aBlankBufferWithContentSurvivesOpeningAFile);
    RUN(anEmptySavedFileIsNeverDiscardedAsBlank);
    RUN(wrapBreaksAgainstThePaneWidthNotTheClientSurface);
    RUN(addCursorChordProducesMultipleSelections);
    RUN(settingsOpenFocusesASettingsPrompt);
    RUN(gridPresenterOwnsScrollAndRejectsAReusedFrameBasis);
    RUN(sessionPaneTopologyRespondsToCommandsAndViewActions);
    RUN(visualLineMovementRequiresPresenterResolution);
    RUN(visualMovementUsesActivePaneAcrossSerializedInput);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
