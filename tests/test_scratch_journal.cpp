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

std::vector<std::byte> bytesFromHex(std::string_view text) {
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

std::vector<std::byte> readHexFixture(std::string_view name) {
    const auto path =
        std::filesystem::path{SSG_SCRATCH_JOURNAL_FIXTURE_DIR} / name;
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    return bytesFromHex(text);
}

ssg::UntitledDocumentId fixtureUntitledId() {
    std::array<std::byte, 16> value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::byte>(index);
    }
    return ssg::UntitledDocumentId{value};
}

ssg::JournalDocument savedDocument(std::string content = "hello\n") {
    return {
        ssg::JournalDocumentKey::saved("notes.txt"),
        ssg::DocumentMode::Edit,
        true,
        std::move(content),
    };
}

ssg::JournalDocument untitledDocument(std::string content = "draft") {
    return {
        ssg::JournalDocumentKey::untitled(fixtureUntitledId()),
        ssg::DocumentMode::ReadOnly,
        false,
        std::move(content),
    };
}

TEST(checkpointEncodingMatchesCrossPlatformByteFixture) {
    const ssg::JournalRecoverySet recovery{
        {savedDocument(), untitledDocument()}};

    ASSERT_EQ(ssg::encodeCheckpointRecord(recovery),
              readHexFixture("checkpoint.hex"));
}

TEST(replayAppliesCheckpointUpdatesAndRemovals) {
    std::vector<std::byte> journal =
        ssg::encodeCheckpointRecord({{savedDocument(), untitledDocument()}});
    const auto updated =
        ssg::encodeDocumentRecord(untitledDocument("new draft"));
    journal.insert(journal.end(), updated.begin(), updated.end());
    const auto removed =
        ssg::encodeRemoveRecord(ssg::JournalDocumentKey::saved("notes.txt"));
    journal.insert(journal.end(), removed.begin(), removed.end());

    const auto replayed = ssg::replayJournal(journal);
    ASSERT_FALSE(replayed.discarded_tail);
    ASSERT_EQ(replayed.valid_bytes, journal.size());
    ASSERT_EQ(replayed.recovery.documents.size(), std::size_t{1});
    ASSERT_EQ(replayed.recovery.documents.front(),
              untitledDocument("new draft"));
}

TEST(replayStartsFromNewestCheckpoint) {
    std::vector<std::byte> journal =
        ssg::encodeCheckpointRecord({{savedDocument("old")}});
    const auto update = ssg::encodeDocumentRecord(savedDocument("ignored"));
    journal.insert(journal.end(), update.begin(), update.end());
    const auto checkpoint =
        ssg::encodeCheckpointRecord({{untitledDocument("new base")}});
    journal.insert(journal.end(), checkpoint.begin(), checkpoint.end());

    const auto replayed = ssg::replayJournal(journal);
    ASSERT_EQ(replayed.recovery.documents,
              std::vector<ssg::JournalDocument>{
                  untitledDocument("new base")});
}

TEST(corruptOrTruncatedTailStopsAtLastValidRecord) {
    const auto checkpoint =
        ssg::encodeCheckpointRecord({{savedDocument("base")}});
    const auto update = ssg::encodeDocumentRecord(savedDocument("changed"));
    std::vector<std::byte> complete = checkpoint;
    complete.insert(complete.end(), update.begin(), update.end());

    auto corrupt = complete;
    corrupt.back() ^= std::byte{0x80};
    const auto corrupt_replay = ssg::replayJournal(corrupt);
    ASSERT_TRUE(corrupt_replay.discarded_tail);
    ASSERT_EQ(corrupt_replay.valid_bytes, checkpoint.size());
    ASSERT_EQ(corrupt_replay.recovery.documents,
              std::vector<ssg::JournalDocument>{savedDocument("base")});

    for (std::size_t cut = checkpoint.size() + 1; cut < complete.size();
         ++cut) {
        const auto truncated =
            ssg::replayJournal(std::span{complete}.first(cut));
        ASSERT_TRUE(truncated.discarded_tail);
        ASSERT_EQ(truncated.valid_bytes, checkpoint.size());
        ASSERT_EQ(truncated.recovery.documents,
                  std::vector<ssg::JournalDocument>{savedDocument("base")});
    }
}

TEST(malformedInputFailsClosedWithoutAllocationOrState) {
    std::vector<std::byte> malformed(16, std::byte{0xff});
    const auto replayed = ssg::replayJournal(malformed);
    ASSERT_TRUE(replayed.discarded_tail);
    ASSERT_EQ(replayed.valid_bytes, std::size_t{0});
    ASSERT_TRUE(replayed.recovery.documents.empty());

    auto oversized = ssg::encodeCheckpointRecord({{}});
    oversized[6] = std::byte{0xff};
    oversized[7] = std::byte{0xff};
    oversized[8] = std::byte{0xff};
    oversized[9] = std::byte{0x7f};
    ASSERT_TRUE(ssg::replayJournal(oversized).recovery.documents.empty());
}

TEST(untitledIdsAreNonzeroUniqueAndStableValues) {
    const auto first = ssg::UntitledDocumentId::generate();
    const auto second = ssg::UntitledDocumentId::generate();
    ASSERT_NE(first, second);
    ASSERT_FALSE(std::all_of(first.bytes().begin(), first.bytes().end(),
                             [](std::byte value) {
                                 return value == std::byte{0};
                             }));
    ASSERT_EQ(ssg::JournalDocumentKey::untitled(first).untitledId(), first);
}

TEST(durableAppendSurvivesCloseAndRestartReplay) {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "document.journal";
    {
        ssg::ScratchJournal journal{path};
        journal.appendCheckpoint({{savedDocument("base")}});
        journal.appendDocument(savedDocument("after restart"));
    }

    const ssg::ScratchJournal reopened{path};
    const auto replayed = reopened.replay();
    ASSERT_FALSE(replayed.discarded_tail);
    ASSERT_EQ(replayed.recovery.documents,
              std::vector<ssg::JournalDocument>{
                  savedDocument("after restart")});
}

TEST(appendRequiresSessionLayerToCreateParentDirectory) {
    TemporaryDirectory temporary;
    const ssg::ScratchJournal journal{
        temporary.path() / "missing" / "document.journal"};
    ASSERT_THROWS(journal.appendCheckpoint({{savedDocument()}}),
                  std::invalid_argument);
}

TEST(appendRejectsInvalidSavedIdentityAndInvalidUtf8) {
    ASSERT_THROWS(ssg::JournalDocumentKey::saved("../escape"),
                  std::invalid_argument);
    ASSERT_THROWS(ssg::JournalDocumentKey::saved(""), std::invalid_argument);
    ASSERT_THROWS(
        ssg::encodeDocumentRecord(
            {ssg::JournalDocumentKey::saved("valid"),
             ssg::DocumentMode::Edit, true, std::string{"bad\xff", 4}}),
        std::invalid_argument);
}

} // namespace

int main() {
    RUN(checkpointEncodingMatchesCrossPlatformByteFixture);
    RUN(replayAppliesCheckpointUpdatesAndRemovals);
    RUN(replayStartsFromNewestCheckpoint);
    RUN(corruptOrTruncatedTailStopsAtLastValidRecord);
    RUN(malformedInputFailsClosedWithoutAllocationOrState);
    RUN(untitledIdsAreNonzeroUniqueAndStableValues);
    RUN(durableAppendSurvivesCloseAndRestartReplay);
    RUN(appendRequiresSessionLayerToCreateParentDirectory);
    RUN(appendRejectsInvalidSavedIdentityAndInvalidUtf8);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
