#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/file_commands.h>
#include <ssg/keymap.h>
#include <ssg/session_snapshot.h>
#include <ssg/text_encoding.h>
#include <ssg/text_input_commands.h>

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
    return {root / "workspace", root / "scratch", root / "recovery"};
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
    auto decoded = ssg::decodeText(original);
    ASSERT_TRUE(decoded.accepted());
    decoded.text->utf8 = "one\ntwo\n";
    decoded.text->lineTerminators = {ssg::LineTerminator::Crlf, ssg::LineTerminator::Crlf};
    decoded.text->status.encoding = ssg::TextEncoding::Utf8Bom;
    decoded.text->status.hadBom = true;
    decoded.text->status.lineEnding = ssg::LineEnding::Crlf;
    decoded.text->status.finalNewline = true;
    auto expected = ssg::encodeText(*decoded.text);
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
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
