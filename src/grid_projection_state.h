#pragma once

#include <ssg/ClientInput.h>
#include <ssg/LineLayoutCache.h>
#include <ssg/ShellState.h>
#include <ssg/Viewport.h>

#include <optional>
#include <vector>

namespace ssg::detail {

struct GridProjectionState {
    ViewportDimensions dimensions{80, 24};
    std::uint32_t paneContentRows = 24;
    std::uint32_t paneContentColumns = 80;
    std::uint32_t reservedPromptRows = 0;
    std::uint32_t panelContentRows = 0;
    SelectionNavigation navigation;
    std::uint32_t treeFirstVisible = 0;
    LineLayoutCache viewportLineCache;
    std::optional<Revision> lineCountRevision;
    std::optional<FileDocumentId> lineCountDocument;
    std::uint32_t lineCountCache = 1;
    std::optional<Revision> cellRunsRevision;
    std::optional<FileDocumentId> cellRunsDocument;
    std::vector<CellRun> cellRunsCache;
    std::optional<Revision> adoptedRevision;
    std::uint64_t generation = 0;
    std::optional<Revision> documentRevision;
    std::optional<std::uint64_t> findGeneration;
    std::optional<TabId> activeTab;
    std::optional<SelectionSet> selections;
    std::optional<TreeNodeId> treeSelection;
    struct PendingSelection {
        TabId activeTab;
        Revision documentRevision;
        SelectionSet expected;
        SelectionNavigation navigation;
    };
    std::optional<PendingSelection> pendingSelection;
    ShellState shell;
};

}  // namespace ssg::detail
