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
    // Move keyboard authority among the active prompt's inputs. focusPromptControl
    // rejects an index that does not address an input (UnknownInput); both leave
    // prompt + projection consistent.
    PromptCommandResult focusPromptControl(std::size_t index);
    PromptCommandResult focusNextPromptControl();

    // Simple base-focus changes -- editor/panel focus that touch only the aggregate, not a
    // transition. focusPanel is honored only while the panel is present (returns whether it
    // took). While a prompt is open its capture still routes effective focus to the prompt;
    // the base change surfaces when the prompt closes.
    void focusEditor();
    bool focusPanel();

    // Reconcile the draft-conflict notice presence into truth. The notice's source is
    // per-document runtime state outside the prompt/panel transitions, so the runtime
    // calls this after each dispatch; a change rebuilds the projection so the notice
    // region shows/hides. Returns whether presence changed (no rebuild when unchanged).
    bool refreshNoticePresence(bool present);

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
    // Advances every time a finder transition (re)opens a picker, INCLUDING a File->File
    // reopen where openPicker is unchanged. A candidate-owning caller keys its rebuild off
    // this epoch so reopening the file finder always refreshes, and comparing openPicker
    // alone cannot miss a same-kind reopen.
    [[nodiscard]] std::uint64_t pickerEpoch() const noexcept { return pickerEpoch_; }

private:
    // Adopt a prospective truth and prompt together: reconcile a stale picker identity
    // (valid only while a Palette prompt is active), build the projection over the CURRENT
    // schema, then assign truth, prompt, and projection -- all computed before any owned
    // state changes, so a rebuild failure cannot leave them divergent.
    void adopt(WholeScreenTruth next, PromptSurface prompt);

    [[nodiscard]] std::vector<TreeProviderPresence> presentProviders() const;

    WholeScreenSchema schema_;
    TreeModel& tree_;
    std::uint64_t nextTreeRevision_;
    std::uint64_t pickerEpoch_ = 0;
    PromptSurface prompt_;
    WholeScreenTruth truth_;
    UiInteractionState interaction_;
};

}  // namespace ssg
