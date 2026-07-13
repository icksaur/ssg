#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/file_commands.h>
#include <ssg/session_snapshot.h>
#include <ssg/text_encoding.h>
#include <ssg/text_input_commands.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path unique_root(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_files_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorRuntimeConfig config_for(const std::filesystem::path& root) {
    return {root / "workspace", root / "scratch", root / "recovery"};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const std::filesystem::path& path, std::initializer_list<std::uint8_t> bytes) {
    std::ofstream output{path, std::ios::binary};
    for (auto byte : bytes) output.put(static_cast<char>(byte));
}

TEST(open_edit_save_round_trips_real_disk_bytes) {
    auto root = unique_root("round_trip");
    {
        std::ofstream output{root / "workspace" / "note.txt", std::ios::binary};
        output << "hello";
    }

    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    auto open = runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}});
    ASSERT_TRUE(open.accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"hello"});

    auto insert = runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}});
    ASSERT_TRUE(insert.accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"!hello"});

    auto save = runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}});
    ASSERT_TRUE(save.accepted());
    ASSERT_EQ(read_text(root / "workspace" / "note.txt"), std::string{"!hello"});
}

TEST(dropped_content_requires_real_capability) {
    auto root = unique_root("drop");
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::websocket}, ssg::ViewId{1}).accepted());

    auto denied = runtime.dispatch(ssg::ClientId{1}, {"file.open_dropped_content", runtime.revision(), ssg::DroppedContentArguments{{'a'}, "a.txt"}});
    ASSERT_FALSE(denied.accepted());

    ASSERT_TRUE(runtime.attach({ssg::ClientId{2}, ssg::InvocationOrigin::websocket, {ssg::CapabilityId{"local_file_drop"}}}, ssg::ViewId{1}).accepted());
    auto accepted = runtime.dispatch(ssg::ClientId{2}, {"file.open_dropped_content", runtime.revision(), ssg::DroppedContentArguments{{'a'}, "a.txt"}});
    ASSERT_TRUE(accepted.accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"a"});
}

TEST(encoding_dispatch_matches_encode_oracle_and_saved_bytes) {
    auto root = unique_root("encoding_save");
    {
        std::ofstream output{root / "workspace" / "note.txt", std::ios::binary};
        output << "one\ntwo";
    }

    auto original = read_bytes(root / "workspace" / "note.txt");
    auto decoded = ssg::decode_text(original);
    ASSERT_TRUE(decoded.accepted());
    decoded.text->utf8 = "one\ntwo\n";
    decoded.text->line_terminators = {ssg::LineTerminator::crlf, ssg::LineTerminator::crlf};
    decoded.text->status.encoding = ssg::TextEncoding::utf8_bom;
    decoded.text->status.had_bom = true;
    decoded.text->status.line_ending = ssg::LineEnding::crlf;
    decoded.text->status.final_newline = true;
    auto expected = ssg::encode_text(*decoded.text);
    ASSERT_TRUE(expected.accepted());

    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_encoding", runtime.revision(), ssg::SetEncodingArguments{ssg::TextEncoding::utf8_bom}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_line_ending", runtime.revision(), ssg::SetLineEndingArguments{ssg::LineEnding::crlf}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_final_newline", runtime.revision(), ssg::SetFinalNewlineArguments{true}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().text_encoding.status, decoded.text->status);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}}).accepted());
    ASSERT_EQ(read_bytes(root / "workspace" / "note.txt"), expected.bytes);
}

TEST(reopen_with_encoding_dispatch_redecodes_real_file_bytes) {
    auto root = unique_root("reopen_encoding");
    write_bytes(root / "workspace" / "latin.txt", {0xe9, 0x0d});

    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"latin.txt"}}).accepted());

    auto reopened = runtime.dispatch(ssg::ClientId{1}, {"file.reopen_with_encoding", runtime.revision(), ssg::ReopenWithEncodingArguments{ssg::TextEncoding::iso88591}});
    ASSERT_TRUE(reopened.accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"\xC3\xA9\n"});
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().text_encoding.status.encoding, ssg::TextEncoding::iso88591);
    ASSERT_EQ(snapshot->sections().text_encoding.status.line_ending, ssg::LineEnding::cr);
}

TEST(closing_the_last_tab_clears_the_editor_document) {
    auto root = unique_root("close_last_tab");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    auto tab_count = [&] {
        return runtime.snapshot(ssg::ClientId{1}, {80, 24})->sections().tabs.tabs.size();
    };

    // Open two files: two tabs, the active document shows content.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    ASSERT_EQ(tab_count(), std::size_t{2});
    ASSERT_EQ(runtime.active_document_text(), std::string{"beta"});

    // Closing one tab switches to the remaining tab's document (still shown).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.close", runtime.revision(), {}}).accepted());
    ASSERT_EQ(tab_count(), std::size_t{1});
    ASSERT_EQ(runtime.active_document_text(), std::string{"alpha"});

    // Closing the last tab must clear the editor document (empty state), not
    // leave a phantom document with no tab.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.close", runtime.revision(), {}}).accepted());
    ASSERT_EQ(tab_count(), std::size_t{0});
    ASSERT_TRUE(runtime.active_document_text().empty());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) ASSERT_TRUE(snapshot->sections().document.text.empty());
}

} // namespace

int main() {
    RUN(open_edit_save_round_trips_real_disk_bytes);
    RUN(dropped_content_requires_real_capability);
    RUN(encoding_dispatch_matches_encode_oracle_and_saved_bytes);
    RUN(reopen_with_encoding_dispatch_redecodes_real_file_bytes);
    RUN(closing_the_last_tab_clears_the_editor_document);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
