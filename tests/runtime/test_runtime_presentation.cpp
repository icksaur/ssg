#include "../test_helpers.h"

#include <ssg/CommandCatalog.h>
#include <ssg/EditorRuntime.h>
#include <ssg/EditorSessionBuilder.h>
#include <ssg/Keymap.h>
#include <ssg/Renderer.h>
#include <ssg/Settings.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

std::filesystem::path uniqueRoot() {
    auto root = std::filesystem::current_path() / "runtime_presentation";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream out{root / "workspace" / "long.txt"};
    for (int line = 0; line < 80; ++line) out << "line " << line << "\n";
    return root;
}

void openLiveDiffTabForLongTxt(ssg::EditorRuntime& runtime) {
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"long.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{1},
                         .baselineIdentity = "head-z:index-1",
                         .files = {{.id = ssg::DiffFileId{"long-id"},
                                    .path = "long.txt",
                                    .baselineContent = std::string{"before\n"},
                                    .workingContent = std::string{"after\n"}}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.select_next", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate", runtime.revision(), {}})
                    .accepted());
}

TEST(viewportShellSettingsAndThemeAreLiveSections) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"long.txt"}}).accepted());

    auto before = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->client().viewport.firstVisualRow, 0U);
    ASSERT_EQ(before->sections().theme.palette.size(), ssg::kThemePaletteSize);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{5}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(after.has_value());
    ASSERT_EQ(after->client().viewport.firstVisualRow, 5U);
    ASSERT_TRUE(after->sections().shell.panel.has_value());
}

TEST(settingsDispatchMatchesSettingsModelOracleSnapshot) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ssg::SettingsModel oracle;
    auto setTheme = ssg::SettingSetArguments{
        ssg::SettingScope::Workspace, ssg::SettingKey::Theme,
        ssg::SettingValue{std::string{"dark"}}};
    ASSERT_TRUE(oracle.set(setTheme.scope, setTheme.key, setTheme.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), setTheme}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    auto expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto setWrap = ssg::SettingSetArguments{
        ssg::SettingScope::User, ssg::SettingKey::WordWrap,
        ssg::SettingValue{true}};
    ASSERT_TRUE(oracle.set(setWrap.scope, setWrap.key, setWrap.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), setWrap}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto resetTheme = ssg::SettingResetArguments{
        ssg::SettingScope::Workspace, ssg::SettingKey::Theme};
    ASSERT_TRUE(oracle.reset(resetTheme.scope, resetTheme.key).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.reset", runtime.revision(), resetTheme}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto setKeymap = ssg::SettingSetArguments{
        ssg::SettingScope::Workspace, ssg::SettingKey::Keymap,
        ssg::SettingValue{std::string{"vim"}}};
    ASSERT_TRUE(oracle.set(setKeymap.scope, setKeymap.key, setKeymap.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), setKeymap}).accepted());
    ASSERT_TRUE(oracle.reset(ssg::SettingScope::Workspace, ssg::SettingKey::Keymap).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.reset_scope", runtime.revision(), ssg::SettingResetScopeArguments{ssg::SettingScope::Workspace}}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);
}

TEST(editorScrollUsesTheRealPaneHeightNotAHardcoded24) {
    auto root = std::filesystem::current_path() / "runtime_presentation_scroll";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"tall.txt"}}).accepted());

    // A 40-row terminal (NOT 24): a page is the real pane content height.
    const ssg::ViewportDimensions dims{80, 40};
    auto snap0 = runtime.snapshot(ssg::ClientId{1}, dims);  // populate the cache
    ASSERT_TRUE(snap0.has_value());
    if (!snap0) return;
    auto const paneRows = static_cast<std::uint32_t>(snap0->sections().shell.panes.front().content.height);
    ASSERT_TRUE(paneRows != 24);  // the whole point: not the hardcoded value

    // PageDown advances by the real pane height, not 24.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_pages", runtime.revision(), ssg::ScrollPagesArguments{1}}).accepted());
    auto afterPage = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(afterPage.has_value());
    if (!afterPage) return;
    ASSERT_EQ(afterPage->client().viewport.firstVisualRow, paneRows);

    // Scroll-to-fraction(1/1) reaches the REAL maximum for this terminal (the last
    // line becomes visible), not the 24-row-derived maximum.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_to_fraction", runtime.revision(), ssg::ScrollFractionArguments{1, 1}}).accepted());
    auto afterBottom = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(afterBottom.has_value());
    if (!afterBottom) return;
    ASSERT_EQ(afterBottom->client().viewport.firstVisualRow,
              afterBottom->client().viewport.scrollbar.maximumFirstRow);
    std::filesystem::remove_all(root);
}

} // namespace


