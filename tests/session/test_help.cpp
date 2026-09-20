#include "../test_helpers.h"
#include "../editor_test_support.h"
#include "../grid_test_frame.h"

#include <ssg/Keymap.h>
#include <ssg/Style.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TextInputCommands.h>
#include <ssg/HitTester.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = testRuntimePath("runtime_help_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorConfig configFor(const std::filesystem::path& root) {
    ssg::EditorConfig config{
        root / "workspace", root / "recovery", root / "archive"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return config;
}

struct Harness {
    std::filesystem::path root;
    ssg::EditorCreateResult created;
    ssg::Editor* runtime = nullptr;

    explicit Harness(std::string_view name) : root(uniqueRoot(name)),
        created(ssg::createEditor(configFor(root))) {
        if (created.accepted()) {
            runtime = created.session.get();
        }
    }
    ~Harness() {
        created.session.reset();
        std::error_code code;
        std::filesystem::remove_all(root, code);
    }
};

std::optional<ssg::TabState> activeTab(ssg::Editor& runtime) {
    auto snapshot = ssg::test::projectGridFrame(runtime);
    if (!snapshot || !snapshot->tabs.active) return std::nullopt;
    for (const auto& tab : snapshot->tabs.tabs) {
        if (tab.id == *snapshot->tabs.active) return tab;
    }
    return std::nullopt;
}

std::size_t tabCount(ssg::Editor& runtime) {
    auto snapshot = ssg::test::projectGridFrame(runtime);
    return snapshot ? snapshot->tabs.tabs.size() : 0;
}

