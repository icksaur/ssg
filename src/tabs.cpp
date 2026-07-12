#include <ssg/tabs.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ssg {
namespace {

TabResult failure(TabError error, std::string message) {
    return {error, std::move(message), {}, {}};
}

bool same_identity(const TabState& left, const TabState& right) {
    if (left.kind != right.kind) {
        return false;
    }
    if (left.kind == TabKind::document) {
        return left.document_key == right.document_key;
    }
    return left.content_identity == right.content_identity;
}

std::optional<std::string> invalid_state(const TabViewState& state) {
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
        if (tab.kind == TabKind::document) {
            if (!tab.document || !tab.document_key ||
                tab.document->value() == 0 || !tab.content_identity.empty()) {
                return "document tab identity is incomplete";
            }
        } else if (tab.document || tab.document_key ||
                   tab.content_identity.empty()) {
            return "non-document tab identity is incomplete";
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (state.tabs[other].id == tab.id) {
                return "tab ids must be unique";
            }
            if (same_identity(state.tabs[other], tab)) {
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

TabDelta derive_tab_delta(const TabViewState& base,
                          const TabViewState& target) {
    return base == target ? TabDelta{} : TabDelta{target};
}

TabReplayResult replay_tab_delta(const TabViewState& base,
                                 const TabDelta& delta) {
    const auto& target = delta.state ? *delta.state : base;
    if (const auto error = invalid_state(target)) {
        return {{}, *error};
    }
    return {target, {}};
}

TabManagementCommandSet tab_management_command_set() {
    return {};
}

struct TabManager::Impl {
    struct ClosedTab {
        TabState state;
        std::size_t index;
        RecoveryRecordId compensation;
    };

    Impl(TabLifecycle& tab_lifecycle, TabManagerConfig manager_config)
        : lifecycle{tab_lifecycle}, config{manager_config} {
        if (config.maximum_recently_closed == 0) {
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

    [[nodiscard]] std::string untitled_label() const {
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

    [[nodiscard]] TabResult close_at(
        std::size_t index, std::chrono::milliseconds durability_timeout,
        std::optional<std::size_t> recorded_index = {}) {
        const auto tab = view.tabs[index];
        auto result = lifecycle.close(tab, durability_timeout);
        if (!result.accepted()) {
            return failure(result.error, std::move(result.message));
        }
        if (!result.compensation) {
            return failure(TabError::lifecycle_failed,
                           "close did not provide a reopen record");
        }
        if (tab.dirty && !result.durable) {
            return failure(TabError::durability_failed,
                           "dirty close did not become durable");
        }

        recently_closed.push_back(
            {tab, recorded_index.value_or(index), *result.compensation});
        if (recently_closed.size() > config.maximum_recently_closed) {
            recently_closed.erase(recently_closed.begin());
        }

        const auto was_active = view.active == tab.id;
        view.tabs.erase(view.tabs.begin() + static_cast<std::ptrdiff_t>(index));
        if (view.tabs.empty()) {
            view.active.reset();
        } else if (was_active) {
            const auto replacement = std::min(index, view.tabs.size() - 1);
            view.active = view.tabs[replacement].id;
        }
        return {TabError::none, {}, tab.id, {}};
    }

    [[nodiscard]] TabResult close_batch(
        std::optional<TabId> keep,
        std::chrono::milliseconds durability_timeout) {
        if (durability_timeout <= std::chrono::milliseconds::zero()) {
            return failure(TabError::invalid_argument,
                           "durability timeout must be positive");
        }
        if (view.tabs.empty()) {
            return failure(TabError::no_tabs, "no tabs are open");
        }

        const auto original = view.tabs;
        const auto original_active = view.active;
        const auto active_position = static_cast<std::size_t>(std::distance(
            original.begin(),
            std::find_if(original.begin(), original.end(), [&](const TabState& tab) {
                return tab.id == original_active;
            })));

        TabResult aggregate;
        for (std::size_t original_index = 0; original_index < original.size();
             ++original_index) {
            const auto id = original[original_index].id;
            if (keep == id) {
                continue;
            }
            const auto current = find(id);
            const auto index =
                static_cast<std::size_t>(std::distance(view.tabs.begin(), current));
            auto result = close_at(index, durability_timeout, original_index);
            if (!result.accepted()) {
                aggregate.failures.push_back(
                    {id, result.error, std::move(result.message)});
            }
        }

        if (!view.tabs.empty()) {
            const auto active_survives =
                original_active &&
                std::any_of(view.tabs.begin(), view.tabs.end(),
                            [&](const TabState& tab) {
                                return tab.id == original_active;
                            });
            if (active_survives) {
                view.active = original_active;
            } else {
                const auto at_or_right = std::find_if(
                    original.begin() +
                        static_cast<std::ptrdiff_t>(
                            std::min(active_position, original.size())),
                    original.end(), [&](const TabState& candidate) {
                        return find(candidate.id) != view.tabs.end();
                    });
                if (at_or_right != original.end()) {
                    view.active = at_or_right->id;
                } else {
                    view.active = view.tabs.back().id;
                }
            }
        }
        return aggregate;
    }

    TabLifecycle& lifecycle;
    TabManagerConfig config;
    TabViewState view;
    std::vector<ClosedTab> recently_closed;
    std::uint64_t next_id = 1;
};

TabManager::TabManager(TabLifecycle& lifecycle, TabManagerConfig config)
    : impl_{std::make_unique<Impl>(lifecycle, config)} {}

TabManager::~TabManager() = default;
TabManager::TabManager(TabManager&&) noexcept = default;
TabManager& TabManager::operator=(TabManager&&) noexcept = default;

const TabViewState& TabManager::view_state() const noexcept {
    return impl_->view;
}

std::size_t TabManager::recently_closed_count() const noexcept {
    return impl_->recently_closed.size();
}

TabResult TabManager::open_document(FileDocumentId document,
                                    JournalDocumentKey identity,
                                    std::string_view label,
                                    DocumentMode mode,
                                    bool dirty,
                                    TabRecoveryBadge recovery) {
    if (document.value() == 0) {
        return failure(TabError::invalid_argument,
                       "document id must be non-zero");
    }
    const auto duplicate = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [&](const TabState& tab) {
            return tab.kind == TabKind::document &&
                   tab.document_key == identity;
        });
    if (duplicate != impl_->view.tabs.end()) {
        impl_->view.active = duplicate->id;
        return {TabError::none, {}, duplicate->id, {}};
    }

    std::string resolved_label{label};
    if (resolved_label.empty()) {
        if (identity.kind() == JournalDocumentKeyKind::untitled) {
            resolved_label = impl_->untitled_label();
        } else {
            const auto& path = identity.saved_path();
            const auto separator = path.find_last_of("/\\");
            resolved_label = path.substr(
                separator == std::string::npos ? 0 : separator + 1);
        }
    }
    if (resolved_label.empty()) {
        return failure(TabError::invalid_argument,
                       "tab label must be non-empty");
    }

    const auto id = TabId{impl_->next_id++};
    impl_->view.tabs.push_back(
        {id, TabKind::document, document, std::move(identity), {},
         std::move(resolved_label), mode, dirty, recovery});
    impl_->view.active = id;
    return {TabError::none, {}, id, {}};
}

TabResult TabManager::open_content(TabKind kind,
                                   std::string_view content_identity,
                                   std::string_view label,
                                   DocumentMode mode) {
    if (kind == TabKind::document || content_identity.empty() || label.empty()) {
        return failure(TabError::invalid_argument,
                       "non-document tabs require kind, identity, and label");
    }
    const auto duplicate = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [&](const TabState& tab) {
            return tab.kind == kind &&
                   tab.content_identity == content_identity;
        });
    if (duplicate != impl_->view.tabs.end()) {
        impl_->view.active = duplicate->id;
        return {TabError::none, {}, duplicate->id, {}};
    }

    const auto id = TabId{impl_->next_id++};
    impl_->view.tabs.push_back(
        {id, kind, {}, {}, std::string{content_identity}, std::string{label},
         mode, false, TabRecoveryBadge::none});
    impl_->view.active = id;
    return {TabError::none, {}, id, {}};
}

TabResult TabManager::update_document(FileDocumentId document,
                                      DocumentMode mode,
                                      bool dirty,
                                      TabRecoveryBadge recovery) {
    const auto found = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [document](const TabState& tab) {
            return tab.document == document;
        });
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::not_found, "document tab is not open");
    }
    found->mode = mode;
    found->dirty = dirty;
    found->recovery = recovery;
    return {TabError::none, {}, found->id, {}};
}

TabResult TabManager::activate(TabId tab) {
    if (impl_->find(tab) == impl_->view.tabs.end()) {
        return failure(TabError::not_found, "tab is not open");
    }
    impl_->view.active = tab;
    return {TabError::none, {}, tab, {}};
}

TabResult TabManager::next() {
    if (impl_->view.tabs.empty()) {
        return failure(TabError::no_tabs, "no tabs are open");
    }
    const auto active = impl_->find(*impl_->view.active);
    const auto index =
        static_cast<std::size_t>(std::distance(impl_->view.tabs.begin(), active));
    impl_->view.active =
        impl_->view.tabs[(index + 1) % impl_->view.tabs.size()].id;
    return {TabError::none, {}, impl_->view.active, {}};
}

TabResult TabManager::previous() {
    if (impl_->view.tabs.empty()) {
        return failure(TabError::no_tabs, "no tabs are open");
    }
    const auto active = impl_->find(*impl_->view.active);
    const auto index =
        static_cast<std::size_t>(std::distance(impl_->view.tabs.begin(), active));
    impl_->view.active =
        impl_->view.tabs[(index + impl_->view.tabs.size() - 1) %
                         impl_->view.tabs.size()]
            .id;
    return {TabError::none, {}, impl_->view.active, {}};
}

TabResult TabManager::move_left(TabId tab) {
    const auto found = impl_->find(tab);
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::not_found, "tab is not open");
    }
    const auto index =
        static_cast<std::size_t>(std::distance(impl_->view.tabs.begin(), found));
    if (index > 0) {
        std::iter_swap(found - 1, found);
    }
    return {TabError::none, {}, tab, {}};
}

