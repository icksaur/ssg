#include "../test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/FileCommands.h>
#include <ssg/GitDiffSource.h>
#include <ssg/Keymap.h>
#include <ssg/session_snapshot.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_files_" + std::string{name});
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

std::string readText(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path& path, std::initializer_list<std::uint8_t> bytes) {
    std::ofstream output{path, std::ios::binary};
    for (auto byte : bytes) output.put(static_cast<char>(byte));
}

bool activeTabDirty(const ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    if (!snapshot) return false;
    const auto& tabs = snapshot->sections().tabs;
    if (!tabs.active) return false;
    for (const auto& tab : tabs.tabs) {
        if (tab.id == *tabs.active) return tab.dirty;
    }
    return false;
}

bool activeTabIsLiveDiff(const ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    if (!snapshot) return false;
    const auto& tabs = snapshot->sections().tabs;
    if (!tabs.active) return false;
    for (const auto& tab : tabs.tabs) {
        if (tab.id == *tabs.active) return tab.kind == ssg::TabKind::LiveDiff;
    }
    return false;
}

// Edit note.txt to a dirty draft, flush it, then drop the runtime — leaving a
// recoverable draft in `root/scratch`. Returns the drafted buffer text.
std::string leaveDirtyDraft(const std::filesystem::path& root) {
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    if (!created.accepted()) return {};
    auto& runtime = *created.runtime;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    (void)runtime.dispatch(ssg::ClientId{1},
                           {"file.open", runtime.revision(), std::string{"note.txt"}});
    (void)runtime.dispatch(ssg::ClientId{1},
                           {"text.insert", runtime.revision(),
                            ssg::TextInputArguments{"!"}});
    const auto draft = runtime.activeDocumentText();
    (void)runtime.flushDueAutosaveDrafts();
    return draft;
}

ssg::CommandResult reopenNote(ssg::EditorRuntime& runtime) {
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    return runtime.dispatch(
        ssg::ClientId{1},
        {"file.open", runtime.revision(), std::string{"note.txt"}});
}

TEST(reopeningADirtyDraftRestoresTheEditsWhenDiskIsUnchanged) {
    auto root = uniqueRoot("draft_restore");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());

    // Disk untouched since the edits branched: recover the draft as a dirty
    // buffer with a "restored" notice, no conflict.
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_TRUE(activeTabDirty(runtime));
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorRuntime::DraftReopenNotice::Restored);
}

TEST(reopeningAConvergedDraftDropsItAndOpensClean) {
    auto root = uniqueRoot("draft_converged");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    // The file on disk now already holds exactly the drafted content: the edits
    // converged. There is nothing unsaved to recover.
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << draft;

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_FALSE(activeTabDirty(runtime));
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorRuntime::DraftReopenNotice::None);
    // The draft was dropped, so the clean buffer has nothing to flush.
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{0});
}

TEST(reopeningADraftAfterAnExternalChangeFlagsConflict) {
    auto root = uniqueRoot("draft_conflict");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    // Something other than SSG rewrote the file since the edits branched.
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    // The draft still loads as a dirty buffer (never a blind blocking choice)...
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_TRUE(activeTabDirty(runtime));
    // ...but the conflict notice fires because disk changed.
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorRuntime::DraftReopenNotice::Conflict);
}

TEST(editingAndSavingARestoredDraftRoundTripsCoherently) {
    // Guards the decoded/document coherence seam: a restored draft's `decoded`
    // must match the buffer so a later edit's terminator bookkeeping and a save
    // do not desynchronize. The disk file uses CRLF so the terminator convention
    // is non-trivial.
    auto root = uniqueRoot("draft_edit_after_restore");
    {
        std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
            << "one\r\ntwo\r\n";
        auto created = ssg::EditorRuntime::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.runtime;
        ASSERT_TRUE(reopenNote(runtime).accepted());
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                     {"text.insert", runtime.revision(),
                                      ssg::TextInputArguments{"X"}})
                        .accepted());
        ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});
    }
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorRuntime::DraftReopenNotice::Restored);
    const auto restored = runtime.activeDocumentText();
    // A further edit after restore must apply cleanly and stay dirty...
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"Y"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), "Y" + restored);
    ASSERT_TRUE(activeTabDirty(runtime));
    // ...and a save must write CRLF back to disk, proving decoded's terminator
    // convention survived the restore.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.save", runtime.revision(), {}})
                    .accepted());
    ASSERT_FALSE(activeTabDirty(runtime));
    ASSERT_NE(readText(root / "workspace" / "note.txt").find("\r\n"),
              std::string::npos);
}

