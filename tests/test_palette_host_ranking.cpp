// seam test — the web host ranks palette candidates through the library's
// PaletteSearcher, never a second ranker. The client owns only the query text
// and selection index; this pins that a given query yields the LIBRARY ranker's
// order, so the web path cannot drift into reimplementing ranking (which would be
// a second behavior path).

#include <ssg/PaletteSearcher.h>

#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

using namespace ssg;

std::vector<PaletteCandidate> candidates() {
    return {
        {"file.save", "Save File", ""},
        {"file.save_as", "Save File As", ""},
        {"edit.undo", "Undo", ""},
        {"view.split", "Split View", ""},
    };
}

TEST(theHostRanksAQueryThroughTheLibrarySearcherNotItsOwnOrder) {
    auto const cands = candidates();
    PaletteWindowState window;
    window.query = "save";
    window.paneRows = 12;
    auto const report = PaletteSearcher{}.report(cands, window);

    // The report the host publishes is exactly the library ranker's order for
    // the client-owned query: both Save rows rank above the non-match.
    ASSERT_TRUE(report.rows.size() >= 2);
    ASSERT_EQ(report.rows[0].label, std::string{"Save File"});
    ASSERT_EQ(report.rows[1].label, std::string{"Save File As"});

    // Same order the raw ranker returns -- the host adds no ordering of its own.
    auto const order = PaletteSearcher{}.rank(cands, "save");
    ASSERT_TRUE(order.size() >= 2);
    ASSERT_EQ(cands[order[0]].label, report.rows[0].label);
    ASSERT_EQ(cands[order[1]].label, report.rows[1].label);
}

}  // namespace

SSG_TEST_SUITE(test_palette_host_ranking) {
    RUN(theHostRanksAQueryThroughTheLibrarySearcherNotItsOwnOrder);
    return 0;
}
