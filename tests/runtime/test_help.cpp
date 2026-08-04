#include "../test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/Keymap.h>
#include <ssg/ShellState.h>
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

ssg::EditorRuntimeConfig configFor(const std::filesystem::path& root) {
    ssg::EditorRuntimeConfig config{
        root / "workspace", root / "scratch", root / "recovery"};
    config.enableGitDiffWorker = false;
    return config;
}

struct Harness {
    std::filesystem::path root;
    ssg::EditorRuntimeCreateResult created;
    ssg::EditorRuntime* runtime = nullptr;

    explicit Harness(std::string_view name) : root(uniqueRoot(name)),
        created(ssg::EditorRuntime::create(configFor(root))) {
        if (created.accepted()) {
            runtime = created.runtime.get();
            (void)runtime->attach(
                {ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                ssg::ViewId{1});
        }
    }
    ~Harness() {
        created.runtime.reset();
        std::error_code code;
        std::filesystem::remove_all(root, code);
    }
};

std::optional<ssg::TabState> activeTab(const ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    if (!snapshot || !snapshot->sections().tabs.active) return std::nullopt;
    for (const auto& tab : snapshot->sections().tabs.tabs) {
        if (tab.id == *snapshot->sections().tabs.active) return tab;
    }
    return std::nullopt;
}

std::size_t tabCount(const ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    return snapshot ? snapshot->sections().tabs.tabs.size() : 0;
}

// The rendered tab title (composedTabTitle) for the active tab, read from the
// shell layout's Tab node whose id matches the active tab index.
std::string activeTabNodeContent(const ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {120, 24});
    if (!snapshot) return {};
    std::string content;
    for (const auto& node : snapshot->sections().shell.accessibilityNodes) {
        if (node.kind == ssg::ShellNodeKind::Tab && !node.content.empty()) {
            content = node.content;  // single tab in these tests
        }
    }
    return content;
}

std::optional<ssg::AccessibilityNode> footerHintNode(
    const ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {120, 24});
    if (!snapshot) return std::nullopt;
    for (const auto& node : snapshot->sections().shell.accessibilityNodes) {
        if (node.kind == ssg::ShellNodeKind::FooterHint) return node;
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
    ASSERT_EQ(tab->label, std::string{"Help"});
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
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_FALSE(snapshot->sections().promptStatus.prompt.has_value());
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
    ASSERT_TRUE(contains(activeTabNodeContent(runtime), "(readonly)"));
}

TEST(footerHintShowsTheHelpKeyAndYieldsWhenUnbound) {
    Harness harness{"hint"};
    ASSERT_TRUE(harness.runtime != nullptr);
    if (!harness.runtime) return;
    auto& runtime = *harness.runtime;

    auto hint = footerHintNode(runtime);
    ASSERT_TRUE(hint.has_value());
    if (hint) {
        ASSERT_TRUE(contains(hint->content, "Alt+h"));
        ASSERT_TRUE(contains(hint->content, "Help"));
        ASSERT_TRUE(hint->commandId.has_value());
        if (hint->commandId) ASSERT_EQ(*hint->commandId, std::string{"help.open"});
    }
    // Unbinding help.open drops the key label from the hint.
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"keymap.unbind", runtime.revision(),
                               ssg::KeymapUnbindArguments{"Alt+KeyH", "*"}})
                    .accepted());
    auto unbound = footerHintNode(runtime);
    ASSERT_TRUE(unbound.has_value());
    if (unbound) {
        ASSERT_FALSE(contains(unbound->content, "Alt+h"));
        ASSERT_TRUE(contains(unbound->content, "Help"));
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
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 40});
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
    RUN(footerHintShowsTheHelpKeyAndYieldsWhenUnbound);
    RUN(closingAHelpTabSucceedsAndReopenIsSkipped);
    RUN(helpTabIsHighlightedAsMarkdown);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
