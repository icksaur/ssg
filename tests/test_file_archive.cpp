#include "test_helpers.h"

#include <ssg/FileArchive.h>
#include <ssg/platform_files.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = testSystemRuntimePath(
            "file_archive_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code code;
        fs::remove_all(path_, code);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};



// Out-of-band so the archive is never its own oracle.
std::string readOutOfBand(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void writeOutOfBand(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

// Counts the files under `root`, so "the bytes exist SOMEWHERE in the archive"
// can be checked without knowing the generated entry name.
std::vector<fs::path> archivedFiles(const fs::path& root) {
    std::vector<fs::path> found;
    if (!fs::exists(root)) return found;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) found.push_back(entry.path());
    }
    return found;
}

std::chrono::system_clock::time_point at(int year, int month, int day) {
    return std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
        std::chrono::day{static_cast<unsigned>(day)}};
}

// The archived bytes must equal the pre-delete bytes exactly, and must be
// reachable by their original relative path so a manual restore is obvious.
TEST(archivingPreservesTheBytesAndTheRelativePath) {
    TemporaryDirectory workspace;
    const auto archiveRoot = workspace.path() / ".ssg" / "archive";
    const std::string payload = "line one\nline two\n";
    const auto source = workspace.path() / "src" / "deep" / "notes.txt";
    writeOutOfBand(source, payload);

    ssg::FileArchive archive{archiveRoot};
    const auto result = archive.archive(workspace.path(), source);
    ASSERT_TRUE(result.ok());

    const auto files = archivedFiles(archiveRoot);
    ASSERT_EQ(files.size(), std::size_t{1});
    ASSERT_EQ(readOutOfBand(files.front()), payload);
    // The relative path is preserved under the entry directory.
    const auto stored = files.front().generic_string();
    ASSERT_TRUE(stored.find("src/deep/notes.txt") != std::string::npos);
    // Archiving copies; it must not remove the original.
    ASSERT_TRUE(fs::exists(source));
}

// Two deletions of the SAME name must both survive. A flat archive keyed by
// name would silently drop the first.
TEST(archivingTheSameNameTwiceKeepsBothCopies) {
    TemporaryDirectory workspace;
    const auto archiveRoot = workspace.path() / ".ssg" / "archive";
    const auto source = workspace.path() / "same.txt";
    ssg::FileArchive archive{archiveRoot};

    writeOutOfBand(source, "first version");
    ASSERT_TRUE(archive.archive(workspace.path(), source).ok());
    writeOutOfBand(source, "second version");
    ASSERT_TRUE(archive.archive(workspace.path(), source).ok());

    const auto files = archivedFiles(archiveRoot);
    ASSERT_EQ(files.size(), std::size_t{2});
    std::string joined;
    for (const auto& file : files) joined += readOutOfBand(file);
    ASSERT_TRUE(joined.find("first version") != std::string::npos);
    ASSERT_TRUE(joined.find("second version") != std::string::npos);
}

TEST(archivingRefusesAPathOutsideTheWorkspace) {
    TemporaryDirectory workspace;
    TemporaryDirectory elsewhere;
    const auto outside = elsewhere.path() / "stranger.txt";
    writeOutOfBand(outside, "not ours");

    ssg::FileArchive archive{workspace.path() / ".ssg" / "archive"};
    ASSERT_FALSE(archive.archive(workspace.path(), outside).ok());
}

TEST(archivingAMissingFileFails) {
    TemporaryDirectory workspace;
    ssg::FileArchive archive{workspace.path() / ".ssg" / "archive"};
    ASSERT_FALSE(
        archive.archive(workspace.path(), workspace.path() / "ghost.txt").ok());
}

// Age comes from the entry's own recorded timestamp, so this is decidable
// without touching filesystem clocks.
TEST(pruningRemovesOnlyEntriesOlderThanTheLimit) {
    TemporaryDirectory workspace;
    const auto archiveRoot = workspace.path() / ".ssg" / "archive";

    const auto now = at(2026, 3, 1);
    const auto old = ssg::DurableStore::entryName(at(2026, 2, 1), "old");
    const auto fresh =
        ssg::DurableStore::entryName(at(2026, 2, 27), "fresh");
    const auto boundary = ssg::DurableStore::entryName(
        now - std::chrono::hours{24 * 14} + std::chrono::hours{1}, "boundary");
    writeOutOfBand(archiveRoot / old / "gone.txt", "expired");
    writeOutOfBand(archiveRoot / fresh / "kept.txt", "recent");
    writeOutOfBand(archiveRoot / boundary / "edge.txt", "just inside");

    ssg::FileArchive archive{archiveRoot};
    const auto report = archive.prune(now, std::chrono::hours{24 * 14});

    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.removed, std::size_t{1});
    ASSERT_FALSE(fs::exists(archiveRoot / old));
    ASSERT_TRUE(fs::exists(archiveRoot / fresh / "kept.txt"));
    ASSERT_TRUE(fs::exists(archiveRoot / boundary / "edge.txt"));
}

// An entry whose name cannot be parsed is more likely a bug than garbage.
// Removing it would be a second data-loss path in a feature that exists to
// prevent the first, so it is RETAINED and reported.
TEST(pruningRetainsAndReportsUnparseableEntries) {
    TemporaryDirectory workspace;
    const auto archiveRoot = workspace.path() / ".ssg" / "archive";
    writeOutOfBand(archiveRoot / "not-a-timestamp" / "mystery.txt", "keep me");
    writeOutOfBand(
        archiveRoot / ssg::DurableStore::entryName(at(2020, 1, 1), "ancient") /
            "ancient.txt",
        "expired");

    ssg::FileArchive archive{archiveRoot};
    const auto report = archive.prune(at(2026, 3, 1), std::chrono::hours{24 * 14});

    // Housekeeping never fails the caller, but it does say what it skipped.
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.removed, std::size_t{1});
    ASSERT_EQ(report.retainedUnparseable, std::size_t{1});
    ASSERT_TRUE(fs::exists(archiveRoot / "not-a-timestamp" / "mystery.txt"));
}

