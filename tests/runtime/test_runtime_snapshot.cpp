#include "command_cases.h"
#include "../test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/EditorSessionBuilder.h>
#include <ssg/Keymap.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef SSG_SOURCE_SCAN_ROOT
#error "SSG_SOURCE_SCAN_ROOT must name the source tree"
#endif

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_snapshot_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorRuntimeConfig configFor(const std::filesystem::path& root) {
    return {root / "workspace", root / "scratch", root / "recovery"};
}

TEST(constructionRejectsInvalidCwd) {
    auto root = uniqueRoot("invalid_cwd");
    auto result = ssg::EditorRuntime::create(configFor(root / "missing"));
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.message.empty());
}

// commandCaseTableExactlyMatchesP0Catalog is deleted.  It proved the test
// case table listed exactly the catalog's commands, back when the table was
// typed out by hand.  The cases are now derived from the running editor, so it
// compared a thing to itself.

TEST(runtimeConstructsAttachesAndProducesLiveSnapshot) {
    auto root = uniqueRoot("snapshot");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;

    auto& runtime = *created.runtime;
    ssg::InvocationPrincipal principal{ssg::ClientId{7}, ssg::InvocationOrigin::InProcess};
    ASSERT_TRUE(runtime.attach(std::move(principal), ssg::ViewId{9}).accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{7}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->revision(), runtime.revision());
    ASSERT_EQ(snapshot->client().clientId, ssg::ClientId{7});
    ASSERT_EQ(snapshot->client().viewId, ssg::ViewId{9});
}

TEST(runtimeSourcesDoNotIncludeFixtureModel) {
    auto root = std::filesystem::path{SSG_SOURCE_SCAN_ROOT};
    bool found = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "src"}) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.rfind("src/runtime/", 0) != 0 && relative != "src/EditorRuntime.cpp") continue;
        std::ifstream input{entry.path()};
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        found = found || text.find("FixtureModel") != std::string::npos;
    }
    ASSERT_FALSE(found);
}

TEST(runtimePublishesValidCuratedKeymap) {
    auto root = uniqueRoot("keymap_valid");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& keymap = snapshot->sections().keymap;
    ASSERT_FALSE(keymap.bindings.empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.validate({}).empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.hasGlobalBinding("settings.open", {}));
}

TEST(everyDocumentLineIsReachableAndTheCaretIsNeverLost) {
    auto root = uniqueRoot("viewport_reach");
    // More lines than fit, so the document genuinely scrolls.
    {
        std::ofstream file{root / "workspace" / "long.txt"};
        for (int line = 1; line <= 60; ++line) file << "line " << line << "\n";
    }
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"long.txt"}}).accepted());
    runtime.focusEditor();

    const ssg::ViewportDimensions dims{80, 24};
    // Walk the caret to the very last line.  The viewport must scroll against
    // the rows the editor PAINTS, not the terminal height: sized to the whole
    // terminal it stops short by the header, tab bar and footer, and the final
    // lines can never be shown.  The visible symptom is a caret the renderer
    // cannot place -- in a terminal the hardware cursor then stays wherever
    // painting ended, which is the footer.
    std::uint32_t lastVisibleLine = 0;
    for (int step = 0; step < 80; ++step) {
        auto snapshot = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        auto const& view = snapshot->presentation()->viewport;
        auto const caretLine =
            snapshot->sections().selection.selections.primary().active.line.value();
        // The caret's line is always inside the window that is actually painted.
        ASSERT_TRUE(caretLine >= view.firstVisualRow);
        ASSERT_TRUE(caretLine < view.firstVisualRow + view.visibleRows.size());
        lastVisibleLine = std::max<std::uint32_t>(lastVisibleLine, caretLine);
        (void)runtime.dispatch(ssg::ClientId{1},
                               {"cursor.line_down", runtime.revision(), {}});
    }
    // The last line of the document was reached, not merely approached.
    auto final = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(final.has_value());
    if (!final) return;
    ASSERT_EQ(lastVisibleLine, final->presentation()->viewport.totalVisualRows - 1);

    // Scrolling to the maximum offset shows the final line, so no row is
    // stranded past the end of the scroll range.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"view.scroll_lines", runtime.revision(),
                                  ssg::ScrollLinesArguments{500}}).accepted());
    auto bottom = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(bottom.has_value());
    if (!bottom) return;
    auto const& view = bottom->presentation()->viewport;
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
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"snug.txt"}}).accepted());
    runtime.focusEditor();
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& view = snapshot->presentation()->viewport;
    ASSERT_TRUE(view.totalVisualRows > view.visibleRows.size());
    ASSERT_TRUE(view.scrollbar.maximumFirstRow > 0);
    std::filesystem::remove_all(root);
}

