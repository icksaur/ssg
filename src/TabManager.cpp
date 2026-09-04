#include <ssg/TabManager.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

TabResult failure(TabError error, std::string message) {
    return {error, std::move(message), {}, {}};
}

bool sameIdentity(const TabState& left, const TabState& right) {
    if (left.kind != right.kind) {
        return false;
    }
    if (left.kind == TabKind::Document) {
        return left.documentKey == right.documentKey;
    }
    return left.contentIdentity == right.contentIdentity;
}

std::optional<std::string> invalidState(const TabViewState& state) {
    if (state.tabs.empty() != !state.active.has_value()) {
        return "active tab must be present exactly when tabs are present";
    }
    for (std::size_t index = 0; index < state.tabs.size(); ++index) {
        const auto& tab = state.tabs[index];
        if (tab.id.value() == 0) {
            return "tab id must be non-zero";
        }
        if (tab.label.empty()) {
            return "tab label must be non-empty";
        }
        if (tab.kind == TabKind::Document) {
            if (!tab.document || !tab.documentKey ||
                tab.document->value() == 0 || !tab.contentIdentity.empty()) {
                return "document tab identity is incomplete";
            }
        } else if (tab.document || tab.documentKey ||
                   tab.contentIdentity.empty()) {
            return "non-document tab identity is incomplete";
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (state.tabs[other].id == tab.id) {
                return "tab ids must be unique";
            }
            if (sameIdentity(state.tabs[other], tab)) {
                return "tab content identities must be unique";
            }
        }
    }
    if (state.active &&
        std::none_of(state.tabs.begin(), state.tabs.end(),
                     [&](const TabState& tab) { return tab.id == state.active; })) {
        return "active tab id is not open";
    }
    return std::nullopt;
}

}  // namespace

struct TabManager::Impl {
    struct ClosedTab {
        TabState state;
        std::size_t index;
        RecoveryRecordId compensation;
    };

    explicit Impl(TabManagerConfig managerConfig) : config{managerConfig} {
        if (config.maximumRecentlyClosed == 0) {
            throw std::invalid_argument(
                "maximum recently-closed tabs must be greater than zero");
        }
    }

    [[nodiscard]] auto find(TabId id) {
        return std::find_if(view.tabs.begin(), view.tabs.end(),
                            [id](const TabState& tab) { return tab.id == id; });
    }

    [[nodiscard]] auto find(TabId id) const {
        return std::find_if(view.tabs.cbegin(), view.tabs.cend(),
                            [id](const TabState& tab) { return tab.id == id; });
    }

    [[nodiscard]] std::string untitledLabel() const {
        for (std::size_t number = 1;; ++number) {
            const auto candidate = "Untitled " + std::to_string(number);
            if (std::none_of(view.tabs.begin(), view.tabs.end(),
                             [&](const TabState& tab) {
                                 return tab.label == candidate;
                             })) {
                return candidate;
            }
        }
    }

    [[nodiscard]] const TabLifecycleResult* outcomeFor(
        TabId id, const std::vector<TabCloseOutcome>& outcomes) const {
        const auto found = std::find_if(
            outcomes.begin(), outcomes.end(),
            [id](const TabCloseOutcome& outcome) { return outcome.tab == id; });
        return found == outcomes.end() ? nullptr : &found->result;
    }

    // I2: a dirty tab is removed only after an accepted durable outcome; only an
    // ephemeral tab may close without compensation or recently-closed history.
    [[nodiscard]] TabResult closeAt(
        std::size_t index, TabLifecycleResult result,
        std::optional<std::size_t> recordedIndex = {}) {
        const auto tab = view.tabs[index];
        if (!result.accepted()) {
            return failure(result.error, std::move(result.message));
        }
        if (tab.dirty && !result.durable) {
            return failure(TabError::DurabilityFailed,
                           "dirty close did not become durable");
        }
        if (!result.compensation) {
            if (!result.ephemeral) {
                return failure(TabError::LifecycleFailed,
                               "close did not provide a reopen record");
            }
            const auto wasActive = view.active == tab.id;
            view.tabs.erase(view.tabs.begin() + static_cast<std::ptrdiff_t>(index));
            if (view.tabs.empty()) {
                view.active.reset();
            } else if (wasActive) {
                const auto replacement = std::min(index, view.tabs.size() - 1);
                view.active = view.tabs[replacement].id;
            }
            return {TabError::None, {}, tab.id, {}};
        }

        recentlyClosed.push_back(
            {tab, recordedIndex.value_or(index), *result.compensation});
        if (recentlyClosed.size() > config.maximumRecentlyClosed) {
            recentlyClosed.erase(recentlyClosed.begin());
        }

        const auto wasActive = view.active == tab.id;
        view.tabs.erase(view.tabs.begin() + static_cast<std::ptrdiff_t>(index));
        if (view.tabs.empty()) {
            view.active.reset();
        } else if (wasActive) {
            const auto replacement = std::min(index, view.tabs.size() - 1);
            view.active = view.tabs[replacement].id;
        }
        return {TabError::None, {}, tab.id, {}};
    }

