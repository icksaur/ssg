#include "command_cases.h"
#include "../grid_test_view.h"
#include "../test_helpers.h"

#include <ssg/EditorSession.h>
#include <tui/GridPresenter.h>
#include <ssg/Keymap.h>
#include <ssg/ViewportProjection.h>

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

static_assert(!std::is_copy_constructible_v<ssg::GridFrame>);
static_assert(!std::is_copy_assignable_v<ssg::GridFrame>);
static_assert(std::is_nothrow_move_constructible_v<ssg::GridFrame>);
static_assert(std::is_nothrow_move_assignable_v<ssg::GridFrame>);
static_assert(!std::is_copy_constructible_v<ssg::GridPresentation>);
static_assert(!std::is_copy_assignable_v<ssg::GridPresentation>);
static_assert(std::is_nothrow_move_constructible_v<ssg::GridPresentation>);
static_assert(std::is_nothrow_move_assignable_v<ssg::GridPresentation>);
template <class T>
concept HasPresentation = requires(T const& value) {
    value.presentation();
};
static_assert(!HasPresentation<ssg::SessionSnapshot>);
template <class T>
concept HasSemantic = requires(T const& value) {
    value.semantic();
    value.sections();
};
static_assert(!HasSemantic<ssg::GridFrame>);
static_assert(HasSemantic<ssg::GridPresentation>);
static_assert(std::same_as<
              decltype(std::declval<ssg::GridFrame const&>().presentation()),
              ssg::GridProjection const&>);
static_assert(std::constructible_from<
              ssg::GridFrame, ssg::SessionSnapshot, ssg::GridProjection,
              ssg::GridBasis, ssg::PaletteReport>);

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = testRuntimePath("runtime_snapshot_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorSessionConfig configFor(const std::filesystem::path& root) {
    return {root / "workspace", root / "scratch", root / "recovery"};
}

TEST(constructionRejectsInvalidCwd) {
    auto root = uniqueRoot("invalid_cwd");
    auto result = ssg::EditorSession::create(configFor(root / "missing"));
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.message.empty());
}

// commandCaseTableExactlyMatchesP0Catalog is deleted.  It proved the test
// case table listed exactly the catalog's commands, back when the table was
// typed out by hand.  The cases are now derived from the running editor, so it
// compared a thing to itself.

TEST(runtimeConstructsAttachesAndProducesLiveSnapshot) {
    auto root = uniqueRoot("snapshot");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;

    auto& runtime = *created.session;
    ssg::InvocationPrincipal principal{ssg::ClientId{7}, ssg::InvocationOrigin::InProcess};
    ASSERT_TRUE(runtime.attach(std::move(principal), ssg::ViewId{9}).accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{7});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->revision(), runtime.revision());
    ASSERT_EQ(snapshot->client().clientId, ssg::ClientId{7});
    ASSERT_EQ(snapshot->client().viewId, ssg::ViewId{9});
}

TEST(gridPresentationRejectsMismatchedSemanticRevision) {
    auto root = uniqueRoot("grid_presentation_revision");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;

    auto& runtime = *created.session;
    const ssg::ClientId client{7};
    const ssg::ViewId view{9};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess}, view)
                    .accepted());
    auto semantic = runtime.snapshot(client);
    ASSERT_TRUE(semantic.has_value());
    if (!semantic) return;

    const auto revision =
        ssg::Revision{semantic->revision().value() + 1};
    ssg::GridFrame frame{
        *semantic,
        ssg::GridProjection{
            ssg::ViewportViewState{ssg::ViewportDimensions{80, 24}},
            ssg::Style{}, {}},
        ssg::GridBasis{view, revision, 0}, {}};
    ASSERT_THROWS((ssg::GridPresentation{
                      std::move(*semantic), std::move(frame)}),
                  std::logic_error);
}