TEST(paletteCandidatesMatchTheCommandRegistry) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    // Candidates are published only for the picker that is actually open, so a
    // closed palette publishes none.
    auto closed = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(closed.has_value());
    if (closed) ASSERT_TRUE(closed->sections().palette.candidates.empty());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    auto const& palette = snapshot->sections().palette;
    ASSERT_TRUE(palette.mode == ssg::SearchMode::Command);
    // Every registered command appears exactly once as a candidate, compared
    // against the runtime's own catalog rather than the static table -- which
    // is only part of the catalog while commands are migrating out of it.
    auto const catalog = runtime.commandCatalog();
    auto const descriptors = catalog->commands();
    ASSERT_EQ(palette.candidates.size(), descriptors.size());
    std::set<std::string> candidateIds;
    for (auto const& candidate : palette.candidates) {
        ASSERT_FALSE(candidate.label.empty());
        candidateIds.insert(candidate.id);
    }
    for (auto const* descriptor : descriptors) {
        ASSERT_TRUE(candidateIds.contains(descriptor->id));
    }
}

TEST(paletteCommandCandidatesAreCachedButInvalidateOnKeymapChange) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());

    auto const detailOf = [](ssg::SessionSnapshot const& snap, std::string_view id) {
        for (auto const& c : snap.sections().palette.candidates) {
            if (c.id == id) return c.detail;
        }
        return std::string{};
    };

    // Two snapshots with no catalog/keymap change publish the identical
    // candidate set (the cache is reused, not rebuilt into something different).
    auto first = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    auto second = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(first.has_value() && second.has_value());
    if (!first || !second) return;
    ASSERT_EQ(first->sections().palette.candidates,
              second->sections().palette.candidates);
    ASSERT_EQ(detailOf(*first, "file.save"), std::string{"Alt+S"});

    // Rebinding a command must invalidate the cache: the new key hint shows up.
    ASSERT_TRUE(runtime.dispatch(
                       ssg::ClientId{1},
                       {"keymap.bind", runtime.revision(),
                        ssg::KeymapBindArguments{"Alt+KeyG", "file.save", "*"}})
                    .accepted());
    auto rebound = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(rebound.has_value());
    if (rebound) ASSERT_EQ(detailOf(*rebound, "file.save"), std::string{"Alt+G"});
}

TEST(shellStatusFieldsUseRegisteredProviders) {
    auto root = uniqueRoot();
    ssg::EditorRuntimeConfig config{
        root / "workspace", root / "scratch", root / "recovery"};
    config.statusFieldProviders.push_back(
        {"status", [](const ssg::StatusFieldProviderContext&) {
             return std::optional<std::string>{"OVERRIDDEN"};
         }});
    auto created = ssg::EditorRuntime::create(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"long.txt"}})
                    .accepted());

    auto snapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    const auto* statusField = [&]() -> const ssg::AccessibilityNode* {
        for (const auto& node : snapshot->sections().shell.accessibilityNodes) {
            if (node.kind == ssg::ShellNodeKind::FooterField &&
                node.id == "status") {
                return &node;
            }
        }
        return nullptr;
    }();
    ASSERT_TRUE(statusField != nullptr);
    if (statusField) {
        ASSERT_EQ(statusField->content, std::string{"OVERRIDDEN"});
    }
}

