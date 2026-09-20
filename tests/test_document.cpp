#include <ssg/Document.h>

#include "reference_editor.h"
#include "test_helpers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ssg::ByteOffset;
using ssg::Document;
using ssg::DocumentError;
using ssg::DocumentMode;
using ssg::EditTransaction;
using std::uint64_t;
using ssg::TextEdit;

TextEdit edit(std::uint64_t offset, std::uint64_t erased,
              std::string inserted) {
    return TextEdit{ByteOffset{offset}, erased, std::move(inserted)};
}

EditTransaction transaction(std::uint64_t base, std::vector<TextEdit> edits) {
    return EditTransaction{base, std::move(edits)};
}

void applyToReference(ref::Editor& editor,
                        std::vector<TextEdit> const& edits) {
    auto ordered = edits;
    std::sort(ordered.begin(), ordered.end(),
              [](TextEdit const& left, TextEdit const& right) {
                  return left.offset.value() > right.offset.value();
              });
    for (auto const& replacement : ordered) {
        const auto begin = static_cast<std::size_t>(replacement.offset.value());
        const auto end = begin + static_cast<std::size_t>(replacement.erasedBytes);
        ref::select_set_range(editor, begin, end);
        ref::text_insert(editor, replacement.insertedText);
    }
}

TEST(constructionProducesCanonicalCleanSnapshot) {
    Document document("alpha\n\xCE\xB2" "eta", DocumentMode::Edit);
    const auto snapshot = document.snapshot();

    ASSERT_EQ(snapshot.text, std::string("alpha\n\xCE\xB2" "eta"));
    ASSERT_EQ(snapshot.revision, std::uint64_t{1});
    ASSERT_EQ(snapshot.mode, DocumentMode::Edit);
    ASSERT_FALSE(snapshot.dirty);
}

TEST(referenceEditorTransactionScript) {
    Document document("0123456789");
    auto oracle = ref::make_editor("0123456789");
    const std::vector<TextEdit> edits{
        edit(1, 2, "one"),
        edit(6, 0, "-"),
        edit(8, 2, "tail"),
    };

    applyToReference(oracle, edits);
    const auto result = document.apply(transaction(document.revision(), edits));

    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(document.snapshot().text, std::string(ref::snapshot_text(oracle)));
    ASSERT_EQ(result.revision, std::uint64_t{2});
    ASSERT_TRUE(document.snapshot().dirty);
}

class Random {
public:
    explicit Random(std::uint64_t state) : state_(state) {}

    std::size_t below(std::size_t limit) {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return limit == 0 ? 0 : static_cast<std::size_t>(state_ % limit);
    }

private:
    std::uint64_t state_;
};

std::string randomInsert(Random& random) {
    static const std::vector<std::string> values{"x", "YZ", "\n", "_"};
    return values[random.below(values.size())];
}

TEST(randomizedMultiEditSnapshotsMatchReferenceEditor) {
    Document document("the quick brown fox\njumps over the lazy dog");
    auto oracle = ref::make_editor("the quick brown fox\njumps over the lazy dog");
    Random random{0xD0C7A11ULL};

    for (std::size_t step = 0; step < 500; ++step) {
        const auto size = ref::snapshot_text(oracle).size();
        const auto count = std::min<std::size_t>(1 + random.below(3), size + 1);
        std::vector<std::size_t> offsets;
        while (offsets.size() < count) {
            const auto candidate = random.below(size + 1);
            if (std::find(offsets.begin(), offsets.end(), candidate) ==
                offsets.end()) {
                offsets.push_back(candidate);
            }
        }
        std::sort(offsets.begin(), offsets.end());

        std::vector<TextEdit> edits;
        for (std::size_t index = 0; index < offsets.size(); ++index) {
            const auto offset = offsets[index];
            const auto next = index + 1 < offsets.size() ? offsets[index + 1] : size;
            const auto available = next - offset;
            const auto erased = available == 0 ? 0 : random.below(
                std::min<std::size_t>(available, 3) + 1);
            edits.push_back(edit(offset, erased, randomInsert(random)));
        }

        applyToReference(oracle, edits);
        const auto result =
            document.apply(transaction(document.revision(), std::move(edits)));
        ASSERT_TRUE(result.accepted());
        ASSERT_EQ(document.snapshot().text,
                  std::string(ref::snapshot_text(oracle)));
    }

    ASSERT_EQ(document.revision(), std::uint64_t{501});
    ASSERT_TRUE(document.snapshot().dirty);
}

TEST(readOnlyAndDiffModesRejectWithoutStateChange) {
    for (const auto mode : {DocumentMode::ReadOnly, DocumentMode::Diff}) {
        Document document("fixed", mode);
        const auto before = document.snapshot();
        const auto result = document.apply(
            transaction(document.revision(), {edit(0, 1, "F")}));

        ASSERT_FALSE(result.accepted());
        ASSERT_EQ(result.error, mode == DocumentMode::ReadOnly
                                    ? DocumentError::ReadOnly
                                    : DocumentError::Diff);
        ASSERT_EQ(document.snapshot(), before);

        const auto replacement = document.replace("changed");
        ASSERT_FALSE(replacement.accepted());
        ASSERT_EQ(replacement.error, result.error);
        ASSERT_EQ(document.snapshot(), before);
    }
}

TEST(staleRevisionRejectsWithoutStateChange) {
    Document document("abc");
    ASSERT_TRUE(document.apply(
        transaction(document.revision(), {edit(3, 0, "d")})).accepted());
    const auto before = document.snapshot();

    const auto result =
        document.apply(transaction(std::uint64_t{1}, {edit(0, 1, "A")}));

    ASSERT_EQ(result.error, DocumentError::StaleRevision);
    ASSERT_EQ(document.snapshot(), before);
}