std::optional<ssg::SolvedUiItem> footerHelpNode(
    ssg::Editor& runtime) {
    auto frame = ssg::test::projectGridFrame(runtime, {120, 24});
    if (!frame || !frame->footer) return std::nullopt;
    for (const auto& item : frame->footer->items) {
        if (item.id == "footer.hint") return item;
    }
    return std::nullopt;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(helpOpenOpensAReadOnlyOutputTab) {
    Harness harness{"open"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    auto tab = activeTab(runtime);
    ASSERT_TRUE(tab.has_value());
    if (!tab) return;
    ASSERT_TRUE(tab->kind == ssg::TabKind::ReadOnlyOutput);
    ASSERT_TRUE(tab->mode == ssg::DocumentMode::ReadOnly);
    ASSERT_EQ(tab->label, std::string{"help"});
}

TEST(clickingFooterHelpHintOpensHelp) {
    Harness harness{"footer_click"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    auto frame = ssg::test::projectGridFrame(runtime, {120, 24});
    auto hint = footerHelpNode(runtime);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(hint.has_value());
    if (!frame || !hint) return;

    const auto hit =
        ssg::HitTester{*frame}.at(hint->rect.x, hint->rect.y);
    ASSERT_EQ(hit.region, ssg::HitRegion::FooterField);
    ASSERT_TRUE(hit.fieldId.has_value());
    if (!hit.fieldId) return;

    ASSERT_TRUE(
        ssg::test::dispatchInput(
            runtime,
            ssg::UiNodePointerInput{ssg::UiNodeId{*hit.fieldId}})
        .accepted());
    const auto tab = activeTab(runtime);
    ASSERT_TRUE(tab.has_value());
    if (tab) ASSERT_EQ(tab->label, std::string{"help"});
}

TEST(helpDocumentContainsProseAndTheLiveKeybinding) {
    Harness harness{"content"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    const auto text = ssg::test::activeDocumentText(runtime);
    ASSERT_TRUE(contains(text, "SSG Help"));
    ASSERT_TRUE(contains(text, "Mouse"));
    // The default Mod+H binding for help.open is generated into the keybindings
    // section (display format lowercases a non-shifted letter: "Mod+h").
    ASSERT_TRUE(contains(text, "Open Help"));
    ASSERT_TRUE(contains(text, "Mod+h"));
    // The help content mentions clicking the header path to show the file tree.
    ASSERT_TRUE(contains(text, "header"));
    // Multiple-cursor usage is documented in prose (not only in the generated
    // keybinding list): the section and its add-cursor key.
    ASSERT_TRUE(contains(text, "Multiple cursors"));
    ASSERT_TRUE(contains(text, "next occurrence"));
    ASSERT_TRUE(contains(text, "Line numbers"));
    ASSERT_TRUE(contains(text, "view.toggle_line_numbers"));
    ASSERT_TRUE(contains(text, "Alt+click adds a cursor"));
    // The Mod rule is stated up front, not left to be inferred from the
    // generated keybinding table.
    ASSERT_TRUE(contains(text, "How keys work"));
    ASSERT_TRUE(contains(text, "Mod is Ctrl or Alt"));
    // Double-click word selection is mentioned in the mouse section.
    ASSERT_TRUE(contains(text, "Double-click a word"));
    // Middle-clicking a tab closes it (mirrors the README note).
    ASSERT_TRUE(contains(text, "middle-click a tab to close it"));
    // No Markdown TABLE syntax (pipe rows) leaks into the help text.
    ASSERT_FALSE(contains(text, "|---|"));
}

TEST(helpDocumentListsChromeGlyphsIncludingTabGlyphsWithValues) {
    Harness harness{"glyphs"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    const auto text = ssg::test::activeDocumentText(runtime);
    // The glyph listing is generated from styleGlyphValues, so both a fixed-slot
    // glyph and the new variable tab glyphs appear -- a newly added glyph would
    // document itself here with no separate list to update.
    ASSERT_TRUE(contains(text, "Chrome glyphs"));
    ASSERT_TRUE(contains(text, "`scrollbar_track`"));
    ASSERT_TRUE(contains(text, "`tab_separator` = \" \""));
    ASSERT_TRUE(contains(text, "`tab_left_edge`"));
    ASSERT_TRUE(contains(text, "style.define"));
}

TEST(helpGlyphListingEscapesQuotesSoItStaysCopyPasteable) {
    Harness harness{"escape"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ssg::StyleDefineArguments args;
    args.values = {{"tab_separator", "\""}};
    ASSERT_TRUE(ssg::applyStyleDefine(runtime, args).accepted);
    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    const auto text = ssg::test::activeDocumentText(runtime);
    // The quote is backslash-escaped so the listed value is a valid Lua string.
    ASSERT_TRUE(contains(text, "`tab_separator` = \"\\\"\""));
}

TEST(helpKeybindingSectionReflectsACustomBind) {
    Harness harness{"custombind"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(ssg::applyKeymapBind(
                    runtime, {"Mod+KeyG", "file.save", "*"})
                    .accepted);
    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    const auto text = ssg::test::activeDocumentText(runtime);
    // A user's custom binding appears because help reads the live keymap
    // (display format lowercases the non-shifted letter: "Mod+g").
    ASSERT_TRUE(contains(text, "Mod+g"));
}

TEST(helpOpenIsIdempotentAndRefreshes) {
    Harness harness{"idempotent"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    const auto firstCount = tabCount(runtime);
    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    // No second help tab.
    ASSERT_EQ(tabCount(runtime), firstCount);
    // The refreshed document is well-formed and still read-only.
    auto tab = activeTab(runtime);
    ASSERT_TRUE(tab.has_value());
    if (tab) ASSERT_TRUE(tab->mode == ssg::DocumentMode::ReadOnly);
    ASSERT_TRUE(contains(ssg::test::activeDocumentText(runtime), "SSG Help"));
}

TEST(helpTabRejectsEditsAndLeavesTheBufferUnchanged) {
    Harness harness{"readonly"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    const auto before = ssg::test::activeDocumentText(runtime);
    // Any mutating command is rejected at the read-only chokepoint.
    ASSERT_FALSE(ssg::test::typeText(runtime, "X").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), before);
}

TEST(savingAHelpTabFailsGracefullyWithoutAPrompt) {
    Harness harness{"save"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    auto save = runtime.dispatch("file.save");
    // Refused, not prompted: a read-only document cannot be saved.
    ASSERT_FALSE(save.accepted());
    // No Save-As path prompt was opened.
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_FALSE(snapshot->prompt.activeKind.has_value());
    }
}

TEST(helpTabTitleCarriesTheReadOnlyMarker) {
    Harness harness{"marker"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
}

TEST(footerHelpShowsTheHelpKeyAndYieldsWhenUnbound) {
    Harness harness{"hint"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    auto hint = footerHelpNode(runtime);
    ASSERT_TRUE(hint.has_value());
    if (hint) {
        ASSERT_TRUE(contains(hint->content, "Mod+h"));
        ASSERT_TRUE(contains(hint->content, "help"));
        ASSERT_TRUE(hint->command.has_value());
        if (hint->command) ASSERT_EQ(*hint->command, std::string{"help.open"});
    }
    // Unbinding help.open drops the key label from the hint.
    ASSERT_TRUE(
        ssg::applyKeymapUnbind(runtime, {"Mod+KeyH", "*"}).accepted);
    auto unbound = footerHelpNode(runtime);
    ASSERT_TRUE(unbound.has_value());
    if (unbound) {
        ASSERT_FALSE(contains(unbound->content, "Mod+h"));
        ASSERT_TRUE(contains(unbound->content, "help"));
    }
}

TEST(closingAHelpTabSucceedsAndReopenIsSkipped) {
    Harness harness{"close"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    // A real editable tab plus the help tab, so closing help leaves something.
    std::ofstream{harness.root / "workspace" / "a.txt"} << "hi\n";
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"a.txt"})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    const auto openCount = tabCount(runtime);
    // Closing the (active) read-only help tab must succeed despite the tab
    // carrying no reopen record.
    ASSERT_TRUE(runtime.dispatch("tab.close")
                    .accepted());
    ASSERT_EQ(tabCount(runtime), openCount - 1);
    auto tab = activeTab(runtime);
    ASSERT_TRUE(tab.has_value());
    if (tab) ASSERT_TRUE(tab->kind != ssg::TabKind::ReadOnlyOutput);
}

TEST(helpTabIsHighlightedAsMarkdown) {
    Harness harness{"markdown"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch("help.open")
                    .accepted());
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& syntax = *snapshot->syntax;
    // The untitled help buffer is highlighted as Markdown (not plain text)
    // because openReadOnlyTab set a language override.
    ASSERT_EQ(syntax.language().value(), std::string{"markdown"});
    // Tree-sitter actually produced highlight spans (headings, etc.), so the
    // prose is colored rather than uniform foreground.
    ASSERT_FALSE(syntax.spans().empty());
}

}  // namespace

SSG_TEST_SUITE(test_help) {
    RUN(helpOpenOpensAReadOnlyOutputTab);
    RUN(clickingFooterHelpHintOpensHelp);
    RUN(helpDocumentContainsProseAndTheLiveKeybinding);
    RUN(helpDocumentListsChromeGlyphsIncludingTabGlyphsWithValues);
    RUN(helpGlyphListingEscapesQuotesSoItStaysCopyPasteable);
    RUN(helpKeybindingSectionReflectsACustomBind);
    RUN(helpOpenIsIdempotentAndRefreshes);
    RUN(helpTabRejectsEditsAndLeavesTheBufferUnchanged);
    RUN(savingAHelpTabFailsGracefullyWithoutAPrompt);
    RUN(helpTabTitleCarriesTheReadOnlyMarker);
    RUN(footerHelpShowsTheHelpKeyAndYieldsWhenUnbound);
    RUN(closingAHelpTabSucceedsAndReopenIsSkipped);
    RUN(helpTabIsHighlightedAsMarkdown);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
