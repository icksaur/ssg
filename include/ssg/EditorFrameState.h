#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/DiffModel.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/LspState.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PaneTopology.h>
#include <ssg/PromptSurface.h>
#include <ssg/Selection.h>
#include <ssg/Style.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TabManager.h>
#include <ssg/Theme.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace ssg {

// Editor captures all fields under one operation lock. Presentation may move
// them into its output, but must never read live editor state through this frame.
struct EditorFrameState {
    EditorFrameState() = default;
    EditorFrameState(const EditorFrameState&) = delete;
    EditorFrameState& operator=(const EditorFrameState&) = delete;
    EditorFrameState(EditorFrameState&&) noexcept = default;
    EditorFrameState& operator=(EditorFrameState&&) noexcept = default;

    std::string documentText;
    std::uint64_t documentRevision = 0;
    std::uint64_t documentLineCount = 1;
    std::optional<std::string> diffFileIdentity;
    Style style;
    PaneTopology panes = PaneTopology::initial();
    SelectionSet selections{std::vector<Selection>{Selection{
        DocumentPosition{ByteOffset{0}, LineIndex{0}, CellIndex{0}},
        DocumentPosition{ByteOffset{0}, LineIndex{0}, CellIndex{0}}}}};
    FindReplaceViewState findReplace;
    DiffViewState diff;
    LspSyncViewState lspSync;
    std::shared_ptr<const SyntaxViewState> syntax;
    ThemeSnapshot theme;
    UiSchema uiTree;
    TabViewState tabs;
    ExternalModificationViewState externalModification;
    FollowMode followMode = FollowMode::Following;
    PromptViewState prompt;
    PaletteViewState paletteView;
    TreeViewState tree;
    std::optional<ClipboardWrite> clipboardWrite;
    bool wordWrap = false;
    bool lineNumbers = false;
};

static_assert(!std::is_copy_constructible_v<EditorFrameState>);

} // namespace ssg
