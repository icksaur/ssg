#include "ssg/scratch_journal.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("ssg-scratch-journal-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> bytes_from_hex(std::string_view text) {
    auto nibble = [](char value) -> unsigned {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        throw std::invalid_argument("invalid fixture hex digit");
    };

    std::vector<std::byte> result;
    unsigned high = 0;
    bool have_high = false;
    for (const char value : text) {
        if (value == '\n' || value == '\r' || value == ' ' || value == '\t') {
            continue;
        }
        if (!have_high) {
            high = nibble(value);
            have_high = true;
        } else {
            result.push_back(
                static_cast<std::byte>((high << 4U) | nibble(value)));
            have_high = false;
        }
    }
    if (have_high) throw std::invalid_argument("odd fixture hex length");
    return result;
}

std::vector<std::byte> read_hex_fixture(std::string_view name) {
    const auto path =
        std::filesystem::path{SSG_SCRATCH_JOURNAL_FIXTURE_DIR} / name;
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    return bytes_from_hex(text);
}

ssg::UntitledDocumentId fixture_untitled_id() {
    std::array<std::byte, 16> value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::byte>(index);
    }
    return ssg::UntitledDocumentId{value};
}

ssg::JournalDocument saved_document(std::string content = "hello\n") {
    return {
        ssg::JournalDocumentKey::saved("notes.txt"),
        ssg::DocumentMode::edit,
        true,
        std::move(content),
    };
}

ssg::JournalDocument untitled_document(std::string content = "draft") {
    return {
        ssg::JournalDocumentKey::untitled(fixture_untitled_id()),
        ssg::DocumentMode::read_only,
        false,
        std::move(content),
    };
}

TEST(checkpoint_encoding_matches_cross_platform_byte_fixture) {
    const ssg::JournalRecoverySet recovery{
        {saved_document(), untitled_document()}};

    ASSERT_EQ(ssg::encode_checkpoint_record(recovery),
              read_hex_fixture("checkpoint.hex"));
}

TEST(replay_applies_checkpoint_updates_and_removals) {
    std::vector<std::byte> journal =
        ssg::encode_checkpoint_record({{saved_document(), untitled_document()}});
    const auto updated =
        ssg::encode_document_record(untitled_document("new draft"));
    journal.insert(journal.end(), updated.begin(), updated.end());
    const auto removed =
        ssg::encode_remove_record(ssg::JournalDocumentKey::saved("notes.txt"));
    journal.insert(journal.end(), removed.begin(), removed.end());

    const auto replayed = ssg::replay_journal(journal);
    ASSERT_FALSE(replayed.discarded_tail);
    ASSERT_EQ(replayed.valid_bytes, journal.size());
    ASSERT_EQ(replayed.recovery.documents.size(), std::size_t{1});
    ASSERT_EQ(replayed.recovery.documents.front(),
              untitled_document("new draft"));
}

TEST(replay_starts_from_newest_checkpoint) {
    std::vector<std::byte> journal =
        ssg::encode_checkpoint_record({{saved_document("old")}});
    const auto update = ssg::encode_document_record(saved_document("ignored"));
    journal.insert(journal.end(), update.begin(), update.end());
    const auto checkpoint =
        ssg::encode_checkpoint_record({{untitled_document("new base")}});
    journal.insert(journal.end(), checkpoint.begin(), checkpoint.end());

    const auto replayed = ssg::replay_journal(journal);
    ASSERT_EQ(replayed.recovery.documents,
              std::vector<ssg::JournalDocument>{
                  untitled_document("new base")});
}

