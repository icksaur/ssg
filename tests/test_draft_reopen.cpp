#include <ssg/DraftReopenClassifier.h>
#include "test_helpers.h"

#include <optional>
#include <string>

namespace {

// Baseline hashes the RAW disk bytes (as the capture path does).
ssg::DraftBaseline baselineForRaw(std::string_view rawDisk,
                                  std::uint64_t mtime = 111,
                                  std::uint64_t size = 7) {
    return ssg::DraftBaseline{mtime, size, ssg::fastContentHash(rawDisk)};
}

std::optional<ssg::DraftDiskState> disk(std::string raw, std::string decoded) {
    return ssg::DraftDiskState{std::move(raw), std::move(decoded)};
}
std::optional<ssg::DraftDiskState> plainDisk(std::string content) {
    return disk(content, content);
}

TEST(missingDiskContentClassifiesMissing) {
    ASSERT_TRUE(ssg::classifyDraftReopen(baselineForRaw("base\n"), "draft\n",
                                         std::nullopt) ==
                ssg::DraftReopenClass::Missing);
}

TEST(draftEqualToDiskClassifiesConverged) {
    ASSERT_TRUE(ssg::classifyDraftReopen(
                    baselineForRaw("old base\n"), "shared\n",
                    plainDisk("shared\n")) ==
                ssg::DraftReopenClass::Converged);
}

TEST(diskMatchingBaselineHashClassifiesUnchanged) {
    const std::string raw = "on disk\n";
    ASSERT_TRUE(ssg::classifyDraftReopen(baselineForRaw(raw, 999, 4242),
                                         "unsaved edits\n", plainDisk(raw)) ==
                ssg::DraftReopenClass::Unchanged);
}

TEST(diskDifferingFromBaselineHashClassifiesConflict) {
    ASSERT_TRUE(ssg::classifyDraftReopen(
                    baselineForRaw("original\n"), "my edits\n",
                    plainDisk("changed externally\n")) ==
                ssg::DraftReopenClass::Conflict);
}

TEST(sameSizeExternalRewriteClassifiesConflict) {
    ASSERT_TRUE(ssg::classifyDraftReopen(
                    baselineForRaw("aaaa\n", 111, 5), "edits\n",
                    plainDisk("bbbb\n")) ==
                ssg::DraftReopenClass::Conflict);
}

TEST(absentBaselineClassifiesConflictNotUnchanged) {
    ASSERT_TRUE(ssg::classifyDraftReopen(
                    std::nullopt, "draft\n", plainDisk("disk\n")) ==
                ssg::DraftReopenClass::Conflict);
}

TEST(convergedTakesPrecedenceOverConflict) {
    ASSERT_TRUE(ssg::classifyDraftReopen(
                    baselineForRaw("different base\n"), "converged\n",
                    plainDisk("converged\n")) ==
                ssg::DraftReopenClass::Converged);
}

TEST(crlfFileUsesRawBytesForHashAndDecodedTextForConvergence) {
    // A CRLF file: raw disk bytes carry \r\n; the decoded buffer text is \n. The
    // baseline hashed the RAW bytes.
    const std::string raw = "line one\r\nline two\r\n";
    const std::string decoded = "line one\nline two\n";
    const auto baseline = baselineForRaw(raw);

    // Unchanged: disk still the CRLF bytes -> raw hash matches baseline even
    // though the draft (decoded \n) differs textually. Hashing the DECODED text
    // would make every CRLF reopen a spurious Conflict.
    ASSERT_TRUE(ssg::classifyDraftReopen(
                    baseline, "edited\n", disk(raw, decoded)) ==
                ssg::DraftReopenClass::Unchanged);

    // Converged: the draft's decoded text equals the disk's decoded text, even
    // though draft (\n) never equals the raw disk bytes (\r\n).
    ASSERT_TRUE(ssg::classifyDraftReopen(baseline, decoded,
                                         disk(raw, decoded)) ==
                ssg::DraftReopenClass::Converged);
}

} // namespace

SSG_TEST_SUITE(test_draft_reopen) {
    RUN(missingDiskContentClassifiesMissing);
    RUN(draftEqualToDiskClassifiesConverged);
    RUN(diskMatchingBaselineHashClassifiesUnchanged);
    RUN(diskDifferingFromBaselineHashClassifiesConflict);
    RUN(sameSizeExternalRewriteClassifiesConflict);
    RUN(absentBaselineClassifiesConflictNotUnchanged);
    RUN(convergedTakesPrecedenceOverConflict);
    RUN(crlfFileUsesRawBytesForHashAndDecodedTextForConvergence);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
