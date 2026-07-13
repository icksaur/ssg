#include <ssg/palette.h>

#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

std::vector<std::string> ids_in_rank_order(
    std::vector<ssg::PaletteCandidate> const& candidates,
    std::string_view query) {
    std::vector<std::string> ids;
    for (auto index : ssg::palette_rank(candidates, query)) {
        ids.push_back(candidates[index].id);
    }
    return ids;
}

}  // namespace

// A hand-authored reference set: the expected order is derived independently of
// the implementation from the documented scoring (word-boundary and contiguity
// bonuses, length penalty, label/id-ascending tiebreak).
const std::vector<ssg::PaletteCandidate> catalog{
    {"file.save", "Save File", ""},
    {"file.save_all", "Save All Files", ""},
    {"file.open", "Open File", ""},
    {"edit.undo", "Undo", ""},
    {"edit.redo", "Redo", ""},
    {"view.split", "Split View", ""},
};

TEST(empty_query_keeps_all_in_label_order) {
    auto const ids = ids_in_rank_order(catalog, "");
    ASSERT_EQ(ids.size(), std::size_t{6});
    // Empty query scores 0 for every candidate, so the tiebreak (label asc)
    // fully determines order.
    ASSERT_EQ(ids, (std::vector<std::string>{"file.open", "edit.redo",
                                             "file.save_all", "file.save",
                                             "view.split", "edit.undo"}));
}

TEST(prefix_query_ranks_word_boundary_matches_first) {
    // "save" matches the label "Save File"/"Save All Files" at a word boundary
    // and the id "file.save"/"file.save_all"; both share the boundary bonus, so
    // the shorter candidate (less length penalty) wins, tiebreak label asc.
    auto const ids = ids_in_rank_order(catalog, "save");
    ASSERT_EQ(ids, (std::vector<std::string>{"file.save", "file.save_all"}));
}

TEST(non_subsequence_query_is_filtered_out) {
    auto const ids = ids_in_rank_order(catalog, "zzz");
    ASSERT_TRUE(ids.empty());
}

TEST(subsequence_matches_across_separators) {
    // "fs" is a subsequence of id "file.save" (f...s) and "file.save_all".
    auto const ids = ids_in_rank_order(catalog, "fs");
    ASSERT_EQ(ids, (std::vector<std::string>{"file.save", "file.save_all"}));
}

TEST(ghost_completes_matching_prefix_case_insensitively) {
    ASSERT_EQ(ssg::palette_ghost("Save File", "sa"), std::string{"ve File"});
    ASSERT_EQ(ssg::palette_ghost("Save File", "Save"), std::string{" File"});
}

TEST(ghost_empty_when_query_is_not_a_prefix) {
    ASSERT_EQ(ssg::palette_ghost("Save File", "ile"), std::string{});
    ASSERT_EQ(ssg::palette_ghost("Save File", ""), std::string{});
    ASSERT_EQ(ssg::palette_ghost("Sa", "save"), std::string{});
}

int main() {
    RUN(empty_query_keeps_all_in_label_order);
    RUN(prefix_query_ranks_word_boundary_matches_first);
    RUN(non_subsequence_query_is_filtered_out);
    RUN(subsequence_matches_across_separators);
    RUN(ghost_completes_matching_prefix_case_insensitively);
    RUN(ghost_empty_when_query_is_not_a_prefix);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
