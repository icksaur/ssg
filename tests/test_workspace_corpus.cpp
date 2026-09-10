#include "test_helpers.h"

#include <ssg/WorkspaceCorpus.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ssg;
namespace fs = std::filesystem;

class FixtureIgnore final : public GitIgnoreMatcher {
public:
    [[nodiscard]] bool usable() const override { return true; }
    [[nodiscard]] bool ignores(
        const fs::path& relative) const override {
        return relative == "ignored" ||
               relative.extension() == ".ignored";
    }
};

fs::path uniqueRoot(std::string_view label) {
    return fs::temp_directory_path() /
           ("ssg-corpus-" + std::string{label} + "-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
}

void writeBytes(const fs::path& path,
                const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void writeText(const fs::path& path, std::string_view text) {
    writeBytes(path, {text.begin(), text.end()});
}

std::optional<std::string> textFor(const WorkspaceCorpus& corpus,
                                   std::string_view path) {
    const auto found = std::ranges::find(corpus.paths(), path);
    if (found == corpus.paths().end()) return std::nullopt;
    const auto file = corpus.read(
        static_cast<std::size_t>(found - corpus.paths().begin()));
    return file ? std::optional<std::string>{file->text} : std::nullopt;
}

TEST(corpusMatchesIndependentFixtureAndOpenBuffersWin) {
    const auto root = uniqueRoot("membership");
    writeText(root / "kept.txt", "disk\n");
    writeText(root / "ignored" / "hidden.txt", "hidden\n");
    writeText(root / "drop.ignored", "ignored\n");
    writeBytes(root / "binary.dat", {'a', 0, 'b'});
    FixtureIgnore ignore;
    std::size_t reads = 0;
    WorkspaceCorpus corpus{
        root,
        {{std::optional<std::string>{"kept.txt"},
          [] { return std::optional<std::string>{"modified\n"}; }},
         {std::nullopt,
          [] { return std::optional<std::string>{"untitled\n"}; }},
         {std::optional<std::string>{"ignored/open.txt"},
          [] { return std::optional<std::string>{"open\n"}; }}},
        ignore,
        [&](const fs::path& path) {
            ++reads;
            return readFile(path);
        }};

    const std::vector<std::string> expected{
        "binary.dat", "ignored/open.txt", "kept.txt"};
    ASSERT_EQ(corpus.paths(), expected);
    ASSERT_EQ(textFor(corpus, "kept.txt"),
              std::optional<std::string>{"modified\n"});
    ASSERT_EQ(textFor(corpus, "ignored/open.txt"),
              std::optional<std::string>{"open\n"});
    ASSERT_FALSE(textFor(corpus, "binary.dat").has_value());
    ASSERT_EQ(reads, std::size_t{1});
    fs::remove_all(root);
}

TEST(corpusStopsAtTheLimitAndOnWalkFailure) {
    const auto root = uniqueRoot("outcomes");
    writeText(root / "a.txt", "a");
    writeText(root / "b.txt", "b");
    FixtureIgnore ignore;
    WorkspaceCorpus truncated{
        root, {}, ignore, readFile,
        WorkspaceCorpusOptions{.maximumFiles = 1}};
    ASSERT_EQ(truncated.paths(),
              (std::vector<std::string>{"a.txt"}));

    const auto regularFile = root / "not-a-directory";
    writeText(regularFile, "x");
    WorkspaceCorpus failedCorpus{regularFile, {}, ignore, readFile};
    ASSERT_TRUE(failedCorpus.paths().empty());
    fs::remove_all(root);
}

TEST(pathEnumerationNeverReadsContents) {
    const auto root = uniqueRoot("path-only");
    writeText(root / "a.txt", "a");
    FixtureIgnore ignore;
    WorkspaceCorpus corpus{
        root, {}, ignore,
        [](const fs::path&) -> FileReadResult {
            throw std::runtime_error{"path enumeration read a file"};
        }};
    ASSERT_EQ(corpus.paths(), (std::vector<std::string>{"a.txt"}));
    fs::remove_all(root);
}

}  // namespace

SSG_TEST_SUITE(test_workspace_corpus) {
    RUN(corpusMatchesIndependentFixtureAndOpenBuffersWin);
    RUN(corpusStopsAtTheLimitAndOnWalkFailure);
    RUN(pathEnumerationNeverReadsContents);
    return failed == 0 ? 0 : 1;
}
