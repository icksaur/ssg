// seam test — both the TUI and the web host resolve one stroke per key press
// (KeymapMatchKind is only None/Resolved; neither client accumulates a pending
// sequence). That is complete ONLY while the authoritative keymap has no
// multi-stroke binding. This pins that precondition: introducing a multi-stroke
// binding fails here, forcing whoever adds it to teach BOTH clients a pending
// buffer rather than silently letting one drop the prefix and diverge.

#include <ssg/EditorRuntime.h>
#include <ssg/Keymap.h>

#include "test_helpers.h"

#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

fs::path uniqueRoot() {
    auto root = fs::current_path() /
                ("keymap_single_stroke_root_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    return root;
}

TEST(theAuthoritativeKeymapHasNoMultiStrokeBindingSoSingleStrokeResolutionIsComplete) {
    auto const root = uniqueRoot();
    auto created =
        ssg::EditorRuntime::create({root, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    auto const snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());

    for (auto const& binding : snapshot->sections().keymap.bindings) {
        ASSERT_EQ(binding.sequence.size(), static_cast<std::size_t>(1));
    }

    fs::remove_all(root);
}

}  // namespace

int main() {
    RUN(theAuthoritativeKeymapHasNoMultiStrokeBindingSoSingleStrokeResolutionIsComplete);
    return 0;
}
