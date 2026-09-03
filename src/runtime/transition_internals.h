#pragma once

#include <ssg/CommandTransition.h>

#include "interaction_state.h"
#include "whole_screen_interaction.h"

namespace ssg {

struct TreeBackingPlan {
    TreeProviderId activate;
    std::optional<TreeProviderSnapshot> create;
};

struct TreeProviderPresence {
    TreeProviderBinding binding;
    TreeRevision revision{0};
};

struct TransitionInputs {
    WholeScreenTruth truth;
    UiSchema schema;
    PromptSurface prompt;
    std::vector<TreeProviderPresence> presentProviders;
    std::optional<TreeProviderBinding> activeProvider;
    TreeRevision nextTreeRevision{0};
};

class PreparedTransition {
public:
    [[nodiscard]] const WholeScreenTruth& truth() const noexcept { return truth_; }
    [[nodiscard]] const UiInteractionState& interaction() const noexcept {
        return interaction_;
    }
    [[nodiscard]] const PromptSurface& prompt() const noexcept { return prompt_; }
    [[nodiscard]] const std::optional<TreeBackingPlan>& tree() const noexcept {
        return tree_;
    }

private:
    PreparedTransition(WholeScreenTruth truth, UiInteractionState interaction,
                       PromptSurface prompt, std::optional<TreeBackingPlan> tree)
        : truth_{std::move(truth)},
          interaction_{std::move(interaction)},
          prompt_{std::move(prompt)},
          tree_{std::move(tree)} {}

    void installInto(WholeScreenTruth& truth, UiInteractionState& interaction,
                     PromptSurface& prompt, TreeModel& tree,
                     std::uint64_t& revisionSource) &&;

    friend struct TransitionBuilder;
    friend class InteractionAuthority;

    WholeScreenTruth truth_;
    UiInteractionState interaction_;
    PromptSurface prompt_;
    std::optional<TreeBackingPlan> tree_;
};

[[nodiscard]] std::optional<PreparedTransition> prepareTransition(
    const CommandTransition& transition, const TransitionInputs& inputs);

}  // namespace ssg