TEST(reactivatingAnOpenTabDoesNotReapplyItsDraft) {
    // The freshlyOpened gate: with a draft still present in scratch, switching
    // back to an already-open tab must not re-run reconcile and clobber the live
    // buffer with the stale draft.
    auto root = uniqueRoot("draft_no_reclobber");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "other.txt", std::ios::binary} << "other\n";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    // Edit past the recovered draft, open another file, then re-open note.txt:
    // openFile short-circuits to the already-open document, so reconcile must not
    // run again and must leave the live edited buffer intact.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"Z"}})
                    .accepted());
    const auto live = runtime.activeDocumentText();
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"other.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"note.txt"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), live);
    ASSERT_TRUE(activeTabDirty(runtime));
}

TEST(draftDiffOnAConflictShowsDraftAgainstDiskHunks) {
    auto root = uniqueRoot("draft_diff_conflict");
    const auto draft = leaveDirtyDraft(root);  // "!hi\n"
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorRuntime::DraftReopenNotice::Conflict);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    // The diff is its own derived tab, distinct from the still-dirty draft.
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    // The merged diff view carries the target (draft) content; the disk-only
    // baseline line is projected as a phantom removed row, so the draft's own
    // line is what the diff document text holds.
    const auto diffText = runtime.activeDocumentText();
    ASSERT_NE(diffText.find("!hi"), std::string::npos);
}

TEST(draftDiffWithDiskMissingDiffsDraftAgainstEmpty) {
    auto root = uniqueRoot("draft_diff_missing");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    // The file vanishes after the draft is open: draft.diff must still succeed,
    // diffing the draft against empty rather than failing.
    std::filesystem::remove(root / "workspace" / "note.txt");
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    ASSERT_NE(runtime.activeDocumentText().find("!hi"), std::string::npos);
}

TEST(draftDiffSurvivesAGitScanThatDoesNotMentionTheFile) {
    // A git rescan reconciles only its own entries; the non-git draft-vs-disk
    // entry must not be evicted just because the scan does not list it.
    auto root = uniqueRoot("draft_diff_git_scan");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    // Pause follow-edits so the scan does not auto-navigate to the git-changed
    // file; this test is about the draft entry surviving, not follow behavior.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"follow_edits.pause", runtime.revision(), {}})
                    .accepted());

    // A git scan that finds unrelated changes (not note.txt) arrives.
    ssg::GitDiffScan scan;
    scan.revision = ssg::Revision{1000};
    scan.baselineIdentity = "index-a";
    scan.currentBranch = "main";
    scan.files.push_back({ssg::DiffFileId{"other.cpp"}, "other.cpp",
                          std::nullopt, std::string{"x\n"},
                          std::string{"y\n"}});
    ASSERT_TRUE(runtime.applyGitDiffScan(std::move(scan)).accepted());

    // The draft-vs-disk diff tab is still active and still shows the draft.
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    ASSERT_NE(runtime.activeDocumentText().find("!hi"), std::string::npos);
}

TEST(draftDiffRefusesWhenTheActiveDocumentIsNotASavedFile) {
    // An untitled scratch buffer has no disk side to diff against.
    auto root = uniqueRoot("draft_diff_untitled");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    // The session starts on an empty untitled buffer; draft.diff must refuse it.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"draft.diff", runtime.revision(), {}})
                     .accepted());
}

TEST(openingAFileRevealsTheCaretResettingAStaleScroll) {
    // Reveal-policy audit (doc/spec-scroll.md): opening a document must show the
    // caret, not inherit the previous document's scroll offset. Two tall files.
    auto root = uniqueRoot("open_reveal");
    std::string tall;
    for (int i = 0; i < 100; ++i) tall += "line\n";
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << tall;
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << tall;
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto firstRow = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->client().viewport.firstVisualRow : 0U;
    };

    // Open A and scroll far down (free scroll leaves the caret off-screen above).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);

    // Opening B resets the view so B's caret (its document start) is visible: the
    // stale offset of 50 must not carry over.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    ASSERT_EQ(firstRow(), 0U);
}

TEST(openEditSaveRoundTripsRealDiskBytes) {
    auto root = uniqueRoot("round_trip");
    {
        std::ofstream output{root / "workspace" / "note.txt", std::ios::binary};
        output << "hello";
    }

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto open = runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}});
    ASSERT_TRUE(open.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"hello"});

    auto insert = runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}});
    ASSERT_TRUE(insert.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"!hello"});

    auto save = runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}});
    ASSERT_TRUE(save.accepted());
    ASSERT_EQ(readText(root / "workspace" / "note.txt"), std::string{"!hello"});
}

