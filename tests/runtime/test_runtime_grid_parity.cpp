// Byte-identical TUI rendered-grid parity guard for the whole-screen publish (5b).
//
// The terminal renders the server-described grid (CellGrid), NOT the medium-agnostic
// uiSchema. Publishing the authority's whole-screen schema + real presence on the wire (a
// web-facing change) must leave that grid byte-for-byte unchanged. This golden captures the
// rendered grid across representative interaction states; if the publish alters TUI output,
// canonical() diverges and this fails. Regenerate (only after an intended TUI change) with
// SSG_REGEN_GOLDEN=1.

#include "../test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/Renderer.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path uniqueRoot() {
    auto root = std::filesystem::current_path() / "runtime_grid_parity";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream out{root / "workspace" / "alpha.txt"};
    for (int line = 0; line < 40; ++line) out << "alpha line " << line << "\n";
    std::ofstream{root / "workspace" / "beta.txt"} << "beta content\n";
    return root;
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
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    if (!created.accepted() || !created.runtime) return "runtime create failed";
    auto& runtime = *created.runtime;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    (void)runtime.dispatch(ssg::ClientId{1},
                           {"file.open", runtime.revision(), std::string{"alpha.txt"}});

    const ssg::ViewportDimensions dims{80, 24};
    std::ostringstream out;
    // The header row (row 0) carries the working-directory path field, whose absolute value
    // is cwd- and machine-specific. Redact it so the golden is portable; header/footer chrome
    // parity is already covered by the ui-layout golden and test_render. The publish's real
    // risk -- the body/panel/content region -- is fully captured.
    auto emit = [&](const std::string& name) {
        auto snapshot = runtime.present(ssg::ClientId{1}, dims);
        out << "=== " << name << " ===\n";
        if (!snapshot) {
            out << "(no snapshot)\n";
            return;
        }
        std::istringstream lines{ssg::Renderer{}.render(*snapshot).canonical()};
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
    (void)runtime.dispatch(ssg::ClientId{1}, {"panel.show_files", runtime.revision(), {}});
    emit("panel-files-shown");
    (void)runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}});
    emit("palette-open");
    (void)runtime.dispatch(ssg::ClientId{1}, {"palette.close", runtime.revision(), {}});
    (void)runtime.dispatch(ssg::ClientId{1}, {"find.open", runtime.revision(), {}});
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
                     "changed (the whole-screen publish must not alter TUI output)\n";
        ++failed;
    } else {
        ++passed;
    }
}

}  // namespace

int main() {
    RUN(tuiRenderedGridMatchesTheCommittedGoldenAcrossInteractionStates);
    return failed;
}
