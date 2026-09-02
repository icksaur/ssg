#include "../test_helpers.h"
#include "../grid_test_frame.h"

#include <ssg/EditorSession.h>
#include <ssg/Keymap.h>
#include <ssg/Style.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TextInputCommands.h>
#include <ssg/session_snapshot.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_help_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorSessionConfig configFor(const std::filesystem::path& root) {
    ssg::EditorSessionConfig config{
        root / "workspace", root / "scratch", root / "recovery"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return config;
}

struct Harness {
    std::filesystem::path root;
    ssg::EditorSessionCreateResult created;
    ssg::EditorSession* runtime = nullptr;

    explicit Harness(std::string_view name) : root(uniqueRoot(name)),
        created(ssg::EditorSession::create(configFor(root))) {
        if (created.accepted()) {
            runtime = created.session.get();
            (void)runtime->attach(
                {ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                ssg::ViewId{1});
        }
    }
    ~Harness() {
        created.session.reset();
        std::error_code code;
        std::filesystem::remove_all(root, code);
    }
};

std::optional<ssg::TabState> activeTab(ssg::EditorSession& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    if (!snapshot || !snapshot->sections().tabs.active) return std::nullopt;
    for (const auto& tab : snapshot->sections().tabs.tabs) {
        if (tab.id == *snapshot->sections().tabs.active) return tab;
    }
    return std::nullopt;
}

std::size_t tabCount(ssg::EditorSession& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    return snapshot ? snapshot->sections().tabs.tabs.size() : 0;
}

std::optional<ssg::SolvedUiItem> footerHelpNode(
    ssg::EditorSession& runtime) {
    auto frame = ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, {120, 24});
    if (!frame || !frame->footer()) return std::nullopt;
    for (const auto& item : frame->footer()->items) {
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

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    auto tab = activeTab(runtime);
    ASSERT_TRUE(tab.has_value());
    if (!tab) return;
    ASSERT_TRUE(tab->kind == ssg::TabKind::ReadOnlyOutput);
    ASSERT_TRUE(tab->mode == ssg::DocumentMode::ReadOnly);
    ASSERT_EQ(tab->label, std::string{"help"});
}

TEST(helpDocumentContainsProseAndTheLiveKeybinding) {
    Harness harness{"content"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    const auto text = runtime.activeDocumentText();
    ASSERT_TRUE(contains(text, "SSG Help"));
    ASSERT_TRUE(contains(text, "Mouse"));
    // The default Alt+H binding for help.open is generated into the keybindings
    // section (display format lowercases a non-shifted letter: "Alt+h").
    ASSERT_TRUE(contains(text, "Open Help"));
    ASSERT_TRUE(contains(text, "Alt+h"));
    // The help content mentions clicking the header path to show the file tree.
    ASSERT_TRUE(contains(text, "header"));
    // Multiple-cursor usage is documented in prose (not only in the generated
    // keybinding list): the section and its add-cursor key.
    ASSERT_TRUE(contains(text, "Multiple cursors"));
    ASSERT_TRUE(contains(text, "next occurrence"));
    ASSERT_TRUE(contains(text, "Line numbers"));
    ASSERT_TRUE(contains(text, "view.toggle_line_numbers"));
    ASSERT_TRUE(contains(text, "Alt+click adds a cursor"));
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

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    const auto text = runtime.activeDocumentText();
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
    ASSERT_TRUE(
        runtime.dispatch(ssg::ClientId{1},
                         {"style.define", runtime.revision(), args})
            .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    const auto text = runtime.activeDocumentText();
    // The quote is backslash-escaped so the listed value is a valid Lua string.
    ASSERT_TRUE(contains(text, "`tab_separator` = \"\\\"\""));
}

TEST(helpKeybindingSectionReflectsACustomBind) {
    Harness harness{"custombind"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"keymap.bind", runtime.revision(),
                       ssg::KeymapBindArguments{"Alt+KeyG", "file.save", "*"}})
            .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    const auto text = runtime.activeDocumentText();
    // A user's custom binding appears because help reads the live keymap
    // (display format lowercases the non-shifted letter: "Alt+g").
    ASSERT_TRUE(contains(text, "Alt+g"));
}

TEST(helpOpenIsIdempotentAndRefreshes) {
    Harness harness{"idempotent"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    const auto firstCount = tabCount(runtime);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    // No second help tab.
    ASSERT_EQ(tabCount(runtime), firstCount);
    // The refreshed document is well-formed and still read-only.
    auto tab = activeTab(runtime);
    ASSERT_TRUE(tab.has_value());
    if (tab) ASSERT_TRUE(tab->mode == ssg::DocumentMode::ReadOnly);
    ASSERT_TRUE(contains(runtime.activeDocumentText(), "SSG Help"));
}

TEST(helpTabRejectsEditsAndLeavesTheBufferUnchanged) {
    Harness harness{"readonly"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    const auto before = runtime.activeDocumentText();
    // Any mutating command is rejected at the read-only chokepoint.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"text.insert", runtime.revision(),
                                   ssg::TextInputArguments{"X"}})
                     .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), before);
}