TEST(presentationProjectionRejectsARevisionThatChangedAfterCapture) {
    auto root = uniqueRoot("presentation_projection_basis");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;

    auto& runtime = *created.session;
    const ssg::ClientId client{7};
    const ssg::ViewId view{9};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess}, view)
                    .accepted());

    auto capture = runtime.capturePresentation(client, view, {});
    ASSERT_TRUE(capture.has_value());
    if (!capture) return;

    ssg::ViewportProjectionState state;
    ssg::ViewportProjectionRequest request{
        client,
        view,
        capture->semantic.revision(),
        ssg::ViewportDimensions{80, 24},
        20,
        80,
        {},
        false,
    };
    auto projected = runtime.projectViewport(request, state);
    ASSERT_TRUE(projected.has_value());

    ASSERT_TRUE(
        runtime.dispatch(client, {"panel.show_files", runtime.revision(), {}})
            .accepted());
    ASSERT_FALSE(runtime.projectViewport(request, state).has_value());
}

TEST(runtimeSourcesDoNotIncludeFixtureModel) {
    auto root = std::filesystem::path{SSG_SOURCE_SCAN_ROOT};
    bool found = false;
    bool snapshotConstCast = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "src"}) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.rfind("src/runtime/", 0) != 0 && relative != "src/EditorSession.cpp") continue;
        std::ifstream input{entry.path()};
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        found = found || text.find("FixtureModel") != std::string::npos;
        snapshotConstCast =
            snapshotConstCast ||
            text.find("const_cast<EditorSession::Impl*>") != std::string::npos;
    }

    ASSERT_FALSE(found);
    ASSERT_FALSE(snapshotConstCast);
}

TEST(runtimePublishesValidCuratedKeymap) {
    auto root = uniqueRoot("keymap_valid");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& keymap = snapshot->sections().keymap;
    ASSERT_FALSE(keymap.bindings.empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.validate().empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.hasGlobalBinding("settings.open"));
}

TEST(everyDocumentLineIsReachableAndTheCaretIsNeverLost) {
    auto root = uniqueRoot("viewport_reach");
    // More lines than fit, so the document genuinely scrolls.
    {
        std::ofstream file{root / "workspace" / "long.txt"};
        for (int line = 1; line <= 60; ++line) file << "line " << line << "\n";
    }
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"long.txt"}}).accepted());
    runtime.focusEditor();

    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{ssg::ClientId{1}, ssg::ViewId{1}, dims};
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
        auto const& view = snapshot->presentation().viewport;
        auto const caretLine =
            snapshot->sections().selection.primary().active.line.value();
        // The caret's line is always inside the window that is actually painted.
        ASSERT_TRUE(caretLine >= view.firstVisualRow);
        ASSERT_TRUE(caretLine < view.firstVisualRow + view.visibleRows.size());
        lastVisibleLine = std::max<std::uint32_t>(lastVisibleLine, caretLine);
        (void)grid.dispatch(
            runtime, {"cursor.line_down", runtime.revision(), {}});
    }
    // The last line of the document was reached, not merely approached.
    auto final = grid.present(runtime);
    ASSERT_TRUE(final.has_value());
    if (!final) return;
    ASSERT_EQ(lastVisibleLine, final->presentation().viewport.totalVisualRows - 1);

    // Scrolling to the maximum offset shows the final line, so no row is
    // stranded past the end of the scroll range.
    ASSERT_TRUE(grid.dispatch(runtime,
                              {"view.scroll_lines", runtime.revision(),
                               ssg::ScrollLinesArguments{500}}).accepted());
    auto bottom = grid.present(runtime);
    ASSERT_TRUE(bottom.has_value());
    if (!bottom) return;
    auto const& view = bottom->presentation().viewport;
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"snug.txt"}}).accepted());
    runtime.focusEditor();
    ssg::test::GridTestView grid{ssg::ClientId{1}, ssg::ViewId{1}, dims};
    auto snapshot = grid.present(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& view = snapshot->presentation().viewport;
    ASSERT_TRUE(view.totalVisualRows > view.visibleRows.size());
    ASSERT_TRUE(view.scrollbar.maximumFirstRow > 0);
    std::filesystem::remove_all(root);
}

