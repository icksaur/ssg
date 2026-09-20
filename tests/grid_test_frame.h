#pragma once

#include <ssg/Editor.h>
#include <ssg/GridPresenter.h>

#include <optional>
#include <utility>

namespace ssg::test {

inline std::optional<GridPresentation> projectGridFrame(
    Editor& session,
    ViewportDimensions dimensions, PaletteReport palette = {}) {
    GridPresenter presenter{};
    return presenter.project(session, {dimensions, std::move(palette)});
}

inline std::optional<GridPresentation> projectGridFrame(
    Editor& session) {
    return projectGridFrame(session, {80, 24});
}

inline std::string activeDocumentText(Editor& session) {
    auto frame = projectGridFrame(session);
    return frame ? std::move(frame->documentText) : std::string{};
}

inline DiffIngressResult applyGitDiffScan(Editor& session, GitDiffScan scan) {
    std::lock_guard lock{session.operationMutex};
    return session.gitDiffIngress.applyGitDiffScanLocked(std::move(scan));
}

inline void registerCommand(Editor& session, std::string id, std::string label,
                            std::function<CommandResult()> handler) {
    session.addCommand(std::move(id), std::move(label), std::move(handler));
}

inline GridPresentation copyGridFrame(
    GridPresentation const& source,
    GridPresentation values,
    GridPresentation projection, PaletteReport palette = {}) {
    values.viewport = std::move(projection.viewport);
    values.style = std::move(projection.style);
    values.palette = std::move(palette);

    std::vector<GridIntrinsicSize> sizes;
    const auto collect = [&](const auto& self, const UiNode& node) -> void {
        if (node.size.kind() == SizeKind::Auto && node.isLeaf()) {
            auto size = GridSize{1, 1};
            if (node.id == UiNodeId{std::string{kExternalModNodeId}}) {
                size = measureExternalModificationSurface(
                    values.externalModification);
            }
            sizes.push_back({node.id, size});
        }
        if (const auto* container = std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) self(self, child);
        }
    };
    collect(collect, values.uiTree.root);
    auto solved = solveUiFrame(
        values.uiTree, sizes,
        {0, 0, static_cast<int>(values.viewport.dimensions.columns),
         static_cast<int>(values.viewport.dimensions.rows)});
    if (!solved.tree) throw std::logic_error{solved.error};
    values.layout = std::move(*solved.tree);

    const auto findNode = [&](const auto& self, const UiNode& node,
                              std::string_view id) -> const UiNode* {
        if (node.id.value() == id) return &node;
        if (const auto* container = std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) {
                if (const auto* found = self(self, child, id)) return found;
            }
        }
        return nullptr;
    };
    const auto solveRegion = [&](std::string_view id, SemanticRole role,
                                 std::optional<SolvedUiRegion>& output,
                                 const PromptInputProjection* input) {
        const auto* rect = values.layout.find(UiNodeId{std::string{id}});
        if (!rect) {
            output.reset();
            return;
        }
        const auto* node = findNode(findNode, values.uiTree.root, id);
        if (!node) throw std::logic_error{"missing UI node"};
        SolvedUiRegion region;
        auto result =
            projectUiRegion(*node, rect->rect, role, values.style, region, input);
        if (!result.ok()) throw std::logic_error{*result.error};
        output = std::move(region);
    };
    PromptInputProjection input{
        values.paletteView.activePicker.has_value(),
        values.palette.query, values.palette.ghost};
    solveRegion(kHeaderNodeId, SemanticRole::Header, values.header,
                input.visible ? &input : nullptr);
    solveRegion(kFooterNodeId, SemanticRole::Footer, values.footer, nullptr);
    if (const auto* node = values.layout.find(
            UiNodeId{std::string{kDocumentViewportNodeId}})) {
        values.document = solveDocumentSurface(
            *node, PaneTopology::initial(),
            values.document && values.document->lineNumbers.width > 0,
            static_cast<std::uint32_t>(
                std::count(values.documentText.begin(),
                           values.documentText.end(), '\n') +
                1),
            values.style.dimensions);
    } else {
        values.document.reset();
    }
    return values;
}

}  // namespace ssg::test
