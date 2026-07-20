#include <ssg/palette.h>

#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

std::vector<std::string> idsInRankOrder(
    std::vector<ssg::PaletteCandidate> const& candidates,
    std::string_view query) {
    std::vector<std::string> ids;
    for (auto index : ssg::paletteRank(candidates, query)) {
        ids.push_back(candidates[index].id);
    }
    return ids;
}

}  // namespace

// A hand-authored reference set: the expected order is derived independently of
// the implementation from the documented scoring (word-boundary and contiguity
// bonuses, length penalty, label/id-ascending tiebreak).
const std::vector<ssg::PaletteCandidate> kCatalog{
    {"file.save", "Save File", ""},
    {"file.save_all", "Save All Files", ""},
    {"file.open", "Open File", ""},
    {"edit.undo", "Undo", ""},
    {"edit.redo", "Redo", ""},
    {"view.split", "Split View", ""},
};

TEST(emptyQueryKeepsAllInLabelOrder) {
    auto const ids = idsInRankOrder(kCatalog, "");
    ASSERT_EQ(ids.size(), std::size_t{6});
    // Empty query scores 0 for every candidate, so the tiebreak (label asc)
    // fully determines order.
    ASSERT_EQ(ids, (std::vector<std::string>{"file.open", "edit.redo",
                                             "file.save_all", "file.save",
                                             "view.split", "edit.undo"}));
}

TEST(prefixQueryRanksWordBoundaryMatchesFirst) {
    // "save" matches the label "Save File"/"Save All Files" at a word boundary
    // and the id "file.save"/"file.save_all"; both share the boundary bonus, so
    // the shorter candidate (less length penalty) wins, tiebreak label asc.
    auto const ids = idsInRankOrder(kCatalog, "save");
    ASSERT_EQ(ids, (std::vector<std::string>{"file.save", "file.save_all"}));
}

TEST(nonSubsequenceQueryIsFilteredOut) {
    auto const ids = idsInRankOrder(kCatalog, "zzz");
    ASSERT_TRUE(ids.empty());
}

TEST(subsequenceMatchesAcrossSeparators) {
    // "fs" is a subsequence of id "file.save" (f...s) and "file.save_all".
    auto const ids = idsInRankOrder(kCatalog, "fs");
    ASSERT_EQ(ids, (std::vector<std::string>{"file.save", "file.save_all"}));
}

TEST(ghostCompletesMatchingPrefixCaseInsensitively) {
    ASSERT_EQ(ssg::paletteGhost("Save File", "sa"), std::string{"ve File"});
    ASSERT_EQ(ssg::paletteGhost("Save File", "Save"), std::string{" File"});
}

TEST(ghostEmptyWhenQueryIsNotAPrefix) {
    ASSERT_EQ(ssg::paletteGhost("Save File", "ile"), std::string{});
    ASSERT_EQ(ssg::paletteGhost("Save File", ""), std::string{});
    ASSERT_EQ(ssg::paletteGhost("Sa", "save"), std::string{});
}

int main() {
    RUN(emptyQueryKeepsAllInLabelOrder);
    RUN(prefixQueryRanksWordBoundaryMatchesFirst);
    RUN(nonSubsequenceQueryIsFilteredOut);
    RUN(subsequenceMatchesAcrossSeparators);
    RUN(ghostCompletesMatchingPrefixCaseInsensitively);
    RUN(ghostEmptyWhenQueryIsNotAPrefix);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