    // I3: close-all and close-others apply outcomes in original left-to-right
    // tab order, keep rejected or omitted targets open, and preserve active-tab
    // selection with the existing final reconciliation rule.
    [[nodiscard]] TabResult closeBatch(std::optional<TabId> keep,
                                       const std::vector<TabCloseOutcome>& outcomes) {
        if (view.tabs.empty()) {
            return failure(TabError::NoTabs, "no tabs are open");
        }

        const auto original = view.tabs;
        const auto originalActive = view.active;
        const auto activePosition = static_cast<std::size_t>(std::distance(
            original.begin(),
            std::find_if(original.begin(), original.end(), [&](const TabState& tab) {
                return tab.id == originalActive;
            })));

        TabResult aggregate;
        for (std::size_t originalIndex = 0; originalIndex < original.size();
             ++originalIndex) {
            const auto id = original[originalIndex].id;
            if (keep == id) {
                continue;
            }
            const auto* outcome = outcomeFor(id, outcomes);
            if (outcome == nullptr) {
                aggregate.failures.push_back(
                    {id, TabError::LifecycleFailed, "close outcome missing for tab"});
                continue;
            }
            const auto current = find(id);
            if (current == view.tabs.end()) {
                continue;
            }
            const auto index =
                static_cast<std::size_t>(std::distance(view.tabs.begin(), current));
            auto result = closeAt(index, *outcome, originalIndex);
            if (!result.accepted()) {
                aggregate.failures.push_back(
                    {id, result.error, std::move(result.message)});
            }
        }

        if (!view.tabs.empty()) {
            const auto activeSurvives =
                originalActive &&
                std::any_of(view.tabs.begin(), view.tabs.end(),
                            [&](const TabState& tab) {
                                return tab.id == originalActive;
                            });
            if (activeSurvives) {
                view.active = originalActive;
            } else {
                const auto atOrRight = std::find_if(
                    original.begin() +
                        static_cast<std::ptrdiff_t>(
                            std::min(activePosition, original.size())),
                    original.end(), [&](const TabState& candidate) {
                        return find(candidate.id) != view.tabs.end();
                    });
                if (atOrRight != original.end()) {
                    view.active = atOrRight->id;
                } else {
                    view.active = view.tabs.back().id;
                }
            }
        }
        return aggregate;
    }

    TabManagerConfig config;
    TabViewState view;
    std::vector<ClosedTab> recentlyClosed;
    std::uint64_t nextId = 1;
};

TabManager::TabManager(TabManagerConfig config)
    : impl_{std::make_unique<Impl>(config)} {}

TabManager::~TabManager() = default;
TabManager::TabManager(TabManager&&) noexcept = default;
TabManager& TabManager::operator=(TabManager&&) noexcept = default;

const TabViewState& TabManager::viewState() const noexcept {
    return impl_->view;
}

std::size_t TabManager::recentlyClosedCount() const noexcept {
    return impl_->recentlyClosed.size();
}