// Every session opens on an empty untitled buffer.  Reporting it unsaved is
// technically true and practically useless: the badge appears before the user
// has done anything, so it stops meaning "you have work to lose".
TEST(anEmptyScratchBufferIsNotUnsavedUntilItHasContent) {
    auto root = uniqueRoot("scratch_dirty");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.new", runtime.revision(), {}}).accepted());
    runtime.focusEditor();

    auto const dirty = [&] {
        auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
        auto const& tabs = snapshot->sections().tabs.tabs;
        return !tabs.empty() && tabs.front().dirty;
    };
    ASSERT_FALSE(dirty());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"x"}}).accepted());
    ASSERT_TRUE(dirty());
    std::filesystem::remove_all(root);
}

// Opening a file beside the startup scratch buffer would otherwise leave a blank
// tab nobody asked for.  It is discarded only when it is the sole tab, untitled,
// and empty -- a buffer with content, or one kept beside others, is never taken.
TEST(openingAFileDiscardsOnlyAnEmptySoleScratchTab) {
    auto root = uniqueRoot("scratch_close");
    std::ofstream{root / "workspace" / "alpha.txt"} << "alpha\n";
    std::ofstream{root / "workspace" / "beta.txt"} << "beta\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    auto const tabLabels = [&] {
        auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
        std::vector<std::string> labels;
        for (auto const& tab : snapshot->sections().tabs.tabs) {
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

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.new", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(hasTab("[new buffer]"));
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"alpha.txt"}}).accepted());
    // The scratch tab went with it rather than lingering blank, and the file --
    // not the scratch buffer -- is what remains.
    ASSERT_TRUE(hasTab("alpha.txt"));
    ASSERT_FALSE(hasTab("[new buffer]"));

    // A second open leaves the file already there alone: only the STARTUP
    // scratch buffer is disposable, never a real document.  Asserted by opening
    // beta while alpha is the sole tab, which is exactly the shape that would
    // trip a rule checking only "is this untitled and empty".
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"beta.txt"}}).accepted());
    ASSERT_TRUE(hasTab("alpha.txt"));
    ASSERT_TRUE(hasTab("beta.txt"));

    // And an EMPTY file is still a file: opening another beside it must not
    // discard it just because it holds no text.
    std::ofstream{root / "workspace" / "empty.txt"};
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"empty.txt"}}).accepted());
    ASSERT_TRUE(hasTab("empty.txt"));
    ASSERT_TRUE(hasTab("alpha.txt"));
    ASSERT_TRUE(hasTab("beta.txt"));

    // Only the SOLE tab is disposable.  An empty scratch buffer sitting beside
    // other tabs was opened deliberately -- the user asked for it with file.new
    // rather than being handed it at startup -- so it stays.  The untitled and
    // empty checks alone would not preserve it; this is what makes the rule "the
    // startup buffer" rather than "any blank buffer".
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.new", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(hasTab("[new buffer]"));
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"alpha.txt"}}).accepted());
    ASSERT_TRUE(hasTab("[new buffer]"));
    std::filesystem::remove_all(root);
}

// A scratch buffer the user has typed into holds work, so opening a file beside
// it must keep it.  This is the assertion that makes the feature safe rather
// than merely tidy.
TEST(aScratchBufferWithContentSurvivesOpeningAFile) {
    auto root = uniqueRoot("scratch_keep");
    std::ofstream{root / "workspace" / "alpha.txt"} << "alpha\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.new", runtime.revision(), {}}).accepted());
    runtime.focusEditor();
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"unsaved work"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"alpha.txt"}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    bool keptScratch = false;
    bool openedFile = false;
    for (auto const& tab : snapshot->sections().tabs.tabs) {
        if (tab.label == "[new buffer]") keptScratch = true;
        if (tab.label == "alpha.txt") openedFile = true;
    }
    ASSERT_TRUE(keptScratch);
    ASSERT_TRUE(openedFile);
    std::filesystem::remove_all(root);
}

// An EMPTY FILE is still a file.  The disposal rule turns on "untitled", not on
// "has no text", so a real but empty document opened as the sole tab must
// survive opening another beside it.
TEST(anEmptySavedFileIsNeverDiscardedAsScratch) {
    auto root = uniqueRoot("scratch_empty_file");
    std::ofstream{root / "workspace" / "blank.txt"};
    std::ofstream{root / "workspace" / "alpha.txt"} << "alpha\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"blank.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"alpha.txt"}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    bool keptBlank = false;
    for (auto const& tab : snapshot->sections().tabs.tabs) {
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
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"wide.txt"}}).accepted());
    runtime.focusEditor();
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"view.toggle_word_wrap", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    // Prime the pane-size cache, then read the frame laid out against it.
    (void)runtime.snapshot(ssg::ClientId{1}, dims);
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& view = snapshot->presentation()->viewport;
    ASSERT_TRUE(view.totalVisualRows > 1);
    std::filesystem::remove_all(root);
}

