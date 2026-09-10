// Byte-identical TUI rendered-grid parity guard for the screen publish (5b).
//
// The terminal renders the server-described grid (CellGrid), NOT the medium-agnostic
// uiSchema. Publishing the authority's screen schema + real presence (a
// web-facing change) must leave that grid byte-for-byte unchanged. This golden captures the
// rendered grid across representative interaction states; if the publish alters TUI output,
// the serialized grid diverges and this fails. Regenerate (only after an intended TUI change) with
// SSG_REGEN_GOLDEN=1.

#include "../test_helpers.h"
#include "../grid_test_frame.h"

#include <ssg/Editor.h>
#include <ssg/Renderer.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

std::filesystem::path uniqueRoot() {
    auto root = testRuntimePath("runtime_grid_parity");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream out{root / "workspace" / "alpha.txt"};
    for (int line = 0; line < 40; ++line) out << "alpha line " << line << "\n";
    std::ofstream{root / "workspace" / "beta.txt"} << "beta content\n";
    return root;
}

std::string escaped(std::string_view text) {
    std::string result;
    for (unsigned char byte : text) {
        switch (byte) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20 || byte == 0x7f) {
                std::ostringstream encoded;
                encoded << "\\x" << std::hex << std::setw(2)
                        << std::setfill('0') << static_cast<unsigned>(byte);
                result += encoded.str();
            } else {
                result.push_back(static_cast<char>(byte));
            }
        }
    }
    return result;
}

std::string serializeGrid(const ssg::CellGrid& grid) {
    std::ostringstream output;
    output << "size " << grid.size.columns << ' ' << grid.size.rows << '\n';
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int column = 0; column < grid.size.columns; ++column) {
            auto const& cell = grid.at(column, row);
            if (cell.text == " " && cell.role == ssg::SemanticRole::Canvas &&
                !cell.continuation && cell.tint == ssg::DiffTint::None) {
                continue;
            }
            output << "cell " << column << ' ' << row << ' '
                   << static_cast<unsigned>(cell.foreground) << ' '
                   << static_cast<unsigned>(cell.background) << ' '
                   << static_cast<unsigned>(cell.role) << ' '
                   << (cell.continuation ? "~" : '"' + escaped(cell.text) + '"')
                   << (cell.tint == ssg::DiffTint::None
                           ? ""
                           : " tint " +
                                 std::to_string(static_cast<unsigned>(cell.tint)))
                   << '\n';
        }
    }
    return output.str();
}

std::string readGolden(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Drive a runtime through representative interaction states and serialize the rendered grid
// of each, so the golden covers editor-only, panel-shown, palette-open, and find-open.
std::string captureGridMatrix() {
    ssg::LineLayoutCache lineCache;
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    if (!created.accepted() || !created.session) return "runtime create failed";
    auto& runtime = *created.session;
    (void)runtime.dispatch({"file.open",  std::string{"alpha.txt"}});

    const ssg::ViewportDimensions dims{80, 24};
    std::ostringstream out;
    // The header row (row 0) carries the working-directory path field, whose absolute value
    // is cwd- and machine-specific. Redact it so the golden is portable; header/footer chrome
    // parity is already covered by the ui-layout golden and test_render. The publish's real
    // risk -- the body/panel/content region -- is fully captured.
    auto emit = [&](const std::string& name) {
        auto frame = ssg::test::projectGridFrame(runtime, dims);
        out << "=== " << name << " ===\n";
        if (!frame) {
            out << "(no snapshot)\n";
            return;
        }
        std::istringstream lines{
            serializeGrid(ssg::renderFrame(*frame, lineCache))};
        std::string line;
        while (std::getline(lines, line)) {
            // Drop "cell <col> 0 ..." (row 0 = header); keep sizes and every other row.
            if (line.rfind("cell ", 0) == 0) {
                const auto firstSpace = line.find(' ', 5);
                const auto secondSpace = line.find(' ', firstSpace + 1);
                const std::string rowField =
                    line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
                if (rowField == "0") continue;
            }
            out << line << '\n';
        }
        out << '\n';
    };

    emit("editor-only");
    (void)runtime.dispatch({"panel.show_files",  {}});
    emit("panel-files-shown");
    (void)runtime.dispatch({"palette.open",  {}});
    emit("palette-open");
    (void)runtime.dispatch({"palette.close",  {}});
    (void)runtime.dispatch({"find.open",  {}});
    emit("find-open");
    return out.str();
}

TEST(tuiRenderedGridMatchesTheCommittedGoldenAcrossInteractionStates) {
    const std::string actual = captureGridMatrix();
    const std::string path =
        std::string{SSG_SOURCE_DIR} + "/tests/fixtures/runtime/grid_parity.txt";
    if (std::getenv("SSG_REGEN_GOLDEN") != nullptr) {
        std::ofstream{path, std::ios::binary} << actual;
        std::cout << "  regenerated " << path << '\n';
        ++passed;
        return;
    }
    const std::string expected = readGolden(path);
    if (expected.empty()) {
        std::cerr << "  grid parity golden missing; run SSG_REGEN_GOLDEN=1 to create "
                  << path << '\n';
        ++failed;
        return;
    }
    if (actual != expected) {
        std::cerr << "  TUI grid parity golden mismatch: the rendered terminal grid "
                     "changed (the screen publish must not alter TUI output)\n";
        ++failed;
    } else {
        ++passed;
    }
}

}  // namespace

SSG_TEST_SUITE(test_session_grid_parity) {
    RUN(tuiRenderedGridMatchesTheCommittedGoldenAcrossInteractionStates);
    return failed;
}
