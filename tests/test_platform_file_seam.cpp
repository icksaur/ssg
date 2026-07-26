#include "ssg/platform_files.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(what.size()),
                     what.data());
        ++failures;
    }
}

std::span<const std::byte> bytesOf(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string textOf(const std::vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Reads a file WITHOUT going through the seam, so the seam is never its own
// oracle. This is the only raw stream in this file and exists precisely so the
// comparison is independent.
std::string readOutOfBand(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void writeOutOfBand(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

class ScopedDirectory {
public:
    explicit ScopedDirectory(std::string_view name)
        : path_(fs::temp_directory_path() / name) {
        std::error_code code;
        fs::remove_all(path_, code);
        fs::create_directories(path_);
    }
    ~ScopedDirectory() {
        std::error_code code;
        fs::remove_all(path_, code);
    }
    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

// A missing file, an empty file, an unreadable file and a populated file must
// all be distinguishable. The bug this pins is the one the old readFileText
// had: absence returning empty content.
void readFileDistinguishesAbsenceFromEmptiness() {
    ScopedDirectory root{"ssg_seam_read"};

    const auto missing = root.path() / "missing.txt";
    const auto missingResult = ssg::readFile(missing);
    check(missingResult.status == ssg::FileIoStatus::NotFound,
          "missing file reads as NotFound");
    check(!missingResult.ok(), "missing file is not ok");

    const auto empty = root.path() / "empty.txt";
    writeOutOfBand(empty, "");
    const auto emptyResult = ssg::readFile(empty);
    check(emptyResult.ok(), "empty file reads ok");
    check(emptyResult.bytes.empty(), "empty file has no bytes");

    check(missingResult.status != emptyResult.status,
          "absence and emptiness are distinguishable");

    const auto populated = root.path() / "populated.txt";
    writeOutOfBand(populated, "hello\nworld\n");
    const auto populatedResult = ssg::readFile(populated);
    check(populatedResult.ok(), "populated file reads ok");
    check(textOf(populatedResult.bytes) == readOutOfBand(populated),
          "read bytes equal the out-of-band bytes");

    // A directory is not a file; reading one must not masquerade as success.
    const auto directoryResult = ssg::readFile(root.path());
    check(!directoryResult.ok(), "reading a directory fails");
}

void readFileRoundTripsBinaryContent() {
    ScopedDirectory root{"ssg_seam_binary"};
    const auto target = root.path() / "binary.bin";

    std::string payload;
    for (int value = 0; value < 256; ++value) {
        payload.push_back(static_cast<char>(value));
    }
    payload += payload;

    const auto created = ssg::createFileExclusively(target, bytesOf(payload));
    check(created.ok(), "binary create succeeds");

    const auto read = ssg::readFile(target);
    check(read.ok(), "binary read succeeds");
    check(textOf(read.bytes) == payload,
          "binary round trip preserves every byte including NUL");
}

// The clash rule. Every name-taking primitive must refuse an occupied
// destination AND leave the occupant's bytes untouched -- a primitive that
// refused after truncating would still satisfy a naive "call failed" check.
void nameTakingPrimitivesRefuseAnOccupiedDestination() {
    ScopedDirectory root{"ssg_seam_clash"};

    const auto occupied = root.path() / "occupied.txt";
    const std::string original = "do not clobber me";
    writeOutOfBand(occupied, original);

    const auto created =
        ssg::createFileExclusively(occupied, bytesOf("replacement"));
    check(created.status == ssg::FileIoStatus::AlreadyExists,
          "createFileExclusively refuses an existing path");
    check(readOutOfBand(occupied) == original,
          "createFileExclusively leaves the occupant unchanged");

    const auto source = root.path() / "source.txt";
    writeOutOfBand(source, "source bytes");
    const auto renamed = ssg::renameFileNoClobber(source, occupied);
    check(renamed.status == ssg::FileIoStatus::AlreadyExists,
          "renameFileNoClobber refuses an existing destination");
    check(readOutOfBand(occupied) == original,
          "renameFileNoClobber leaves the occupant unchanged");
    check(fs::exists(source), "a refused rename leaves the source in place");

    const auto copied = ssg::copyFileDurably(source, occupied);
    check(copied.status == ssg::FileIoStatus::AlreadyExists,
          "copyFileDurably refuses an existing destination");
    check(readOutOfBand(occupied) == original,
          "copyFileDurably leaves the occupant unchanged");
}

void nameTakingPrimitivesSucceedOnAFreeDestination() {
    ScopedDirectory root{"ssg_seam_free"};

    const auto created = root.path() / "created.txt";
    check(ssg::createFileExclusively(created, bytesOf("fresh")).ok(),
          "createFileExclusively succeeds on a free path");
    check(readOutOfBand(created) == "fresh", "created content is correct");

    const auto moved = root.path() / "moved.txt";
    check(ssg::renameFileNoClobber(created, moved).ok(),
          "renameFileNoClobber succeeds on a free destination");
    check(!fs::exists(created), "rename removes the source");
    check(readOutOfBand(moved) == "fresh", "rename preserves content");

    const auto copy = root.path() / "copy.txt";
    check(ssg::copyFileDurably(moved, copy).ok(),
          "copyFileDurably succeeds on a free destination");
    check(fs::exists(moved), "copy leaves the source in place");
    check(readOutOfBand(copy) == readOutOfBand(moved),
          "copy reproduces the source bytes exactly");
}

void missingSourcesAreReportedAsNotFound() {
    ScopedDirectory root{"ssg_seam_missing"};
    const auto absent = root.path() / "absent.txt";
    const auto destination = root.path() / "destination.txt";

    check(ssg::renameFileNoClobber(absent, destination).status ==
              ssg::FileIoStatus::NotFound,
          "renaming a missing source reports NotFound");
    check(ssg::copyFileDurably(absent, destination).status ==
              ssg::FileIoStatus::NotFound,
          "copying a missing source reports NotFound");
    check(ssg::removeFile(absent).status == ssg::FileIoStatus::NotFound,
          "removing a missing file reports NotFound");

    const auto present = root.path() / "present.txt";
    writeOutOfBand(present, "bytes");
    check(ssg::removeFile(present).ok(), "removing a present file succeeds");
    check(!fs::exists(present), "removeFile actually removes");
}

class FailingInjector : public ssg::FileIoFaultInjector {
public:
    explicit FailingInjector(std::string operation)
        : operation_(std::move(operation)) {}

    ssg::FileIoStatus beforeOperation(std::string_view operation,
                                      const fs::path&) override {
        return operation == operation_ ? ssg::FileIoStatus::IoError
                                       : ssg::FileIoStatus::Ok;
    }

private:
    std::string operation_;
};

// Fault injection has to make failure handling reachable, and must not fire
// when it was not asked to.
void faultInjectionFailsOnlyTheNamedOperation() {
    ScopedDirectory root{"ssg_seam_fault"};
    const auto target = root.path() / "target.txt";

    FailingInjector injector{"createFileExclusively"};
    auto* previous = ssg::installFileIoFaultInjector(&injector);

    const auto created = ssg::createFileExclusively(target, bytesOf("data"));
    check(created.status == ssg::FileIoStatus::IoError,
          "the injected operation fails");
    check(!fs::exists(target),
          "an injected failure happens before the filesystem is touched");

    const auto read = ssg::readFile(target);
    check(read.status == ssg::FileIoStatus::NotFound,
          "an unnamed operation is unaffected by the injector");

    (void)ssg::installFileIoFaultInjector(previous);

    check(ssg::createFileExclusively(target, bytesOf("data")).ok(),
          "the seam works again once the injector is removed");
}

}  // namespace

int main() {
    readFileDistinguishesAbsenceFromEmptiness();
    readFileRoundTripsBinaryContent();
    nameTakingPrimitivesRefuseAnOccupiedDestination();
    nameTakingPrimitivesSucceedOnAFreeDestination();
    missingSourcesAreReportedAsNotFound();
    faultInjectionFailsOnlyTheNamedOperation();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