TEST(shellStatusFieldsPreserveDefaultContentOrderAndLabels) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"long.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"settings.export_workspace", runtime.revision(),
                               {}})
                    .accepted());

    auto snapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    std::vector<const ssg::AccessibilityNode*> headerFields;
    std::vector<const ssg::AccessibilityNode*> footerFields;
    for (const auto& node : snapshot->sections().shell.accessibilityNodes) {
        if (node.kind == ssg::ShellNodeKind::HeaderField &&
            (node.id == "path" || node.id == "branch")) {
            headerFields.push_back(&node);
        }
        if (node.kind == ssg::ShellNodeKind::FooterField &&
            (node.id == "status" || node.id == "follow")) {
            footerFields.push_back(&node);
        }
    }

    ASSERT_EQ(headerFields.size(), std::size_t{1});
    ASSERT_EQ(headerFields[0]->id, std::string{"path"});
    ASSERT_EQ(headerFields[0]->label, std::string{"Path"});
    ASSERT_EQ(headerFields[0]->content, runtime.workspaceRoot().string());

    ASSERT_EQ(snapshot->sections().tabs.tabs.size(), std::size_t{1});
    ASSERT_EQ(snapshot->sections().tabs.tabs.front().label, std::string{"long.txt"});

    ASSERT_EQ(footerFields.size(), std::size_t{2});
    ASSERT_EQ(footerFields[0]->id, std::string{"status"});
    ASSERT_EQ(footerFields[0]->label, std::string{"Status"});
    ASSERT_TRUE(footerFields[0]->content.starts_with("schema=1\n"));
    ASSERT_EQ(footerFields[1]->id, std::string{"follow"});
    ASSERT_EQ(footerFields[1]->label, std::string{"Follow edits"});
    ASSERT_EQ(footerFields[1]->content, std::string{"following"});
}

TEST(shellStatusFieldsRenderBranchWhenGitBranchIsApplied) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());

    ssg::GitDiffScan scan;
    scan.revision = ssg::Revision{1};
    scan.currentBranch = std::string{"main"};
    ASSERT_TRUE(runtime.applyGitDiffScan(std::move(scan)).accepted());
    auto snapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto* branchField = [&]() -> const ssg::AccessibilityNode* {
        for (const auto& node : snapshot->sections().shell.accessibilityNodes) {
            if (node.kind == ssg::ShellNodeKind::HeaderField &&
                node.id == "branch") {
                return &node;
            }
        }
        return nullptr;
    }();
    ASSERT_TRUE(branchField != nullptr);
    if (branchField) {
        ASSERT_EQ(branchField->label, std::string{"Branch"});
        ASSERT_EQ(branchField->content, std::string{"\xE2\x8E\x87 main"});
    }
}

TEST(liveDiffTabTitlePrefixesGlyphWithoutChangingDocumentTabs) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());

    openLiveDiffTabForLongTxt(runtime);
    auto snapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    std::optional<std::string> documentTitle;
    std::optional<std::string> liveDiffTitle;
    for (const auto& node : snapshot->sections().shell.accessibilityNodes) {
        if (node.kind != ssg::ShellNodeKind::Tab) {
            continue;
        }
        if (node.content == "long.txt") {
            documentTitle = node.content;
        }
        if (node.content == "D long.txt") {
            liveDiffTitle = node.content;
        }
    }
    ASSERT_TRUE(documentTitle.has_value());
    ASSERT_TRUE(liveDiffTitle.has_value());
}