TEST(droppedContentRequiresRealCapability) {
    auto root = uniqueRoot("drop");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::Websocket}, ssg::ViewId{1}).accepted());

    auto denied = runtime.dispatch(ssg::ClientId{1}, {"file.open_dropped_content", runtime.revision(), ssg::DroppedContentArguments{{'a'}, "a.txt"}});
    ASSERT_FALSE(denied.accepted());

    ASSERT_TRUE(runtime.attach({ssg::ClientId{2}, ssg::InvocationOrigin::Websocket, {ssg::CapabilityId{"local_file_drop"}}}, ssg::ViewId{1}).accepted());
    auto accepted = runtime.dispatch(ssg::ClientId{2}, {"file.open_dropped_content", runtime.revision(), ssg::DroppedContentArguments{{'a'}, "a.txt"}});
    ASSERT_TRUE(accepted.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"a"});
}

TEST(encodingDispatchMatchesEncodeOracleAndSavedBytes) {
    auto root = uniqueRoot("encoding_save");
    {
        std::ofstream output{root / "workspace" / "note.txt", std::ios::binary};
        output << "one\ntwo";
    }

    auto original = readBytes(root / "workspace" / "note.txt");
    auto decoded = ssg::TextCodec{}.decode(original);
    ASSERT_TRUE(decoded.accepted());
    decoded.text->utf8 = "one\ntwo\n";
    decoded.text->lineTerminators = {ssg::LineTerminator::Crlf, ssg::LineTerminator::Crlf};
    decoded.text->status.encoding = ssg::TextEncoding::Utf8Bom;
    decoded.text->status.hadBom = true;
    decoded.text->status.lineEnding = ssg::LineEnding::Crlf;
    decoded.text->status.finalNewline = true;
    auto expected = ssg::TextCodec{}.encode(*decoded.text);
    ASSERT_TRUE(expected.accepted());

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_encoding", runtime.revision(), ssg::SetEncodingArguments{ssg::TextEncoding::Utf8Bom}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_line_ending", runtime.revision(), ssg::SetLineEndingArguments{ssg::LineEnding::Crlf}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_final_newline", runtime.revision(), ssg::SetFinalNewlineArguments{true}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().textEncoding.status, decoded.text->status);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}}).accepted());
    ASSERT_EQ(readBytes(root / "workspace" / "note.txt"), expected.bytes);
}

TEST(reopenWithEncodingDispatchRedecodesRealFileBytes) {
    auto root = uniqueRoot("reopen_encoding");
    writeBytes(root / "workspace" / "latin.txt", {0xe9, 0x0d});

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"latin.txt"}}).accepted());

    auto reopened = runtime.dispatch(ssg::ClientId{1}, {"file.reopen_with_encoding", runtime.revision(), ssg::ReopenWithEncodingArguments{ssg::TextEncoding::Iso88591}});
    ASSERT_TRUE(reopened.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"\xC3\xA9\n"});
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().textEncoding.status.encoding, ssg::TextEncoding::Iso88591);
    ASSERT_EQ(snapshot->sections().textEncoding.status.lineEnding, ssg::LineEnding::Cr);
}

TEST(closingTheLastTabClearsTheEditorDocument) {
    auto root = uniqueRoot("close_last_tab");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto tabCount = [&] {
        return runtime.snapshot(ssg::ClientId{1}, {80, 24})->sections().tabs.tabs.size();
    };

    // Open two files: two tabs, the active document shows content.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    ASSERT_EQ(tabCount(), std::size_t{2});
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"beta"});

    // Closing one tab switches to the remaining tab's document (still shown).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.close", runtime.revision(), {}}).accepted());
    ASSERT_EQ(tabCount(), std::size_t{1});
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"alpha"});

    // Closing the last tab must clear the editor document (empty state), not
    // leave a phantom document with no tab.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.close", runtime.revision(), {}}).accepted());
    ASSERT_EQ(tabCount(), std::size_t{0});
    ASSERT_TRUE(runtime.activeDocumentText().empty());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) ASSERT_TRUE(snapshot->sections().document.text.empty());
}

TEST(tabActivateFocusesTheEditor) {
    auto root = uniqueRoot("tab_activate_focus");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->sections().shell.focus : ssg::FocusTarget::Editor;
    };

    // Move focus to the panel, then activating a tab (a tab click) returns focus
    // to the editor.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.focus", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    auto first = runtime.snapshot(ssg::ClientId{1}, dims)->sections().tabs.tabs.front().id;
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.activate", runtime.revision(), first}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);
}

