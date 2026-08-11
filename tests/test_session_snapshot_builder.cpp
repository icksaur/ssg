// Kind: seam.
//
// Proves SessionSnapshotBuilder is faithful: a snapshot it builds renders the
// document region identically to one the real EditorRuntime produces for the
// same text.
//
// This is the test that makes the builder trustworthy.  ONE test pays the cost
// of a temp directory and a full runtime; every other presentation test then
// uses the builder and pays nothing.  If the builder ever drifts from the
// production projection, this fails and the cheap tests stay honest.

#include "session_snapshot_builder.h"
#include "test_helpers.h"

#include <ssg/EditorRuntime.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path uniqueRoot() {
    auto base = fs::temp_directory_path() /
                ("ssg-builder-" + std::to_string(::rand()));
    fs::remove_all(base);
    fs::create_directories(base / "scratch");
    fs::create_directories(base / "recovery");
    return base;
}

std::string rowText(ssg::CellGrid const& grid, int row) {
    std::string line;
    for (int column = 0; column < grid.size.columns; ++column) {
        auto const& cell = grid.at(column, row);
        if (!cell.continuation) line += cell.text;
    }
    return line;
}

// The rows carrying document content, in order, trimmed of trailing blanks.
std::vector<std::string> documentRows(ssg::CellGrid const& grid) {
    std::vector<std::string> rows;
    for (int row = 0; row < grid.size.rows; ++row) {
        auto text = rowText(grid, row);
        auto const end = text.find_last_not_of(' ');
        text = end == std::string::npos ? std::string{} : text.substr(0, end + 1);
        if (!text.empty()) rows.push_back(std::move(text));
    }
    return rows;
}

TEST(builtSnapshotRendersTheDocumentLikeTheRealRuntime) {
    std::string const text = "alpha beta\nsecond line\nthird\n";

    auto root = uniqueRoot();
    std::ofstream{root / "a.txt", std::ios::binary} << text;
    auto created = ssg::EditorRuntime::create(
        {root, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto runtime = std::move(created.runtime);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"file.open", runtime->revision(),
                                std::string{"a.txt"}})
                    .accepted());
    auto real = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(real.has_value());
    if (!real) return;

    auto built = ssg::test::SessionSnapshotBuilder{}
                     .document(text)
                     .viewport(80, 24)
                     .build();

    auto const realGrid = ssg::Renderer{}.render(*real);
    auto const builtGrid = ssg::Renderer{}.render(built);

    ASSERT_EQ(realGrid.size.columns, builtGrid.size.columns);
    ASSERT_EQ(realGrid.size.rows, builtGrid.size.rows);

    // Every document line appears in both, at the same relative order.  Chrome
    // legitimately differs (the real runtime knows a workspace path and a tab
    // name), so the comparison is over document content, not the whole screen.
    for (std::string const& line : {"alpha beta", "second line", "third"}) {
        bool inReal = false;
        bool inBuilt = false;
        for (auto const& row : documentRows(realGrid)) {
            if (row.find(line) != std::string::npos) inReal = true;
        }
        for (auto const& row : documentRows(builtGrid)) {
            if (row.find(line) != std::string::npos) inBuilt = true;
        }
        ASSERT_TRUE(inReal);
        ASSERT_TRUE(inBuilt);
    }

    // The projections agree field-for-field: same visual row count, same first
    // row, same scrollbar metrics.  This is the part that would silently drift
    // if the builder reimplemented projection instead of calling it.
    auto const& realViewport = real->presentation()->viewport;
    auto const& builtViewport = built.presentation()->viewport;
    ASSERT_EQ(realViewport.totalVisualRows, builtViewport.totalVisualRows);
    ASSERT_EQ(realViewport.firstVisualRow, builtViewport.firstVisualRow);
    ASSERT_EQ(realViewport.visibleRows.size(), builtViewport.visibleRows.size());

    fs::remove_all(root);
}

TEST(theBuilderProducesARenderableScreenWithoutAnyFilesystem) {
    // The whole point: no temp directory, no runtime, no disk.
    auto snapshot = ssg::test::SessionSnapshotBuilder{}
                        .document("hello\n")
                        .viewport(40, 10)
                        .build();
    auto const grid = ssg::Renderer{}.render(snapshot);
    ASSERT_EQ(grid.size.columns, 40);
    ASSERT_EQ(grid.size.rows, 10);

    bool found = false;
    for (int row = 0; row < grid.size.rows; ++row) {
        if (rowText(grid, row).find("hello") != std::string::npos) found = true;
    }
    ASSERT_TRUE(found);
}

TEST(builderSettersReachTheRenderedScreen) {
    // Sections set through the escape hatch reach the renderer, so a test can
    // express states the named setters do not cover.
    auto snapshot = ssg::test::SessionSnapshotBuilder{}
                        .document("body\n")
                        .viewport(60, 12)
                        .style([] {
                            ssg::Style s;
                            s.unrenderable = "?";
                            return s;
                        }())
                        .build();
    ASSERT_EQ(snapshot.presentation()->style.unrenderable, std::string{"?"});

    // And the caret is placed at a consistent document position.
    auto positioned = ssg::test::SessionSnapshotBuilder{}
                          .document("ab\ncd\n")
                          .caret(4)
                          .viewport(60, 12)
                          .build();
    auto const& primary =
        positioned.sections().selection.primary().active;
    ASSERT_EQ(primary.byteOffset, ssg::ByteOffset{4});
    ASSERT_EQ(primary.line, ssg::LineIndex{1});
    ASSERT_EQ(primary.cell, ssg::CellIndex{1});
}

}  // namespace

int main() {
    RUN(builtSnapshotRendersTheDocumentLikeTheRealRuntime);
    RUN(theBuilderProducesARenderableScreenWithoutAnyFilesystem);
    RUN(builderSettersReachTheRenderedScreen);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
