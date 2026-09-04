#pragma once

#include <ssg/ClientInput.h>
#include <ssg/LineLayoutCache.h>
#include <ssg/Viewport.h>

#include <optional>
#include <vector>

namespace ssg::detail {

struct GridProjectionState {
    Viewport viewport;
    LineLayoutCache lineCache;
    SelectionNavigation navigation;
    std::uint32_t treeFirstVisible = 0;
    std::uint64_t generation = 0;
    std::optional<std::uint64_t> documentRevision;
    std::optional<std::uint64_t> findGeneration;
    std::optional<TabId> activeTab;
    std::optional<SelectionSet> selections;
    std::optional<TreeNodeId> treeSelection;
    bool panelVisible = false;
    std::optional<std::uint64_t> wrappedDocumentRevision;
    std::optional<TabId> wrappedTab;
    std::vector<CellRun> wrappedCellRuns;
    struct PendingSelection {
        TabId activeTab;
        std::uint64_t documentRevision;
        SelectionSet expected;
        SelectionNavigation navigation;
    };
    std::optional<PendingSelection> pendingSelection;
};

}  // namespace ssg::detail