// Every session opens on an empty untitled buffer.  Reporting it unsaved is
// technically true and practically useless: the badge appears before the user
// has done anything, so it stops meaning "you have work to lose".
TEST(anEmptyScratchBufferIsNotUnsavedUntilItHasContent) {
    auto root = uniqueRoot("scratch_dirty");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.new", runtime.revision(), {}}).accepted());
    runtime.focusEditor();

    auto const dirty = [&] {
        auto snapshot = runtime.snapshot(ssg::ClientId{1});
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    auto const tabLabels = [&] {
        auto snapshot = runtime.snapshot(ssg::ClientId{1});
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"blank.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"alpha.txt"}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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
    ssg::test::GridTestView grid{ssg::ClientId{1}, ssg::ViewId{1}, dims};
    (void)grid.present(runtime);
    auto snapshot = grid.present(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& view = snapshot->presentation().viewport;
    ASSERT_TRUE(view.totalVisualRows > 1);
    std::filesystem::remove_all(root);
}

TEST(curatedKeymapBindingsAreArgumentFree) {
    auto root = uniqueRoot("keymap_argfree");
    std::ofstream{root / "workspace" / "doc.txt"} << "alpha\nbeta\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    (void)runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"doc.txt"}});
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());

    // Resolve the add-cursor-down chord from the published keymap, then dispatch
    // the resolved command: the snapshot must show more than one selection.
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto chord = *ssg::KeyCodec{}.parseSequence({"Alt+KeyJ"});
    auto resolved = ssg::KeymapMatcher{snapshot->sections().keymap}.resolveSequence(chord, "editor");
    ASSERT_EQ(resolved.kind, ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(resolved.commandId, std::string{"select.add_cursor_down"});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {resolved.commandId, runtime.revision(), {}}).accepted());

    auto after = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->sections().selection.items().size() > std::size_t{1});
}

TEST(settingsOpenFocusesASettingsPrompt) {
    auto root = uniqueRoot("settings_open");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The chord actually opens: focus moves to the prompt with a visible input.
    ASSERT_EQ(snapshot->sections().uiFrame.effectiveFocus(),
              ssg::FocusTarget::Prompt);
}

TEST(theDimensionlessSnapshotCarriesSemanticStateButNeverGridProjection) {
    // The reframe's load-bearing seam: a client that lays out the semantic model
    // itself asks for a snapshot WITHOUT ViewportDimensions and gets the identical
    // semantic sections a grid client sees, but no PresentationSnapshot at all.
    // Semantic state is never gated on grid geometry.
    auto root = uniqueRoot("dimensionless_semantic");
    std::ofstream{root / "workspace" / "m.txt"} << "alpha\nbeta\ngamma\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());
    // Show the panel and select a node so the tree has a live scroll window: the
    // window is grid projection and must NOT leak into the semantic tree section.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());

    // A grid client (with dimensions) and a native client (without) taken at the
    // same revision.
    ssg::test::GridTestView gridView{
        ssg::ClientId{1}, ssg::ViewId{1}, {80, 24}};
    auto grid = gridView.present(runtime);
    auto semantic = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(grid.has_value());
    ASSERT_TRUE(semantic.has_value());
    if (!grid || !semantic) return;

    ASSERT_TRUE(grid->document().has_value());
    ASSERT_TRUE(grid->panel().has_value());

    // The semantic sections are byte-for-byte identical: document, selection set,
    // tabs, keymap, theme roles, focus, AND the tree (nodes/selection/expansion,
    // with no scroll window). Geometry does not change what the model IS.
    ASSERT_TRUE(grid->semantic().sections() == semantic->sections());
    ASSERT_TRUE(grid->semantic().sections().tree == semantic->sections().tree);
    ASSERT_FALSE(semantic->sections().tree.providers.empty());

    // The same command drives the same semantic result on the dimensionless path:
    // an edit is visible in a subsequent dimensionless snapshot with no geometry
    // supplied at any point.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                    {"text.insert", runtime.revision(),
                     ssg::TextInputArguments{"X"}}).accepted());
    auto edited = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(edited.has_value());
    if (!edited) return;
    ASSERT_TRUE(edited->sections().document.text.find('X') != std::string::npos);
    ASSERT_TRUE(edited->sections().document != semantic->sections().document);
}

