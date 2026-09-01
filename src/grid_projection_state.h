#pragma once

#include <ssg/ClientInput.h>
#include <ssg/ShellState.h>
#include <ssg/ViewportProjection.h>

#include <optional>
#include <vector>

namespace ssg::detail {

struct GridProjectionState {
    ViewportProjectionState viewport;
    SelectionNavigation navigation;
    std::uint32_t treeFirstVisible = 0;
    std::optional<Revision> adoptedRevision;
    std::uint64_t generation = 0;
    std::optional<Revision> documentRevision;
    std::optional<std::uint64_t> findGeneration;
    std::optional<TabId> activeTab;
    std::optional<SelectionSet> selections;
    std::optional<TreeNodeId> treeSelection;
    bool panelVisible = false;
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