TEST(liveDiffTabGlyphColorTracksThemePalette) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());

    openLiveDiffTabForLongTxt(runtime);
    auto darkSnapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(darkSnapshot.has_value());
    if (!darkSnapshot) return;
    std::optional<ssg::TabId> documentTabId;
    for (const auto& tab : darkSnapshot->sections().tabs.tabs) {
        if (tab.kind == ssg::TabKind::Document) {
            documentTabId = tab.id;
            break;
        }
    }
    ASSERT_TRUE(documentTabId.has_value());
    if (!documentTabId) return;
    auto darkGrid = ssg::Renderer{}.render(*darkSnapshot);
    const auto* darkLiveTab = [&]() -> const ssg::AccessibilityNode* {
        for (const auto& node : darkSnapshot->sections().shell.accessibilityNodes) {
            if (node.kind == ssg::ShellNodeKind::Tab &&
                node.content.starts_with("D ")) {
                return &node;
            }
        }
        return nullptr;
    }();
    ASSERT_TRUE(darkLiveTab != nullptr);
    if (!darkLiveTab) return;
    const auto darkCell = darkGrid.at(darkLiveTab->rect.x, darkLiveTab->rect.y);
    const auto darkColor = darkGrid.palette[darkCell.foreground];

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.activate", runtime.revision(),
                               *documentTabId})
                    .accepted());
    auto inactiveSnapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(inactiveSnapshot.has_value());
    if (!inactiveSnapshot) return;
    auto inactiveGrid = ssg::Renderer{}.render(*inactiveSnapshot);
    const auto* inactiveLiveTab = [&]() -> const ssg::AccessibilityNode* {
        for (const auto& node :
             inactiveSnapshot->sections().shell.accessibilityNodes) {
            if (node.kind == ssg::ShellNodeKind::Tab &&
                node.content.starts_with("D ")) {
                return &node;
            }
        }
        return nullptr;
    }();
    ASSERT_TRUE(inactiveLiveTab != nullptr);
    if (!inactiveLiveTab) return;
    const auto inactiveCell = inactiveGrid.at(inactiveLiveTab->rect.x,
                                              inactiveLiveTab->rect.y);
    const auto inactiveColor = inactiveGrid.palette[inactiveCell.foreground];
    ASSERT_TRUE(darkColor != inactiveColor);
}

TEST(panelShowCommandsToggleAndSwitchProviders) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());

    auto providerLabel = [&](ssg::SessionSnapshot const& snapshot) {
        for (const auto& node : snapshot.sections().shell.accessibilityNodes) {
            if (node.kind == ssg::ShellNodeKind::PanelProvider &&
                node.id == "panel.provider") {
                return node.content;
            }
        }
        return std::string{};
    };

    auto snapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->sections().shell.panel.has_value());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->sections().shell.panel.has_value());
    ASSERT_EQ(providerLabel(*snapshot), std::string{"Files"});

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_FALSE(snapshot->sections().shell.panel.has_value());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->sections().shell.panel.has_value());
    ASSERT_EQ(providerLabel(*snapshot), std::string{"Files"});

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->sections().shell.panel.has_value());
    ASSERT_EQ(providerLabel(*snapshot), std::string{"Git"});

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->sections().shell.panel.has_value());
    ASSERT_EQ(providerLabel(*snapshot), std::string{"Files"});

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->sections().shell.panel.has_value());
    ASSERT_EQ(providerLabel(*snapshot), std::string{"Git"});

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_FALSE(snapshot->sections().shell.panel.has_value());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->sections().shell.panel.has_value());
    ASSERT_EQ(providerLabel(*snapshot), std::string{"Files"});

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                {"panel.show_files", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_FALSE(snapshot->sections().shell.panel.has_value());
}

int main() {
    RUN(viewportShellSettingsAndThemeAreLiveSections);
    RUN(settingsDispatchMatchesSettingsModelOracleSnapshot);
    RUN(paletteCandidatesMatchTheCommandRegistry);
    RUN(paletteCommandCandidatesAreCachedButInvalidateOnKeymapChange);
    RUN(shellStatusFieldsUseRegisteredProviders);
    RUN(shellStatusFieldsPreserveDefaultContentOrderAndLabels);
    RUN(shellStatusFieldsRenderBranchWhenGitBranchIsApplied);
    RUN(liveDiffTabTitlePrefixesGlyphWithoutChangingDocumentTabs);
    RUN(liveDiffTabGlyphColorTracksThemePalette);
    RUN(panelShowCommandsToggleAndSwitchProviders);
    RUN(editorScrollUsesTheRealPaneHeightNotAHardcoded24);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
