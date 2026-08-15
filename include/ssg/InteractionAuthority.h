#pragma once

// The runtime's single interaction authority: the sole owner of the whole-screen schema
// generation, the prompt surface, the semantic truth, the derived interaction projection,
// and the tree-provider revision source. Every focus/presence/prompt change flows through
// it, so ShellState and the snapshot become READERS of a projection rather than
// independent writers.
//
// Two mutation shapes, both atomic:
//   - apply(CommandTransition): the closed multi-subsystem transitions (panel, provider,
//     finder). prepare + install are hidden behind this ONE method so no revision can be
//     allocated between peeking the source and consuming the prepared transition; a
//     rejected preflight mutates nothing.
//   - openPrompt/submitPrompt/cancelPrompt/updatePromptValue: every generic prompt
//     lifecycle path. Each mutates a COPY of the prompt, derives the replacement
//     interaction, then swaps prompt and projection together, so prompt activity and focus
//     authority never diverge.
//
// The authority owns the ONE tree-revision source: allocateTreeRevision is the sole minter
// for non-transition tree updates, and a transition's create is stamped from the same
// source inside apply. Workers never mint tree revisions.

#include <cstdint>
#include <optional>

#include <ssg/CommandTransition.h>
#include <ssg/InteractionState.h>
#include <ssg/Picker.h>
#include <ssg/PromptSurface.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/Widget.h>  // UiComposition
#include <ssg/WholeScreenInteraction.h>
#include <ssg/WholeScreenSchema.h>

namespace ssg {

class InteractionAuthority {
public:
    // Seed from the initial whole-screen assembly at generation 0, over the caller-owned
    // TreeModel, with the first tree revision the source will hand out.
    InteractionAuthority(UiComposition initialAssembly, TreeModel& tree,
                         std::uint64_t firstTreeRevision = 1);

    // Apply a transition atomically: prepare against current truth/prompt/tree and, on a
    // non-null preflight, install as one consuming owner swap. Returns whether it applied;
    // a rejected transition mutates nothing.
    bool apply(const CommandTransition& transition);

    // Generic prompt lifecycle -- the single typed owner. Each returns the underlying
    // PromptSurface result and leaves prompt + projection consistent.
    PromptCommandResult openPrompt(PromptRequest request);
    PromptCommandResult submitPrompt();
    PromptCommandResult cancelPrompt();
    PromptCommandResult updatePromptValue(std::size_t index, std::string value);

    // Re-assemble the whole-screen schema; when its generation advances, rebuild the
    // interaction projection from the SAME truth and prompt over the new schema (the
    // migration). Returns whether the generation advanced.
    bool updateComposition(UiComposition assembly);

    // The sole minter of tree revisions for non-transition tree updates (filesystem/git
    // refresh, panel-provider create), so all revisions come from one monotonic source.
    TreeRevision allocateTreeRevision();

    [[nodiscard]] const WholeScreenTruth& truth() const noexcept { return truth_; }
    [[nodiscard]] const UiInteractionState& interaction() const noexcept {
        return interaction_;
    }
    [[nodiscard]] const PromptSurface& prompt() const noexcept { return prompt_; }
    [[nodiscard]] FocusTarget effectiveFocus() const noexcept {
        return interaction_.effectiveFocus();
    }
    [[nodiscard]] std::optional<PickerKind> openPicker() const noexcept {
        return truth_.openPicker;
    }

private:
    // Adopt `prompt` and rebuild truth+projection together from it: derive the prompt-focus
    // region, reconcile openPicker (valid only while a Palette prompt is active), and build
    // the interaction over the CURRENT schema -- all computed before the owned state is
    // assigned, so a rebuild failure cannot leave prompt and focus authority divergent.
    void applyPromptState(PromptSurface prompt);

    [[nodiscard]] std::vector<TreeProviderPresence> presentProviders() const;

    WholeScreenSchema schema_;
    TreeModel& tree_;
    std::uint64_t nextTreeRevision_;
    PromptSurface prompt_;
    WholeScreenTruth truth_;
    UiInteractionState interaction_;
};

}  // namespace ssg