TEST(gridPresenterCannotChangeOrReassembleSemanticState) {
    auto root = uniqueRoot("grid_presenter_semantics");
    std::ofstream{root / "workspace" / "m.txt"} << "alpha\nbeta\ngamma\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    const ssg::ViewId view{9};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess}, view)
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client, {"file.open", runtime.revision(),
                                       std::string{"m.txt"}})
                    .accepted());

    auto before = runtime.snapshot(client);
    ASSERT_TRUE(before.has_value());
    if (!before) return;
    const auto revision = runtime.revision();
    ssg::GridPresenter presenter{view};
    auto frame = presenter.project(runtime, client, {{80, 24}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->basis().viewId, view);
    ASSERT_EQ(frame->basis().semanticRevision, revision);
    const auto firstGeneration = frame->basis().presentationGeneration;
    ASSERT_EQ(frame->semantic().sections(), before->sections());
    ASSERT_EQ(runtime.revision(), revision);

    auto after = runtime.snapshot(client);
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_EQ(after->sections(), before->sections());
    ASSERT_EQ(after->topology(), before->topology());
    ASSERT_EQ(after->client(), before->client());

    auto nextFrame = presenter.project(runtime, client, {{100, 30}, {}});
    ASSERT_TRUE(nextFrame.has_value());
    if (!nextFrame) return;
    ASSERT_EQ(nextFrame->basis().semanticRevision, revision);
    ASSERT_EQ(nextFrame->basis().presentationGeneration,
              firstGeneration + 1);
    ASSERT_EQ(nextFrame->semantic().sections(), before->sections());

    ssg::GridPresenter wrongView{ssg::ViewId{10}};
    ASSERT_FALSE(
        wrongView.project(runtime, client, {{80, 24}, {}}).has_value());
    ASSERT_EQ(runtime.revision(), revision);

    auto staleRoot = uniqueRoot("grid_presenter_stale_revision");
    auto staleCreated = ssg::EditorSession::create(configFor(staleRoot));
    ASSERT_TRUE(staleCreated.accepted());
    if (!staleCreated.accepted()) return;
    auto& staleRuntime = *staleCreated.session;
    ASSERT_TRUE(
        staleRuntime
            .attach({client, ssg::InvocationOrigin::InProcess}, view)
            .accepted());
    ASSERT_TRUE(staleRuntime.revision() < revision);
    ASSERT_FALSE(
        presenter.project(staleRuntime, client, {{80, 24}, {}}).has_value());

    auto resumedFrame = presenter.project(runtime, client, {{100, 30}, {}});
    ASSERT_TRUE(resumedFrame.has_value());
    if (!resumedFrame) return;
    ASSERT_EQ(resumedFrame->basis().presentationGeneration,
              nextFrame->basis().presentationGeneration + 1);
}

