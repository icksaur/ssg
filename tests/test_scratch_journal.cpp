#include <ssg/ScratchJournal.h>
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
    bool haveHigh = false;
    for (const char value : text) {
        if (value == '\n' || value == '\r' || value == ' ' || value == '\t') {
            continue;
        }
        if (!haveHigh) {
            high = nibble(value);
            haveHigh = true;
        } else {
            result.push_back(
                static_cast<std::byte>((high << 4U) | nibble(value)));
            haveHigh = false;
        }
    }
    if (haveHigh) throw std::invalid_argument("odd fixture hex length");
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

// Regenerate a byte fixture when SSG_REGEN_JOURNAL_FIXTURE is set, so a
// deliberate format change reproduces the on-disk hex rather than being
// hand-edited. Returns true when it regenerated (the caller then skips the
// assertion for this run).
bool maybeRegenerateHexFixture(std::string_view name,
                               std::span<const std::byte> bytes) {
    if (std::getenv("SSG_REGEN_JOURNAL_FIXTURE") == nullptr) return false;
    const auto path =
        std::filesystem::path{SSG_SCRATCH_JOURNAL_FIXTURE_DIR} / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    static constexpr char kHex[] = "0123456789abcdef";
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        output << kHex[value >> 4U] << kHex[value & 0x0fU];
    }
    output << '\n';
    return true;
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

    const auto encoded = ssg::encodeJournalCheckpoint(recovery);
    if (maybeRegenerateHexFixture("checkpoint.hex", encoded)) return;
    ASSERT_EQ(encoded, readHexFixture("checkpoint.hex"));
}

ssg::JournalDocument savedDocumentWithBaseline() {
    ssg::JournalDocument document = savedDocument("edited\n");
    document.baseline =
        ssg::DraftBaseline{1234567890123456789ULL, 42ULL,
                           ssg::fastContentHash("hello\n")};
    return document;
}

TEST(baselineRoundTripsThroughDocumentAndCheckpoint) {
    // A single-document record with a baseline replays byte-for-byte.
    const auto document = savedDocumentWithBaseline();
    const auto record = ssg::encodeJournalDocument(document);
    const auto replayed = ssg::replayJournal(record);
    ASSERT_FALSE(replayed.discardedTail);
    ASSERT_EQ(replayed.recovery.documents,
              std::vector<ssg::JournalDocument>{document});
    ASSERT_TRUE(replayed.recovery.documents.front().baseline.has_value());
    ASSERT_EQ(replayed.recovery.documents.front().baseline->size,
              std::uint64_t{42});

    // A baseline also survives inside a checkpoint alongside a baseline-less doc.
    const ssg::JournalRecoverySet recovery{
        {savedDocumentWithBaseline(), untitledDocument()}};
    const auto checkpoint = ssg::encodeJournalCheckpoint(recovery);
    const auto checkpointReplay = ssg::replayJournal(checkpoint);
    ASSERT_FALSE(checkpointReplay.discardedTail);
    ASSERT_EQ(checkpointReplay.recovery, recovery);
    ASSERT_FALSE(checkpointReplay.recovery.documents.back().baseline.has_value());
}

TEST(legacyV1RecordReplaysWithUnknownBaseline) {
    // The v1 fixture predates the baseline field. It must still replay (no torn
    // tail) with each document's baseline == nullopt ("unknown"), never garbage.
    const auto v1 = readHexFixture("checkpoint_v1.hex");
    const auto replayed = ssg::replayJournal(v1);
    ASSERT_FALSE(replayed.discardedTail);
    ASSERT_EQ(replayed.validBytes, v1.size());
    ASSERT_EQ(replayed.recovery.documents.size(), std::size_t{2});
    for (const auto& document : replayed.recovery.documents) {
        ASSERT_FALSE(document.baseline.has_value());
    }
    // With baseline defaulting to nullopt, the decoded docs equal the helpers.
    ASSERT_EQ(replayed.recovery.documents.front(), savedDocument());
    ASSERT_EQ(replayed.recovery.documents.back(), untitledDocument());
}