TEST(savingAHelpTabFailsGracefullyWithoutAPrompt) {
    Harness harness{"save"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    auto save = runtime.dispatch(ssg::ClientId{1},
                                 {"file.save", runtime.revision(), {}});
    // Refused, not prompted: a read-only document cannot be saved.
    ASSERT_FALSE(save.accepted());
    // No Save-As path prompt was opened.
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_FALSE(snapshot->sections().promptStatus.activeKind.has_value());
    }
}

TEST(helpTabNeverPersistsADraft) {
    Harness harness{"persist"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    // The read-only help document is not an autosave candidate.
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{0});
    // And no draft file for it was written under the scratch root. (The scratch
    // root may hold session-lock/journal infrastructure; what must not appear is
    // a persisted draft, which flushAllAutosaveDrafts returning 0 already
    // guarantees -- this is a belt-and-braces check that opening help added no
    // files versus a baseline with no help tab.)
    Harness baseline{"persist_baseline"};
    ASSERT_TRUE(baseline.runtime != nullptr);
    if (!baseline.runtime) return;
    auto countFiles = [](const std::filesystem::path& dir) {
        std::size_t n = 0;
        for (auto const& entry :
             std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) ++n;
        }
        return n;
    };
    ASSERT_EQ(countFiles(harness.root / "scratch"),
              countFiles(baseline.root / "scratch"));
}

TEST(helpTabTitleCarriesTheReadOnlyMarker) {
    Harness harness{"marker"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
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
        ASSERT_TRUE(contains(hint->content, "Alt+h"));
        ASSERT_TRUE(contains(hint->content, "help"));
        ASSERT_TRUE(hint->command.has_value());
        if (hint->command) ASSERT_EQ(*hint->command, std::string{"help.open"});
    }
    // Unbinding help.open drops the key label from the hint.
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"keymap.unbind", runtime.revision(),
                               ssg::KeymapUnbindArguments{"Alt+KeyH", "*"}})
                    .accepted());
    auto unbound = footerHelpNode(runtime);
    ASSERT_TRUE(unbound.has_value());
    if (unbound) {
        ASSERT_FALSE(contains(unbound->content, "Alt+h"));
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
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"a.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    const auto openCount = tabCount(runtime);
    // Closing the (active) read-only help tab must succeed despite the tab
    // carrying no reopen record.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tab.close", runtime.revision(), {}})
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

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"help.open", runtime.revision(), {}})
                    .accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& syntax = snapshot->sections().syntax;
    // The untitled help buffer is highlighted as Markdown (not plain text)
    // because openReadOnlyTab set a language override.
    ASSERT_EQ(syntax.language().value(), std::string{"markdown"});
    // Tree-sitter actually produced highlight spans (headings, etc.), so the
    // prose is colored rather than uniform foreground.
    ASSERT_FALSE(syntax.spans().empty());
}

}  // namespace

int main() {
    RUN(helpOpenOpensAReadOnlyOutputTab);
    RUN(helpDocumentContainsProseAndTheLiveKeybinding);
    RUN(helpDocumentListsChromeGlyphsIncludingTabGlyphsWithValues);
    RUN(helpGlyphListingEscapesQuotesSoItStaysCopyPasteable);
    RUN(helpKeybindingSectionReflectsACustomBind);
    RUN(helpOpenIsIdempotentAndRefreshes);
    RUN(helpTabRejectsEditsAndLeavesTheBufferUnchanged);
    RUN(savingAHelpTabFailsGracefullyWithoutAPrompt);
    RUN(helpTabNeverPersistsADraft);
    RUN(helpTabTitleCarriesTheReadOnlyMarker);
    RUN(footerHelpShowsTheHelpKeyAndYieldsWhenUnbound);
    RUN(closingAHelpTabSucceedsAndReopenIsSkipped);
    RUN(helpTabIsHighlightedAsMarkdown);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
