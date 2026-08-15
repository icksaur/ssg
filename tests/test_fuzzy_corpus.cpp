// Cross-client corpus oracle (algorithm) for the shared fuzzy match-and-order
// contract. The expected match set and ordering per query are hand-derivable from
// the rules (subsequence match, ASCII fold over UTF-8 bytes, score, stable order
// with label-then-id tie-break) for this small fixed corpus, and are stored in the
// shared fixture tests/fixtures/fuzzy_corpus.tsv. BOTH the C++ reference
// (ssg::referenceRank) here and the web matcher (tests/web/test_fuzzy_corpus.mjs)
// must reproduce them identically -- that agreement, not a copied blob, is the
// contract. The fixture is shared so the two clients cannot drift onto different
// corpora.

#include "ssg/PaletteSearcher.h"

#include "test_helpers.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ssg;

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream stream(line);
    while (std::getline(stream, field, '\t')) out.push_back(field);
    // getline drops a trailing empty field; restore it so a line ending in '\t'
    // (an empty detail) keeps its column.
    if (!line.empty() && line.back() == '\t') out.push_back("");
    return out;
}

std::vector<std::string> splitCommas(const std::string& value) {
    std::vector<std::string> out;
    if (value.empty()) return out;
    std::string item;
    std::istringstream stream(value);
    while (std::getline(stream, item, ',')) out.push_back(item);
    return out;
}

struct Corpus {
    PaletteViewState state;
    struct Query {
        std::string query;
        std::vector<std::string> expected;  // candidate ids, in order
    };
    std::vector<Query> queries;
};

Corpus loadCorpus() {
    std::ifstream input{SSG_FUZZY_CORPUS};
    Corpus corpus;
    std::string line;
    while (std::getline(input, line)) {
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        auto fields = splitTabs(line);
        if (fields[0] == "C") {
            // C \t id \t label \t detail
            corpus.state.candidates.push_back(
                {fields.at(1), fields.at(2), fields.size() > 3 ? fields[3] : ""});
        } else if (fields[0] == "Q") {
            // Q \t query \t comma-separated-expected-ids
            std::string query = fields.size() > 1 ? fields[1] : "";
            std::string expected = fields.size() > 2 ? fields[2] : "";
            corpus.queries.push_back({query, splitCommas(expected)});
        }
    }
    return corpus;
}

TEST(referenceRankReproducesTheCorpusOrdering) {
    const auto corpus = loadCorpus();
    ASSERT_TRUE(!corpus.state.candidates.empty());
    ASSERT_TRUE(!corpus.queries.empty());
    for (const auto& query : corpus.queries) {
        auto order = referenceRank(corpus.state, query.query);
        std::vector<std::string> ids;
        for (auto index : order) ids.push_back(corpus.state.candidates[index].id);
        ASSERT_EQ(ids, query.expected);
    }
}

// The matcher boundary is safe for ANY in-process input, not only wire-decoded ones:
// out-of-domain weights or an oversized candidate would breach the score-exactness
// invariant, so they are REFUSED loudly (std::invalid_argument), matching the web
// client -- never silently normalized into a plausible ranking.
TEST(referenceRankRejectsOutOfDomainParametersAtTheBoundary) {
    PaletteViewState state;
    state.parameters.baseScore = kMaxMatcherParameterMagnitude * 1000;  // out of domain
    state.candidates = {{"edit.undo", "Undo", ""}};
    ASSERT_THROWS(referenceRank(state, "u"), std::invalid_argument);
}

TEST(referenceRankRejectsAnOversizedCandidateAtTheBoundary) {
    PaletteViewState state;
    state.candidates = {
        {"huge", std::string(static_cast<std::size_t>(kMaxCandidateBytes) + 1, 'u'),
         ""}};
    ASSERT_THROWS(referenceRank(state, "u"), std::invalid_argument);
}

}  // namespace

int main() {
    RUN(referenceRankReproducesTheCorpusOrdering);
    RUN(referenceRankRejectsOutOfDomainParametersAtTheBoundary);
    RUN(referenceRankRejectsAnOversizedCandidateAtTheBoundary);
    return failed;
}
