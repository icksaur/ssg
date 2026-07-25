#pragma once

#include <ssg/GitDiffSource.h>
#include <ssg/PaletteSearcher.h>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace ssg {

struct WorkspaceFileIndexOptions {
    bool respectGitignore = true;
    // A pathological tree must not hang the picker's open.  Truncation is a
    // documented, deterministic outcome rather than a silent one.
    std::size_t maximumFiles = 20000;
};

struct WorkspaceFileIndexResult {
    std::vector<PaletteCandidate> candidates;
    bool truncated = false;
};

// Walks a workspace once and produces the file picker's candidate set.
//
// Ignored DIRECTORIES are pruned during descent rather than filtered out of the
// results afterwards.  This is the load-bearing performance rule and is not
// reconstructible from the observable behavior: a post-hoc filter still pays the
// full walk of node_modules/build/target, which is the bulk of a working tree
// (this repository is ~2900 files on disk against ~1200 tracked).
//
// Traversal order is deterministic -- entries are sorted at each level, so both
// the candidate order and WHICH candidates survive truncation are stable across
// runs and filesystems.
class WorkspaceFileIndex {
public:
    [[nodiscard]] WorkspaceFileIndexResult build(
        const std::filesystem::path& workspaceRoot,
        const GitIgnoreMatcher& ignore,
        const WorkspaceFileIndexOptions& options = {}) const;
};

}  // namespace ssg