TabResult TabManager::openDocument(FileDocumentId document,
                                   JournalDocumentKey identity,
                                   std::string_view label,
                                   DocumentMode mode,
                                   bool dirty,
                                   std::optional<ScratchDurability> recovery) {
    if (document.value() == 0) {
        return failure(TabError::InvalidArgument,
                       "document id must be non-zero");
    }
    const auto duplicate = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [&](const TabState& tab) {
            return tab.kind == TabKind::Document &&
                   tab.documentKey == identity;
        });
    if (duplicate != impl_->view.tabs.end()) {
        assert(duplicate->document.has_value());
        impl_->view.active = duplicate->id;
        return {TabError::None, {}, duplicate->id, {}};
    }

    std::string resolvedLabel{label};
    if (resolvedLabel.empty()) {
        if (identity.kind() == JournalDocumentKeyKind::Untitled) {
            resolvedLabel = impl_->untitledLabel();
        } else {
            const auto& path = identity.savedPath();
            const auto separator = path.find_last_of("/\\");
            resolvedLabel = path.substr(
                separator == std::string::npos ? 0 : separator + 1);
        }
    }
    if (resolvedLabel.empty()) {
        return failure(TabError::InvalidArgument,
                       "tab label must be non-empty");
    }

    const auto id = TabId{impl_->nextId++};
    impl_->view.tabs.push_back(
        {id, TabKind::Document, document, std::move(identity), {},
         std::move(resolvedLabel), mode, dirty, recovery});
    impl_->view.active = id;
    return {TabError::None, {}, id, {}};
}

TabResult TabManager::openContent(TabKind kind,
                                  std::string_view contentIdentity,
                                  std::string_view label,
                                  DocumentMode mode) {
    if (kind == TabKind::Document || contentIdentity.empty() || label.empty()) {
        return failure(TabError::InvalidArgument,
                       "non-document tabs require kind, identity, and label");
    }
    const auto duplicate = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [&](const TabState& tab) {
            return tab.kind == kind &&
                   tab.contentIdentity == contentIdentity;
        });
    if (duplicate != impl_->view.tabs.end()) {
        impl_->view.active = duplicate->id;
        return {TabError::None, {}, duplicate->id, {}};
    }

    const auto id = TabId{impl_->nextId++};
    impl_->view.tabs.push_back(
        {id, kind, {}, {}, std::string{contentIdentity}, std::string{label},
         mode, false, std::nullopt});
    impl_->view.active = id;
    return {TabError::None, {}, id, {}};
}

TabResult TabManager::updateDocument(FileDocumentId document,
                                     JournalDocumentKey identity,
                                     std::string_view label,
                                     DocumentMode mode,
                                     bool dirty,
                                     std::optional<ScratchDurability> recovery) {
    const auto found = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [document](const TabState& tab) {
            return tab.document == document;
        });
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::NotFound, "document tab is not open");
    }
    found->documentKey = identity;
    found->label = label;
    found->mode = mode;
    found->dirty = dirty;
    found->recovery = recovery;
    return {TabError::None, {}, found->id, {}};
}

std::size_t TabManager::dropDocument(FileDocumentId document) {
    std::size_t removed = 0;
    for (std::size_t index = impl_->view.tabs.size(); index > 0; --index) {
        const auto position = index - 1;
        if (impl_->view.tabs[position].document != document) continue;
        const auto wasActive =
            impl_->view.active == impl_->view.tabs[position].id;
        impl_->view.tabs.erase(impl_->view.tabs.begin() +
                               static_cast<std::ptrdiff_t>(position));
        ++removed;
        if (impl_->view.tabs.empty()) {
            impl_->view.active.reset();
        } else if (wasActive) {
            const auto replacement =
                std::min(position, impl_->view.tabs.size() - 1);
            impl_->view.active = impl_->view.tabs[replacement].id;
        }
    }
    return removed;
}

TabResult TabManager::activate(TabId tab) {
    if (impl_->find(tab) == impl_->view.tabs.end()) {
        return failure(TabError::NotFound, "tab is not open");
    }
    impl_->view.active = tab;
    return {TabError::None, {}, tab, {}};
}

TabResult TabManager::next() {
    if (impl_->view.tabs.empty()) {
        return failure(TabError::NoTabs, "no tabs are open");
    }
    const auto active = impl_->find(*impl_->view.active);
    const auto index =
        static_cast<std::size_t>(std::distance(impl_->view.tabs.begin(), active));
    impl_->view.active =
        impl_->view.tabs[(index + 1) % impl_->view.tabs.size()].id;
    return {TabError::None, {}, impl_->view.active, {}};
}