TEST(pruningAnAbsentArchiveIsNotAnError) {
    TemporaryDirectory workspace;
    ssg::FileArchive archive{workspace.path() / ".ssg" / "archive"};
    const auto report =
        archive.prune(at(2026, 3, 1), std::chrono::hours{24 * 14});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.removed, std::size_t{0});
}

// Entry names must sort chronologically, so the archive is browsable and the
// counter cannot reorder same-second entries.
TEST(entryDirectoryNamesSortChronologically) {
    const auto earlier =
        ssg::DurableStore::entryName(at(2026, 2, 1), "same");
    const auto later =
        ssg::DurableStore::entryName(at(2026, 3, 1), "same");
    ASSERT_TRUE(earlier < later);

    const auto first = ssg::DurableStore::entryName(at(2026, 2, 1), "first");
    const auto second = ssg::DurableStore::entryName(at(2026, 2, 1), "second");
    ASSERT_TRUE(first < second);
}

// The round trip that makes pruning trustworthy: a name this code generates is
// a name this code can read back.
TEST(entryTimestampsRoundTripThroughTheDirectoryName) {
    for (const auto moment :
         {at(2020, 1, 1), at(2026, 2, 28), at(2026, 12, 31)}) {
        const auto name = ssg::DurableStore::entryName(moment, "roundtrip");
        const auto parsed = ssg::DurableStore::entryTimestamp(name);
        ASSERT_TRUE(parsed.has_value());
        ASSERT_TRUE(*parsed == moment);
    }
    ASSERT_FALSE(
        ssg::DurableStore::entryTimestamp("not-a-timestamp").has_value());
    ASSERT_FALSE(ssg::DurableStore::entryTimestamp("").has_value());
}

// A second process deleting in the same second starts its counter at zero too.
// Simulating that with two FileArchive instances: the entries must not merge,
// because a later failure cleans up "its" entry with remove_all and would
// otherwise erase the other process's archived files.
TEST(twoArchivesInTheSameSecondDoNotShareAnEntryDirectory) {
    TemporaryDirectory workspace;
    const auto archiveRoot = workspace.path() / ".ssg" / "archive";
    writeOutOfBand(workspace.path() / "one.txt", "first process");
    writeOutOfBand(workspace.path() / "two.txt", "second process");

    ssg::FileArchive first{archiveRoot};
    ssg::FileArchive second{archiveRoot};
    const auto a = first.archive(workspace.path(), workspace.path() / "one.txt");
    const auto b = second.archive(workspace.path(), workspace.path() / "two.txt");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());

    ASSERT_TRUE(a.archivedPath.parent_path() != b.archivedPath.parent_path());
    ASSERT_EQ(readOutOfBand(a.archivedPath), std::string{"first process"});
    ASSERT_EQ(readOutOfBand(b.archivedPath), std::string{"second process"});
}

TEST(pruningRetainsAndReportsFutureDatedEntries) {
    TemporaryDirectory workspace;
    const auto archiveRoot = workspace.path() / ".ssg" / "archive";
    const auto now = at(2026, 3, 1);
    writeOutOfBand(
        archiveRoot / ssg::DurableStore::entryName(at(2027, 1, 1), "future") /
            "tomorrow.txt",
        "from the future");

    ssg::FileArchive archive{archiveRoot};
    const auto report = archive.prune(now, std::chrono::hours{24 * 14});

    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.removed, std::size_t{0});
    ASSERT_EQ(report.retainedFutureDated, std::size_t{1});
}

TEST(pruningFailureDoesNotStopTheCallerOrLoseTheEntry) {
#ifdef _WIN32
    return;
#endif
    TemporaryDirectory workspace;
    const auto archiveRoot = workspace.path() / ".ssg" / "archive";
    const auto now = at(2026, 3, 1);
    const auto expired =
        ssg::DurableStore::entryName(at(2020, 1, 1), "expired");
    writeOutOfBand(archiveRoot / expired / "ancient.txt", "expired");
    fs::permissions(archiveRoot / expired, fs::perms::owner_write,
                    fs::perm_options::remove);

    ssg::FileArchive archive{archiveRoot};
    const auto report = archive.prune(now, std::chrono::hours{24 * 14});

    fs::permissions(archiveRoot / expired, fs::perms::owner_all,
                    fs::perm_options::add);
    ASSERT_FALSE(report.message.empty());
    ASSERT_EQ(report.removed, std::size_t{0});
    ASSERT_TRUE(fs::exists(archiveRoot / expired / "ancient.txt"));
}

}  // namespace

SSG_TEST_SUITE(test_file_archive) {
    RUN(archivingPreservesTheBytesAndTheRelativePath);
    RUN(archivingTheSameNameTwiceKeepsBothCopies);
    RUN(archivingRefusesAPathOutsideTheWorkspace);
    RUN(archivingAMissingFileFails);
    RUN(pruningRemovesOnlyEntriesOlderThanTheLimit);
    RUN(pruningRetainsAndReportsUnparseableEntries);
    RUN(pruningAnAbsentArchiveIsNotAnError);
    RUN(entryDirectoryNamesSortChronologically);
    RUN(entryTimestampsRoundTripThroughTheDirectoryName);
    RUN(twoArchivesInTheSameSecondDoNotShareAnEntryDirectory);
    RUN(pruningRetainsAndReportsFutureDatedEntries);
    RUN(pruningFailureDoesNotStopTheCallerOrLoseTheEntry);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