TEST(fastContentHashIsDeterministicAndDistinguishes) {
    ASSERT_EQ(ssg::fastContentHash("hello\n"), ssg::fastContentHash("hello\n"));
    ASSERT_NE(ssg::fastContentHash("hello\n"), ssg::fastContentHash("hello"));
    ASSERT_NE(ssg::fastContentHash(""), ssg::fastContentHash("x"));
    // FNV-1a-64 offset basis for empty input — pins the standard algorithm and
    // cross-platform stability.
    ASSERT_EQ(ssg::fastContentHash(""), std::uint64_t{14695981039346656037ULL});
    // Known FNV-1a-64 test vector for "a" (0xaf63dc4c8601ec8c).
    ASSERT_EQ(ssg::fastContentHash("a"), std::uint64_t{12638187200555641996ULL});
}

TEST(replayAppliesCheckpointUpdatesAndRemovals) {
    std::vector<std::byte> journal =
        ssg::encodeJournalCheckpoint({{savedDocument(), untitledDocument()}});
    const auto updated =
        ssg::encodeJournalDocument(untitledDocument("new draft"));
    journal.insert(journal.end(), updated.begin(), updated.end());
    const auto removed =
        ssg::encodeJournalRemove(ssg::JournalDocumentKey::saved("notes.txt"));
    journal.insert(journal.end(), removed.begin(), removed.end());

    const auto replayed = ssg::replayJournal(journal);
    ASSERT_FALSE(replayed.discardedTail);
    ASSERT_EQ(replayed.validBytes, journal.size());
    ASSERT_EQ(replayed.recovery.documents.size(), std::size_t{1});
    ASSERT_EQ(replayed.recovery.documents.front(),
              untitledDocument("new draft"));
}

TEST(replayStartsFromNewestCheckpoint) {
    std::vector<std::byte> journal =
        ssg::encodeJournalCheckpoint({{savedDocument("old")}});
    const auto update = ssg::encodeJournalDocument(savedDocument("ignored"));
    journal.insert(journal.end(), update.begin(), update.end());
    const auto checkpoint =
        ssg::encodeJournalCheckpoint({{untitledDocument("new base")}});
    journal.insert(journal.end(), checkpoint.begin(), checkpoint.end());

    const auto replayed = ssg::replayJournal(journal);
    ASSERT_EQ(replayed.recovery.documents,
              std::vector<ssg::JournalDocument>{
                  untitledDocument("new base")});
}

TEST(corruptOrTruncatedTailStopsAtLastValidRecord) {
    const auto checkpoint =
        ssg::encodeJournalCheckpoint({{savedDocument("base")}});
    const auto update = ssg::encodeJournalDocument(savedDocument("changed"));
    std::vector<std::byte> complete = checkpoint;
    complete.insert(complete.end(), update.begin(), update.end());

    auto corrupt = complete;
    corrupt.back() ^= std::byte{0x80};
    const auto corruptReplay = ssg::replayJournal(corrupt);
    ASSERT_TRUE(corruptReplay.discardedTail);
    ASSERT_EQ(corruptReplay.validBytes, checkpoint.size());
    ASSERT_EQ(corruptReplay.recovery.documents,
              std::vector<ssg::JournalDocument>{savedDocument("base")});

    for (std::size_t cut = checkpoint.size() + 1; cut < complete.size();
         ++cut) {
        const auto truncated =
            ssg::replayJournal(std::span{complete}.first(cut));
        ASSERT_TRUE(truncated.discardedTail);
        ASSERT_EQ(truncated.validBytes, checkpoint.size());
        ASSERT_EQ(truncated.recovery.documents,
                  std::vector<ssg::JournalDocument>{savedDocument("base")});
    }
}

TEST(malformedInputFailsClosedWithoutAllocationOrState) {
    std::vector<std::byte> malformed(16, std::byte{0xff});
    const auto replayed = ssg::replayJournal(malformed);
    ASSERT_TRUE(replayed.discardedTail);
    ASSERT_EQ(replayed.validBytes, std::size_t{0});
    ASSERT_TRUE(replayed.recovery.documents.empty());

    auto oversized = ssg::encodeJournalCheckpoint({{}});
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
    ASSERT_FALSE(replayed.discardedTail);
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
        ssg::encodeJournalDocument(
            {ssg::JournalDocumentKey::saved("valid"),
             ssg::DocumentMode::Edit, true, std::string{"bad\xff", 4}}),
        std::invalid_argument);
}

} // namespace

SSG_TEST_SUITE(test_scratch_journal) {
    RUN(checkpointEncodingMatchesCrossPlatformByteFixture);
    RUN(baselineRoundTripsThroughDocumentAndCheckpoint);
    RUN(legacyV1RecordReplaysWithUnknownBaseline);
    RUN(fastContentHashIsDeterministicAndDistinguishes);
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
