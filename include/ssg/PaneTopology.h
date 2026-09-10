#pragma once

#include <ssg/PaneNavigation.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ssg {

struct PaneTopologyNode {
    PaneId id;
    std::vector<PaneTopologyNode> children;
    SplitAxis axis = SplitAxis::Horizontal;

    [[nodiscard]] bool isLeaf() const noexcept { return children.empty(); }
    friend bool operator==(const PaneTopologyNode&, const PaneTopologyNode&) = default;
};

class Editor;

class PaneTopology {
public:
    [[nodiscard]] static PaneTopology initial();

    [[nodiscard]] const PaneTopologyNode& root() const noexcept { return root_; }
    [[nodiscard]] PaneId activePane() const noexcept { return active_; }
    [[nodiscard]] std::vector<PaneId> panes() const;
    [[nodiscard]] bool contains(PaneId pane) const noexcept;

    PaneId splitActive(SplitAxis axis);
    [[nodiscard]] bool closeActive();
    void cycle(CycleDirection direction);
    [[nodiscard]] bool focus(PaneId pane) noexcept;

    friend bool operator==(const PaneTopology&, const PaneTopology&) = default;

private:
    explicit PaneTopology(PaneTopologyNode root, PaneId active,
                          std::uint32_t nextId) noexcept;

    PaneTopologyNode root_;
    PaneId active_;
    std::uint32_t nextId_;
};

}  // namespace ssg