TabResult TabManager::previous() {
    if (impl_->view.tabs.empty()) {
        return failure(TabError::NoTabs, "no tabs are open");
    }
    const auto active = impl_->find(*impl_->view.active);
    const auto index =
        static_cast<std::size_t>(std::distance(impl_->view.tabs.begin(), active));
    impl_->view.active =
        impl_->view.tabs[(index + impl_->view.tabs.size() - 1) %
                         impl_->view.tabs.size()]
            .id;
    return {TabError::None, {}, impl_->view.active, {}};
}

TabResult TabManager::moveLeft(TabId tab) {
    const auto found = impl_->find(tab);
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::NotFound, "tab is not open");
    }
    const auto index =
        static_cast<std::size_t>(std::distance(impl_->view.tabs.begin(), found));
    if (index > 0) {
        std::iter_swap(found - 1, found);
    }
    return {TabError::None, {}, tab, {}};
}

TabResult TabManager::moveRight(TabId tab) {
    const auto found = impl_->find(tab);
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::NotFound, "tab is not open");
    }
    if (found + 1 != impl_->view.tabs.end()) {
        std::iter_swap(found, found + 1);
    }
    return {TabError::None, {}, tab, {}};
}

TabResult TabManager::close(TabId tab, TabLifecycleResult result) {
    const auto found = impl_->find(tab);
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::NotFound, "tab is not open");
    }
    return impl_->closeAt(
        static_cast<std::size_t>(
            std::distance(impl_->view.tabs.begin(), found)),
        std::move(result));
}

TabResult TabManager::closeOthers(TabId tab,
                                  std::vector<TabCloseOutcome> outcomes) {
    if (impl_->find(tab) == impl_->view.tabs.end()) {
        return failure(TabError::NotFound, "tab is not open");
    }
    return impl_->closeBatch(tab, outcomes);
}

TabResult TabManager::closeAll(std::vector<TabCloseOutcome> outcomes) {
    return impl_->closeBatch({}, outcomes);
}

std::variant<TabResult, TabReopenRequest> TabManager::beginReopenClosed() {
    if (impl_->recentlyClosed.empty()) {
        return failure(TabError::NoRecentlyClosed,
                       "no recently closed tab is available");
    }
    auto& closed = impl_->recentlyClosed.back();
    const auto existing = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [&](const TabState& tab) { return sameIdentity(tab, closed.state); });
    if (existing != impl_->view.tabs.end()) {
        const auto id = existing->id;
        impl_->view.active = id;
        impl_->recentlyClosed.pop_back();
        return TabResult{TabError::None, {}, id, {}};
    }
    return TabReopenRequest{closed.state, closed.compensation, closed.index};
}

TabResult TabManager::finishReopenClosed(TabReopenRequest request,
                                         TabLifecycleResult result) {
    // I4: matching open identities are handled by beginReopenClosed without an
    // effect; a rejected effect keeps history; success restores position and any
    // replacement document identity.
    if (impl_->recentlyClosed.empty()) {
        return failure(TabError::LifecycleFailed, "reopen request is stale");
    }
    const auto& closed = impl_->recentlyClosed.back();
    if (!sameIdentity(closed.state, request.tab) ||
        closed.compensation != request.compensation) {
        return failure(TabError::LifecycleFailed, "reopen request is stale");
    }
    if (!result.accepted()) {
        return failure(result.error, std::move(result.message));
    }

    auto restoredState = std::move(request.tab);
    if (restoredState.kind == TabKind::Document &&
        result.reopenedDocument.has_value()) {
        restoredState.document = result.reopenedDocument;
        if (result.reopenedDocumentKey.has_value()) {
            restoredState.documentKey = result.reopenedDocumentKey;
        }
    }
    const auto index = std::min(request.index, impl_->view.tabs.size());
    const auto id = restoredState.id;
    impl_->view.tabs.insert(
        impl_->view.tabs.begin() + static_cast<std::ptrdiff_t>(index),
        std::move(restoredState));
    impl_->view.active = id;
    impl_->recentlyClosed.pop_back();
    return {TabError::None, {}, id, {}};
}

}  // namespace ssg