TEST(gridPresenterOwnsScrollAndRejectsAReusedFrameBasis) {
    auto root = uniqueRoot("grid_presenter_scroll");
    {
        std::ofstream file{root / "workspace" / "long.txt"};
        for (int line = 0; line < 80; ++line) file << "line\n";
    }

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    const ssg::ViewId view{3};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess}, view)
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client, {"file.open", runtime.revision(),
                                       std::string{"long.txt"}})
                    .accepted());

    ssg::GridPresenter presenter{view};
    auto frame = presenter.project(runtime, client, {{80, 12}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto revision = runtime.revision();
    auto noOpCommand = runtime.dispatch(
        client, {"view.scroll_lines", revision,
                 ssg::ScrollLinesArguments{-1}});
    ASSERT_TRUE(noOpCommand.viewAction.has_value());
    if (!noOpCommand.viewAction) return;
    auto noOp = presenter.apply(*noOpCommand.viewAction, *frame);
    ASSERT_TRUE(noOp.accepted());
    ASSERT_FALSE(
        presenter.apply(*noOpCommand.viewAction, *frame).accepted());
    frame = presenter.project(runtime, client, {{80, 12}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;

    auto command = runtime.dispatch(
        client, {"view.scroll_lines", revision,
                 ssg::ScrollLinesArguments{5}});
    ASSERT_EQ(command.outcome(),
              ssg::CommandResult::Outcome::ViewActionRequired);
    ASSERT_TRUE(command.viewAction.has_value());
    ASSERT_EQ(runtime.revision(), revision);
    if (!command.viewAction) return;

    auto applied = presenter.apply(*command.viewAction, *frame);
    ASSERT_TRUE(applied.accepted());
    ASSERT_EQ(runtime.revision(), revision);
    auto stale = presenter.apply(*command.viewAction, *frame);
    ASSERT_FALSE(stale.accepted());

    auto scrolled = presenter.project(runtime, client, {{80, 12}, {}});
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    ASSERT_EQ(scrolled->presentation().viewport.firstVisualRow, 5U);

    const auto selectionBefore = runtime.snapshot(client)->sections().selection;
    auto visual = runtime.dispatch(
        client, {"cursor.page_down", runtime.revision(), {}});
    ASSERT_EQ(visual.outcome(),
              ssg::CommandResult::Outcome::ViewActionRequired);
    auto selectionAfter = runtime.snapshot(client);
    ASSERT_TRUE(selectionAfter.has_value());
    if (selectionAfter) {
        ASSERT_EQ(selectionAfter->sections().selection, selectionBefore);
    }
}

TEST(sessionOwnsIndependentPaneTopologyForEachAttachment) {
    auto root = uniqueRoot("grid_presenter_panes");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId firstClient{1};
    const ssg::ClientId secondClient{2};
    ASSERT_TRUE(runtime
                    .attach({firstClient, ssg::InvocationOrigin::InProcess,
                             {ssg::CapabilityId{"local_file_drop"}}},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .attach({secondClient, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{2})
                    .accepted());
    auto firstSnapshot = runtime.snapshot(firstClient);
    auto secondSnapshot = runtime.snapshot(secondClient);
    ASSERT_TRUE(firstSnapshot.has_value());
    ASSERT_TRUE(secondSnapshot.has_value());
    if (!firstSnapshot || !secondSnapshot) return;
    ASSERT_EQ(firstSnapshot->client().capabilities.size(), std::size_t{1});
    ASSERT_TRUE(secondSnapshot->client().capabilities.empty());
    ASSERT_EQ(firstSnapshot->topology().panes.panes().size(), std::size_t{1});
    ASSERT_EQ(secondSnapshot->topology().panes.panes().size(),
              std::size_t{1});

    ssg::GridPresenter first{ssg::ViewId{1}};
    ssg::GridPresenter second{ssg::ViewId{2}};
    auto firstFrame = first.project(runtime, firstClient, {{80, 24}, {}});
    auto secondFrame = second.project(runtime, secondClient, {{80, 24}, {}});
    ASSERT_TRUE(firstFrame.has_value());
    ASSERT_TRUE(secondFrame.has_value());
    if (!firstFrame || !secondFrame) return;
    ASSERT_TRUE(firstFrame->document().has_value());
    ASSERT_TRUE(secondFrame->document().has_value());
    if (!firstFrame->document() || !secondFrame->document()) return;
    ASSERT_EQ(firstFrame->document()->panes.size(), std::size_t{1});
    ASSERT_EQ(secondFrame->document()->panes.size(), std::size_t{1});

    const auto revision = runtime.revision();
    auto split = runtime.dispatch(
        firstClient, {"pane.split_horizontal", revision, {}});
    ASSERT_TRUE(split.completed());
    ASSERT_EQ(runtime.revision(), ssg::Revision{revision.value() + 1});
    ASSERT_FALSE(split.viewAction.has_value());

    firstFrame = first.project(runtime, firstClient, {{80, 24}, {}});
    secondFrame = second.project(runtime, secondClient, {{80, 24}, {}});
    ASSERT_TRUE(firstFrame.has_value());
    ASSERT_TRUE(secondFrame.has_value());
    if (!firstFrame || !secondFrame) return;
    ASSERT_TRUE(firstFrame->document().has_value());
    ASSERT_TRUE(secondFrame->document().has_value());
    if (!firstFrame->document() || !secondFrame->document()) return;
    ASSERT_EQ(firstFrame->document()->panes.size(), std::size_t{2});
    ASSERT_EQ(secondFrame->document()->panes.size(), std::size_t{1});
    firstSnapshot = runtime.snapshot(firstClient);
    secondSnapshot = runtime.snapshot(secondClient);
    ASSERT_TRUE(firstSnapshot.has_value());
    ASSERT_TRUE(secondSnapshot.has_value());
    if (!firstSnapshot || !secondSnapshot) return;
    ASSERT_EQ(firstSnapshot->topology().panes.panes().size(), std::size_t{2});
    ASSERT_EQ(secondSnapshot->topology().panes.panes().size(),
              std::size_t{1});

    auto blockedFocus = runtime.dispatch(
        firstClient, {"pane.focus_down", runtime.revision(), {}});
    ASSERT_TRUE(blockedFocus.viewAction.has_value());
    if (!blockedFocus.viewAction) return;
    auto blockedApplied = first.apply(*blockedFocus.viewAction, *firstFrame);
    ASSERT_TRUE(blockedApplied.accepted());
    ASSERT_FALSE(blockedApplied.transition.has_value());

    ASSERT_TRUE(runtime
                    .dispatch(firstClient,
                              {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    firstFrame = first.project(runtime, firstClient, {{80, 24}, {}});
    ASSERT_TRUE(firstFrame.has_value());
    if (!firstFrame) return;
    auto focused = runtime.dispatch(
        firstClient, {"pane.focus_up", runtime.revision(), {}});
    ASSERT_TRUE(focused.viewAction.has_value());
    if (!focused.viewAction) return;
    auto focusedApplied = first.apply(*focused.viewAction, *firstFrame);
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
    const auto generationBeforeFocus =
        firstFrame->sections().followEdits.generation;
    const auto focusRevision = runtime.revision();
    const auto focusResult =
        runtime.input(firstClient, *focusedApplied.transition);
    ASSERT_EQ(focusResult.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(runtime.revision(),
              ssg::Revision{focusRevision.value() + 1});
    auto focusedSnapshot = runtime.snapshot(firstClient);
    ASSERT_TRUE(focusedSnapshot.has_value());
    if (!focusedSnapshot) return;
    ASSERT_EQ(focusedSnapshot->sections().uiFrame.effectiveFocus(),
              ssg::FocusTarget::Editor);
    ASSERT_EQ(focusedSnapshot->sections().followEdits.mode,
              ssg::FollowMode::Paused);
    ASSERT_EQ(focusedSnapshot->sections().followEdits.generation,
              generationBeforeFocus + 1);

    firstFrame = first.project(runtime, firstClient, {{80, 24}, {}});
    ASSERT_TRUE(firstFrame.has_value());
    if (!firstFrame) return;
    auto focusedAgain = runtime.dispatch(
        firstClient, {"pane.focus_down", runtime.revision(), {}});
    ASSERT_TRUE(focusedAgain.viewAction.has_value());
    if (!focusedAgain.viewAction) return;
    auto focusedAgainApplied =
        first.apply(*focusedAgain.viewAction, *firstFrame);
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
    const auto generationBeforeRepeatedFocus =
        firstFrame->sections().followEdits.generation;
    const auto repeatedFocusResult =
        runtime.input(firstClient, *focusedAgainApplied.transition);
    ASSERT_EQ(repeatedFocusResult.outcome,
              ssg::ClientInputOutcome::Dispatched);
    auto repeatedFocusSnapshot = runtime.snapshot(firstClient);
    ASSERT_TRUE(repeatedFocusSnapshot.has_value());
    if (!repeatedFocusSnapshot) return;
    ASSERT_EQ(repeatedFocusSnapshot->sections().followEdits.generation,
              generationBeforeRepeatedFocus + 1);

    firstFrame = first.project(runtime, firstClient, {{80, 24}, {}});
    ASSERT_TRUE(firstFrame.has_value());
    if (!firstFrame) return;
    auto cycled =
        runtime.dispatch(firstClient, {"pane.next", runtime.revision(), {}});
    ASSERT_TRUE(cycled.completed());
    ASSERT_FALSE(cycled.viewAction.has_value());
    auto cycledSnapshot = runtime.snapshot(firstClient);
    ASSERT_TRUE(cycledSnapshot.has_value());
    if (!cycledSnapshot) return;
    ASSERT_EQ(cycledSnapshot->topology().panes.activePane(), ssg::PaneId{1});

    firstFrame = first.project(runtime, firstClient, {{80, 24}, {}});
    ASSERT_TRUE(firstFrame.has_value());
    if (!firstFrame) return;
    auto closed =
        runtime.dispatch(firstClient, {"pane.close", runtime.revision(), {}});
    ASSERT_TRUE(closed.completed());
    ASSERT_FALSE(closed.viewAction.has_value());

    auto closedFrame = first.project(runtime, firstClient, {{80, 24}, {}});
    ASSERT_TRUE(closedFrame.has_value());
    if (closedFrame) {
        ASSERT_TRUE(closedFrame->document().has_value());
        if (closedFrame->document()) {
            ASSERT_EQ(closedFrame->document()->panes.size(), std::size_t{1});
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
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"file.open", runtime.revision(),
                               std::string{"lines.txt"}})
                    .accepted());

    const auto revision = runtime.revision();
    auto moved = runtime.dispatch(
        client, {"cursor.line_down", revision, {}});
    ASSERT_EQ(moved.outcome(),
              ssg::CommandResult::Outcome::ViewActionRequired);
    ASSERT_EQ(runtime.revision(), revision);
    ASSERT_TRUE(moved.viewAction.has_value());
    if (!moved.viewAction) return;
    const auto expectedMove = ssg::ViewAction{
        ssg::MoveVisualSelection{
            ssg::VisualSelectionDirection::LineDown, false}};
    ASSERT_EQ(moved.viewAction->action, expectedMove);

    ssg::GridPresenter presenter{ssg::ViewId{1}};
    auto frame = presenter.project(runtime, client, {{80, 12}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto applied = presenter.apply(*moved.viewAction, *frame);
    ASSERT_TRUE(applied.accepted());
    ASSERT_TRUE(
        applied.transition &&
        std::holds_alternative<ssg::SelectionTransition>(
            applied.transition->transition));
    const auto submitted = runtime.input(client, *applied.transition);
    ASSERT_EQ(submitted.outcome, ssg::ClientInputOutcome::Dispatched);
    auto confirmed = presenter.project(runtime, client, {{80, 12}, {}});
    ASSERT_TRUE(confirmed.has_value());
    if (!confirmed) return;
    ASSERT_EQ(confirmed->sections().selection.primary().active.line,
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
        const auto before = confirmed->sections().selection;
        auto command = runtime.dispatch(
            client, {movement.command, runtime.revision(), {}});
        ASSERT_EQ(command.outcome(),
                  ssg::CommandResult::Outcome::ViewActionRequired);
        ASSERT_TRUE(command.viewAction.has_value());
        if (!command.viewAction) return;
        const auto expected = ssg::ViewAction{
            ssg::MoveVisualSelection{
                movement.direction, movement.extend}};
        ASSERT_EQ(command.viewAction->action, expected);
        auto result = presenter.apply(*command.viewAction, *confirmed);
        ASSERT_TRUE(result.transition.has_value());
        if (!result.transition) return;
        ASSERT_EQ(runtime.input(client, *result.transition).outcome,
                  ssg::ClientInputOutcome::Dispatched);
        confirmed = presenter.project(runtime, client, {{80, 12}, {}});
        ASSERT_TRUE(confirmed.has_value());
        if (!confirmed) return;
        ASSERT_NE(confirmed->sections().selection, before);
    }
}

TEST(visualMovementUsesActivePaneAndDiscardsMismatchedProposal) {
    auto root = uniqueRoot("grid_presenter_active_pane_selection");
    {
        std::ofstream lines{root / "workspace" / "lines.txt"};
        for (int line = 0; line < 60; ++line) {
            lines << "line " << line << '\n';
        }
    }
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"file.open", runtime.revision(),
                               std::string{"lines.txt"}})
                    .accepted());

    ssg::GridPresenter presenter{ssg::ViewId{1}};
    auto frame = presenter.project(runtime, client, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto split = runtime.dispatch(
        client, {"pane.split_horizontal", runtime.revision(), {}});
    ASSERT_TRUE(split.completed());
    frame = presenter.project(runtime, client, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame || !frame->document() ||
        frame->document()->panes.size() != 2) {
        return;
    }
    const auto activeRows = static_cast<std::uint32_t>(
        frame->document()->panes.back().content.height);

    auto page = runtime.dispatch(
        client, {"cursor.page_down", runtime.revision(), {}});
    ASSERT_TRUE(page.viewAction.has_value());
    if (!page.viewAction) return;
    auto proposed = presenter.apply(*page.viewAction, *frame);
    ASSERT_TRUE(proposed.transition.has_value());
    if (!proposed.transition) return;
    ASSERT_EQ(runtime.input(client, *proposed.transition).outcome,
              ssg::ClientInputOutcome::Dispatched);
    frame = presenter.project(runtime, client, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->sections().selection.primary().active.line.value(),
              activeRows);

    auto next = runtime.dispatch(
        client, {"cursor.line_down", runtime.revision(), {}});
    ASSERT_TRUE(next.viewAction.has_value());
    if (!next.viewAction) return;
    auto staleProposal = presenter.apply(*next.viewAction, *frame);
    ASSERT_TRUE(staleProposal.transition.has_value());
    if (!staleProposal.transition) return;
    ASSERT_EQ(
        runtime
            .input(client,
                   ssg::ViewTransitionInput{
                       {runtime.revision()},
                       ssg::PaneFocusTransition{ssg::PaneId{1}}})
            .outcome,
        ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(runtime.input(client, *staleProposal.transition).outcome,
              ssg::ClientInputOutcome::Rejected);
    frame = presenter.project(runtime, client, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto unchangedLine =
        frame->sections().selection.primary().active.line;
    auto retry = runtime.dispatch(
        client, {"cursor.line_down", runtime.revision(), {}});
    ASSERT_TRUE(retry.viewAction.has_value());
    if (!retry.viewAction) return;
    auto retried = presenter.apply(*retry.viewAction, *frame);
    ASSERT_TRUE(retried.transition.has_value());
    if (!retried.transition) return;
    ASSERT_EQ(runtime.input(client, *retried.transition).outcome,
              ssg::ClientInputOutcome::Dispatched);
    frame = presenter.project(runtime, client, {{41, 15}, {}});
    ASSERT_TRUE(frame.has_value());
    if (frame) {
        ASSERT_EQ(frame->sections().selection.primary().active.line.value(),
                  unchangedLine.value() + 1);
    }
}

} // namespace

SSG_TEST_SUITE(test_session_snapshot) {
    RUN(constructionRejectsInvalidCwd);
    RUN(runtimeConstructsAttachesAndProducesLiveSnapshot);
    RUN(presentationProjectionRejectsARevisionThatChangedAfterCapture);
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
    RUN(theDimensionlessSnapshotCarriesSemanticStateButNeverGridProjection);
    RUN(gridPresenterCannotChangeOrReassembleSemanticState);
    RUN(gridPresenterOwnsScrollAndRejectsAReusedFrameBasis);
    RUN(sessionOwnsIndependentPaneTopologyForEachAttachment);
    RUN(visualLineMovementRequiresPresenterResolution);
    RUN(visualMovementUsesActivePaneAndDiscardsMismatchedProposal);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