TEST(curatedKeymapBindingsAreArgumentFree) {
    auto root = uniqueRoot("keymap_argfree");
    std::ofstream{root / "workspace" / "doc.txt"} << "alpha\nbeta\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    (void)runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"doc.txt"}});
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    // Every bound command must dispatch with an empty payload without failing
    // for a missing/mistyped argument.  Benign state failures (e.g. a prompt
    // command with no open prompt) are allowed; an argument-shaped failure is
    // not.
    std::set<std::string> commands;
    for (const auto& binding : snapshot->sections().keymap.bindings) {
        commands.insert(binding.commandId);
    }
    for (const auto& command : commands) {
        auto result = runtime.dispatch(ssg::ClientId{1}, {command, runtime.revision(), {}});
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
}

TEST(curatedKeymapResolvesPerContext) {
    auto root = uniqueRoot("keymap_resolve");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& keymap = snapshot->sections().keymap;

    const auto down = *ssg::KeyCodec{}.parseSequence({"ArrowDown"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "editor").commandId,
              std::string{"cursor.line_down"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "panel").commandId,
              std::string{"tree.select_next"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "prompt").commandId,
              std::string{"prompt.next"});

    const auto save = *ssg::KeyCodec{}.parseSequence({"Alt+KeyS"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(save, "editor").commandId,
              std::string{"file.save"});

    // Alt+Home/End are additional editor-context bindings for the document
    // extremes, alongside the existing Ctrl+Home/End.
    const auto altHome = *ssg::KeyCodec{}.parseSequence({"Alt+Home"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altHome, "editor").commandId,
              std::string{"cursor.document_start"});
    const auto altEnd = *ssg::KeyCodec{}.parseSequence({"Alt+End"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altEnd, "editor").commandId,
              std::string{"cursor.document_end"});

    // The settings.open escape hatch resolves in every context.
    const auto settings = *ssg::KeyCodec{}.parseSequence({"Alt+Shift+KeyT"});
    for (const auto context : {"editor", "panel", "prompt"}) {
        ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(settings, context).commandId,
                  std::string{"settings.open"});
    }

    // Cut/copy/paste are editor-context Alt chords; paste is additionally bound
    // in the prompt.
    struct ClipboardBinding {
        char const* key;
        char const* command;
    };
    for (auto const& binding : {ClipboardBinding{"Alt+KeyX", "clipboard.cut"},
                                ClipboardBinding{"Alt+KeyC", "clipboard.copy"},
                                ClipboardBinding{"Alt+KeyV", "clipboard.paste"}}) {
        const auto sequence = *ssg::KeyCodec{}.parseSequence({binding.key});
        ASSERT_EQ(
            ssg::KeymapMatcher{keymap}.resolveSequence(sequence, "editor").commandId,
            std::string{binding.command});
    }

    // Editor-scoped means editor-ONLY for cut and copy.
    for (auto const* const key : {"Alt+KeyX", "Alt+KeyC"}) {
        const auto sequence = *ssg::KeyCodec{}.parseSequence({key});
        for (auto const* const context : {"panel", "prompt"}) {
            const auto resolved =
                ssg::KeymapMatcher{keymap}.resolveSequence(sequence, context);
            ASSERT_TRUE(resolved.commandId.rfind("clipboard.", 0) != 0);
        }
    }
    // Paste is the exception: it is bound in the prompt too, because a prompt
    // owns text a user will want to paste into.  It is fulfilled against the
    // prompt's own value client-side, never dispatched at the document.
    const auto paste = *ssg::KeyCodec{}.parseSequence({"Alt+KeyV"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(paste, "prompt").commandId,
              std::string{"clipboard.paste"});
    // But not in the panel: there is no text there to paste into.
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}
                    .resolveSequence(paste, "panel")
                    .commandId.rfind("clipboard.", 0) != 0);

    // A single Escape cancels a focused prompt in one press.
    const auto escape = *ssg::KeyCodec{}.parseSequence({"Escape"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(escape, "prompt").commandId,
              std::string{"prompt.cancel"});

    // M7-M selection/multi-cursor bindings: Shift+Arrow extends the selection in
    // the editor; the multi-cursor chords resolve globally.
    const auto shiftRight = *ssg::KeyCodec{}.parseSequence({"Shift+ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(shiftRight, "editor").commandId,
              std::string{"select.right"});
    const auto shiftUp = *ssg::KeyCodec{}.parseSequence({"Shift+ArrowUp"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(shiftUp, "editor").commandId,
              std::string{"select.line_up"});
    // Plain ArrowRight is still cursor motion, distinct from the shifted stroke.
    const auto plainRight = *ssg::KeyCodec{}.parseSequence({"ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(plainRight, "editor").commandId,
              std::string{"cursor.right"});
    const auto addNext = *ssg::KeyCodec{}.parseSequence({"Alt+KeyD"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(addNext, "editor").commandId,
              std::string{"select.add_next_occurrence"});
    const auto findOpen = *ssg::KeyCodec{}.parseSequence({"Alt+Slash"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(findOpen, "editor").commandId,
              std::string{"find.open"});
    // Alt+8 seeds find with the word under the caret, in the editor context.
    const auto findWord = *ssg::KeyCodec{}.parseSequence({"Alt+Digit8"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(findWord, "editor").commandId,
              std::string{"find.word_under_cursor"});
    const auto replaceOpen = *ssg::KeyCodec{}.parseSequence({"Alt+KeyR"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(replaceOpen, "editor").commandId,
              std::string{"replace.open"});
    // Tab cycling moved off the brackets to Alt+Period/Comma.
    const auto tabNext = *ssg::KeyCodec{}.parseSequence({"Alt+Period"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(tabNext, "editor").commandId,
              std::string{"tab.next"});
    const auto tabPrev = *ssg::KeyCodec{}.parseSequence({"Alt+Comma"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(tabPrev, "editor").commandId,
              std::string{"tab.previous"});

    // Delete forward and Alt word navigation (with Shift for select).
    const auto del = *ssg::KeyCodec{}.parseSequence({"Delete"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(del, "editor").commandId,
              std::string{"text.delete_forward"});
    // Alt+Backspace deletes the word to the left.
    const auto deleteWord = *ssg::KeyCodec{}.parseSequence({"Alt+Backspace"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(deleteWord, "editor").commandId,
              std::string{"text.delete_word_backward"});
    const auto altWordLeft = *ssg::KeyCodec{}.parseSequence({"Alt+ArrowLeft"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altWordLeft, "editor").commandId,
              std::string{"cursor.word_left"});
    const auto altWordRight = *ssg::KeyCodec{}.parseSequence({"Alt+ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altWordRight, "editor").commandId,
              std::string{"cursor.word_right"});
    const auto altSelectWordLeft =
        *ssg::KeyCodec{}.parseSequence({"Alt+Shift+ArrowLeft"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altSelectWordLeft, "editor").commandId,
              std::string{"select.word_left"});
    const auto altSelectWordRight =
        *ssg::KeyCodec{}.parseSequence({"Alt+Shift+ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(altSelectWordRight, "editor").commandId,
              std::string{"select.word_right"});
}

TEST(addCursorChordProducesMultipleSelections) {
    auto root = uniqueRoot("multi_cursor");
    std::ofstream{root / "workspace" / "m.txt"} << "alpha\nbeta\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());

    // Resolve the add-cursor-down chord from the published keymap, then dispatch
    // the resolved command: the snapshot must show more than one selection.
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto chord = *ssg::KeyCodec{}.parseSequence({"Alt+KeyJ"});
    auto resolved = ssg::KeymapMatcher{snapshot->sections().keymap}.resolveSequence(chord, "editor");
    ASSERT_EQ(resolved.kind, ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(resolved.commandId, std::string{"select.add_cursor_down"});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {resolved.commandId, runtime.revision(), {}}).accepted());

    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->sections().selection.selections.items().size() > std::size_t{1});
}

TEST(settingsOpenFocusesASettingsPrompt) {
    auto root = uniqueRoot("settings_open");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The chord actually opens: focus moves to the prompt with a visible input.
    ASSERT_EQ(snapshot->sections().focus, ssg::FocusTarget::Prompt);
    ASSERT_TRUE(snapshot->presentation()->prompt.has_value());
}

} // namespace

int main() {
    RUN(constructionRejectsInvalidCwd);
    RUN(runtimeConstructsAttachesAndProducesLiveSnapshot);
    RUN(runtimeSourcesDoNotIncludeFixtureModel);
    RUN(runtimePublishesValidCuratedKeymap);
    RUN(everyDocumentLineIsReachableAndTheCaretIsNeverLost);
    RUN(aDocumentClippedByTheChromeStillReportsAScrollbar);
    RUN(anEmptyScratchBufferIsNotUnsavedUntilItHasContent);
    RUN(openingAFileDiscardsOnlyAnEmptySoleScratchTab);
    RUN(aScratchBufferWithContentSurvivesOpeningAFile);
    RUN(anEmptySavedFileIsNeverDiscardedAsScratch);
    RUN(wrapBreaksAgainstThePaneWidthNotTheClientSurface);
    RUN(curatedKeymapBindingsAreArgumentFree);
    RUN(curatedKeymapResolvesPerContext);
    RUN(addCursorChordProducesMultipleSelections);
    RUN(settingsOpenFocusesASettingsPrompt);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