TEST(switchingTabsRevealsTheNewDocumentsCaret) {
    // Reveal-policy: switching to a different tab shows that document's caret
    // instead of inheriting the previous tab's scroll offset.
    auto root = uniqueRoot("tab_switch_reveal");
    std::string tall;
    for (int i = 0; i < 100; ++i) tall += "line\n";
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << tall;
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << tall;
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto firstRow = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->client().viewport.firstVisualRow : 0U;
    };
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    // B is active; scroll it far down (free scroll leaves B's caret off-screen).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);

    // Switch to A (previous tab): its caret (top) is revealed, not B's stale 50.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.previous", runtime.revision(), {}}).accepted());
    ASSERT_EQ(firstRow(), 0U);

    // Moving a tab keeps the SAME active document and must NOT snap the scroll:
    // switch back to B, scroll away, move the tab, and the offset stays put.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.move_left", runtime.revision(), {}}).accepted());
    ASSERT_EQ(firstRow(), 50U);  // same document -> no reveal snap
}

TEST(closingNonActiveDirtyTabReopensItsOwnContentWithNewDocumentId) {
    auto root = uniqueRoot("close_non_active_dirty");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"a.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"text.insert", runtime.revision(),
                               ssg::TextInputArguments{"!"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"!alpha"});
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"b.txt"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"beta"});

    auto beforeClose = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(beforeClose.has_value());
    if (!beforeClose.has_value()) return;
    std::optional<ssg::TabId> tabA;
    std::optional<ssg::FileDocumentId> documentA;
    for (auto const& tab : beforeClose->sections().tabs.tabs) {
        if (tab.label == "a.txt") {
            tabA = tab.id;
            documentA = tab.document;
            break;
        }
    }
    ASSERT_TRUE(tabA.has_value());
    ASSERT_TRUE(documentA.has_value());
    if (!tabA || !documentA) return;

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.close", runtime.revision(), *tabA})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"beta"});
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.reopen_closed", runtime.revision(), {}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"!alpha"});

    auto afterReopen = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(afterReopen.has_value());
    if (!afterReopen.has_value()) return;
    std::optional<ssg::FileDocumentId> reopenedDocumentA;
    for (auto const& tab : afterReopen->sections().tabs.tabs) {
        if (tab.label == "a.txt") {
            reopenedDocumentA = tab.document;
            break;
        }
    }
    ASSERT_TRUE(reopenedDocumentA.has_value());
    ASSERT_TRUE(*reopenedDocumentA != *documentA);
}

TEST(autosaveFlushesADirtyDocumentEagerlyThenDebounces) {
    auto root = uniqueRoot("autosave_eager");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());

    // A clean, just-opened document is not flushed.
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});

    // Editing makes it dirty; the first tick flushes eagerly (bounds the crash
    // window to one tick).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}}).accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});

    // An immediate second tick with unchanged content is debounced (default
    // interval is 10s, and nothing changed anyway).
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});
}

TEST(autosaveFlushesNothingWhenNoDocumentIsDirty) {
    auto root = uniqueRoot("autosave_clean");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());
    // Edit then save -> clean again -> no autosave.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});
}

TEST(autosaveFlushAllForcesADirtyDocumentAfterAnEagerFlush) {
    auto root = uniqueRoot("autosave_exit");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}}).accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});
    // A clean-exit flush ignores the debounce and writes the dirty draft again,
    // capturing any edits newer than the last tick.
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{1});
    // A clean document is still skipped by the exit flush.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{0});
}

} // namespace

int main() {
    RUN(openEditSaveRoundTripsRealDiskBytes);
    RUN(openingAFileRevealsTheCaretResettingAStaleScroll);
    RUN(droppedContentRequiresRealCapability);
    RUN(encodingDispatchMatchesEncodeOracleAndSavedBytes);
    RUN(reopenWithEncodingDispatchRedecodesRealFileBytes);
    RUN(closingTheLastTabClearsTheEditorDocument);
    RUN(tabActivateFocusesTheEditor);
    RUN(switchingTabsRevealsTheNewDocumentsCaret);
    RUN(closingNonActiveDirtyTabReopensItsOwnContentWithNewDocumentId);
    RUN(autosaveFlushesADirtyDocumentEagerlyThenDebounces);
    RUN(autosaveFlushesNothingWhenNoDocumentIsDirty);
    RUN(autosaveFlushAllForcesADirtyDocumentAfterAnEagerFlush);
    RUN(reopeningADirtyDraftRestoresTheEditsWhenDiskIsUnchanged);
    RUN(reopeningAConvergedDraftDropsItAndOpensClean);
    RUN(reopeningADraftAfterAnExternalChangeFlagsConflict);
    RUN(editingAndSavingARestoredDraftRoundTripsCoherently);
    RUN(reactivatingAnOpenTabDoesNotReapplyItsDraft);
    RUN(draftDiffOnAConflictShowsDraftAgainstDiskHunks);
    RUN(draftDiffWithDiskMissingDiffsDraftAgainstEmpty);
    RUN(draftDiffSurvivesAGitScanThatDoesNotMentionTheFile);
    RUN(draftDiffRefusesWhenTheActiveDocumentIsNotASavedFile);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