TEST(corrupt_or_truncated_tail_stops_at_last_valid_record) {
    const auto checkpoint =
        ssg::encode_checkpoint_record({{saved_document("base")}});
    const auto update = ssg::encode_document_record(saved_document("changed"));
    std::vector<std::byte> complete = checkpoint;
    complete.insert(complete.end(), update.begin(), update.end());

    auto corrupt = complete;
    corrupt.back() ^= std::byte{0x80};
    const auto corrupt_replay = ssg::replay_journal(corrupt);
    ASSERT_TRUE(corrupt_replay.discarded_tail);
    ASSERT_EQ(corrupt_replay.valid_bytes, checkpoint.size());
    ASSERT_EQ(corrupt_replay.recovery.documents,
              std::vector<ssg::JournalDocument>{saved_document("base")});

    for (std::size_t cut = checkpoint.size() + 1; cut < complete.size();
         ++cut) {
        const auto truncated =
            ssg::replay_journal(std::span{complete}.first(cut));
        ASSERT_TRUE(truncated.discarded_tail);
        ASSERT_EQ(truncated.valid_bytes, checkpoint.size());
        ASSERT_EQ(truncated.recovery.documents,
                  std::vector<ssg::JournalDocument>{saved_document("base")});
    }
}

TEST(malformed_input_fails_closed_without_allocation_or_state) {
    std::vector<std::byte> malformed(16, std::byte{0xff});
    const auto replayed = ssg::replay_journal(malformed);
    ASSERT_TRUE(replayed.discarded_tail);
    ASSERT_EQ(replayed.valid_bytes, std::size_t{0});
    ASSERT_TRUE(replayed.recovery.documents.empty());

    auto oversized = ssg::encode_checkpoint_record({{}});
    oversized[6] = std::byte{0xff};
    oversized[7] = std::byte{0xff};
    oversized[8] = std::byte{0xff};
    oversized[9] = std::byte{0x7f};
    ASSERT_TRUE(ssg::replay_journal(oversized).recovery.documents.empty());
}

TEST(untitled_ids_are_nonzero_unique_and_stable_values) {
    const auto first = ssg::UntitledDocumentId::generate();
    const auto second = ssg::UntitledDocumentId::generate();
    ASSERT_NE(first, second);
    ASSERT_FALSE(std::all_of(first.bytes().begin(), first.bytes().end(),
                             [](std::byte value) {
                                 return value == std::byte{0};
                             }));
    ASSERT_EQ(ssg::JournalDocumentKey::untitled(first).untitled_id(), first);
}

TEST(durable_append_survives_close_and_restart_replay) {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "document.journal";
    {
        ssg::ScratchJournal journal{path};
        journal.append_checkpoint({{saved_document("base")}});
        journal.append_document(saved_document("after restart"));
    }

    const ssg::ScratchJournal reopened{path};
    const auto replayed = reopened.replay();
    ASSERT_FALSE(replayed.discarded_tail);
    ASSERT_EQ(replayed.recovery.documents,
              std::vector<ssg::JournalDocument>{
                  saved_document("after restart")});
}

TEST(append_requires_session_layer_to_create_parent_directory) {
    TemporaryDirectory temporary;
    const ssg::ScratchJournal journal{
        temporary.path() / "missing" / "document.journal"};
    ASSERT_THROWS(journal.append_checkpoint({{saved_document()}}),
                  std::invalid_argument);
}

TEST(append_rejects_invalid_saved_identity_and_invalid_utf8) {
    ASSERT_THROWS(ssg::JournalDocumentKey::saved("../escape"),
                  std::invalid_argument);
    ASSERT_THROWS(ssg::JournalDocumentKey::saved(""), std::invalid_argument);
    ASSERT_THROWS(
        ssg::encode_document_record(
            {ssg::JournalDocumentKey::saved("valid"),
             ssg::DocumentMode::edit, true, std::string{"bad\xff", 4}}),
        std::invalid_argument);
}

} // namespace

int main() {
    RUN(checkpoint_encoding_matches_cross_platform_byte_fixture);
    RUN(replay_applies_checkpoint_updates_and_removals);
    RUN(replay_starts_from_newest_checkpoint);
    RUN(corrupt_or_truncated_tail_stops_at_last_valid_record);
    RUN(malformed_input_fails_closed_without_allocation_or_state);
    RUN(untitled_ids_are_nonzero_unique_and_stable_values);
    RUN(durable_append_survives_close_and_restart_replay);
    RUN(append_requires_session_layer_to_create_parent_directory);
    RUN(append_rejects_invalid_saved_identity_and_invalid_utf8);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