TEST(invalidLaterEditPreservesWholeTransaction) {
    Document document("abcdef");
    const auto before = document.snapshot();

    const auto result = document.apply(transaction(
        document.revision(), {edit(1, 2, "ok"), edit(99, 0, "bad")}));

    ASSERT_EQ(result.error, DocumentError::InvalidRange);
    ASSERT_EQ(document.snapshot(), before);
}

TEST(overlappingAndDuplicateRangesAreAtomicRejections) {
    Document document("abcdef");
    const auto before = document.snapshot();

    ASSERT_EQ(document.apply(transaction(
                  document.revision(), {edit(1, 3, "x"), edit(2, 1, "y")}))
                  .error,
              DocumentError::OverlappingEdits);
    ASSERT_EQ(document.snapshot(), before);
    ASSERT_EQ(document.apply(transaction(
                  document.revision(), {edit(2, 0, "x"), edit(2, 0, "y")}))
                  .error,
              DocumentError::OverlappingEdits);
    ASSERT_EQ(document.snapshot(), before);
}

TEST(utf8TextAndBoundariesAreValidatedAtomically) {
    Document document("a\xC3\xA9z");
    const auto before = document.snapshot();

    ASSERT_TRUE(document.apply(transaction(
        document.revision(), {edit(1, 2, "\xE4\xB8\xAD")}))
                    .accepted());
    ASSERT_EQ(document.snapshot().text, std::string("a\xE4\xB8\xADz"));

    Document rejected("a\xC3\xA9z");
    const auto rejectedBefore = rejected.snapshot();
    ASSERT_EQ(document.apply(transaction(
                  document.revision(), {edit(2, 0, "x")}))
                  .error,
              DocumentError::InvalidUtf8Boundary);
    ASSERT_EQ(rejected.apply(transaction(
                  rejected.revision(), {edit(1, 1, "x")}))
                  .error,
              DocumentError::InvalidUtf8Boundary);
    ASSERT_EQ(rejected.apply(transaction(
                  rejected.revision(), {edit(1, 2, std::string("\xC3", 1))}))
                  .error,
              DocumentError::InvalidUtf8);
    ASSERT_EQ(rejected.apply(transaction(
                  rejected.revision(), {edit(1, 2, std::string("x\0y", 3))}))
                  .error,
              DocumentError::InvalidUtf8);
    ASSERT_EQ(rejected.snapshot(), rejectedBefore);
}

TEST(invalidConstructionIsActionable) {
    ASSERT_THROWS(Document(std::string("\xC3", 1)), std::invalid_argument);
    ASSERT_THROWS(Document(std::string("x\0y", 3)), std::invalid_argument);
}

TEST(emptyAndNoopTransactionsAreRejected) {
    Document document("abc");
    const auto before = document.snapshot();

    ASSERT_EQ(document.apply(transaction(document.revision(), {})).error,
              DocumentError::EmptyTransaction);
    ASSERT_EQ(document.apply(transaction(
                  document.revision(), {edit(1, 0, "")}))
                  .error,
              DocumentError::EmptyTransaction);
    ASSERT_EQ(document.snapshot(), before);
}

TEST(snapshotIsOwningAndRevisionAdvancesOncePerTransaction) {
    Document document("abc");
    const auto oldSnapshot = document.snapshot();

    const auto result = document.apply(transaction(
        document.revision(), {edit(0, 1, "A"), edit(3, 0, "!")}));

    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.revision, std::uint64_t{2});
    ASSERT_EQ(document.revision(), std::uint64_t{2});
    ASSERT_EQ(oldSnapshot.text, std::string("abc"));
    ASSERT_FALSE(oldSnapshot.dirty);
    ASSERT_EQ(document.snapshot().text, std::string("Abc!"));
    ASSERT_TRUE(document.snapshot().dirty);
}

TEST(lineCountTracksAcceptedTransactionsAndReplacement) {
    Document document("one\ntwo\n");
    ASSERT_EQ(document.lineCount(), std::uint64_t{3});

    const auto edited = document.apply(transaction(
        document.revision(), {edit(3, 1, ""), edit(7, 0, "\nthree\n")}));
    ASSERT_TRUE(edited.accepted());
    ASSERT_EQ(document.lineCount(), std::uint64_t{4});

    const auto rejected =
        document.apply(transaction(document.revision() - 1, {edit(0, 0, "\n")}));
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(document.lineCount(), std::uint64_t{4});

    ASSERT_TRUE(document.replace("single").accepted());
    ASSERT_EQ(document.lineCount(), std::uint64_t{1});
}

}  // namespace

SSG_TEST_SUITE(test_document) {
    RUN(constructionProducesCanonicalCleanSnapshot);
    RUN(referenceEditorTransactionScript);
    RUN(randomizedMultiEditSnapshotsMatchReferenceEditor);
    RUN(readOnlyAndDiffModesRejectWithoutStateChange);
    RUN(staleRevisionRejectsWithoutStateChange);
    RUN(invalidLaterEditPreservesWholeTransaction);
    RUN(overlappingAndDuplicateRangesAreAtomicRejections);
    RUN(utf8TextAndBoundariesAreValidatedAtomically);
    RUN(invalidConstructionIsActionable);
    RUN(emptyAndNoopTransactionsAreRejected);
    RUN(snapshotIsOwningAndRevisionAdvancesOncePerTransaction);
    RUN(lineCountTracksAcceptedTransactionsAndReplacement);
    return failed == 0 ? 0 : 1;
}
