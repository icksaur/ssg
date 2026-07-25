// Uses the library exactly as an embedder would: through public headers only,
// with highlighting disabled by leaving syntaxParser null.
#include <ssg/EditorRuntime.h>

#include <filesystem>
#include <iostream>

int main() {
    auto root = std::filesystem::temp_directory_path() / "ssg-embed-consumer";
    std::filesystem::create_directories(root / "workspace");
    ssg::EditorRuntimeConfig config;
    config.cwd = root / "workspace";
    config.scratchRoot = root / "scratch";
    config.recoveryRoot = root / "recovery";
    // No syntaxParser: the runtime disable, which is the only one after Phase B.
    auto created = ssg::EditorRuntime::create(config);
    if (!created.accepted()) {
        std::cerr << "embed_consumer: runtime creation failed\n";
        return 1;
    }
    std::filesystem::remove_all(root);
    std::cout << "embed_consumer ok\n";
    return 0;
}