TabResult TabManager::move_right(TabId tab) {
    const auto found = impl_->find(tab);
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::not_found, "tab is not open");
    }
    if (found + 1 != impl_->view.tabs.end()) {
        std::iter_swap(found, found + 1);
    }
    return {TabError::none, {}, tab, {}};
}

TabResult TabManager::close(
    TabId tab, std::chrono::milliseconds durability_timeout) {
    if (durability_timeout <= std::chrono::milliseconds::zero()) {
        return failure(TabError::invalid_argument,
                       "durability timeout must be positive");
    }
    const auto found = impl_->find(tab);
    if (found == impl_->view.tabs.end()) {
        return failure(TabError::not_found, "tab is not open");
    }
    return impl_->close_at(
        static_cast<std::size_t>(
            std::distance(impl_->view.tabs.begin(), found)),
        durability_timeout);
}

TabResult TabManager::close_others(
    TabId tab, std::chrono::milliseconds durability_timeout) {
    if (impl_->find(tab) == impl_->view.tabs.end()) {
        return failure(TabError::not_found, "tab is not open");
    }
    return impl_->close_batch(tab, durability_timeout);
}

TabResult TabManager::close_all(
    std::chrono::milliseconds durability_timeout) {
    return impl_->close_batch({}, durability_timeout);
}

TabResult TabManager::reopen_closed() {
    if (impl_->recently_closed.empty()) {
        return failure(TabError::no_recently_closed,
                       "no recently closed tab is available");
    }
    auto& closed = impl_->recently_closed.back();
    const auto existing = std::find_if(
        impl_->view.tabs.begin(), impl_->view.tabs.end(),
        [&](const TabState& tab) { return same_identity(tab, closed.state); });
    if (existing != impl_->view.tabs.end()) {
        const auto id = existing->id;
        impl_->view.active = id;
        impl_->recently_closed.pop_back();
        return {TabError::none, {}, id, {}};
    }
    auto restored =
        impl_->lifecycle.reopen(closed.state, closed.compensation);
    if (!restored.accepted()) {
        return failure(restored.error, std::move(restored.message));
    }
    const auto index = std::min(closed.index, impl_->view.tabs.size());
    const auto id = closed.state.id;
    impl_->view.tabs.insert(
        impl_->view.tabs.begin() + static_cast<std::ptrdiff_t>(index),
        std::move(closed.state));
    impl_->view.active = id;
    impl_->recently_closed.pop_back();
    return {TabError::none, {}, id, {}};
}

}  // namespace ssg
