#include <ssg/CommandCatalog.h>

#include <mutex>
#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

// A registration is rejected for exactly one reason at a time, named, so a
// component author is told which field they left out rather than that
// "registration failed".
void requireField(bool present, std::string_view id, std::string_view field) {
    if (present) return;
    throw std::runtime_error{"command \"" + std::string{id} +
                             "\" is missing its " + std::string{field}};
}

}  // namespace

CommandCatalog::CommandCatalog() = default;
CommandCatalog::~CommandCatalog() = default;

CommandHandle CommandCatalog::add(CommandSpecBuilder spec) {
    requireField(!spec.id_.empty(), "<unnamed>", "id");
    requireField(!spec.owner_.empty(), spec.id_, "owner");
    requireField(!spec.summary_.empty(), spec.id_, "summary");
    requireField(spec.effect_.has_value(), spec.id_,
                 "effect (call mutates() or observes())");
    requireField(static_cast<bool>(spec.handler_), spec.id_, "handler");

    std::unique_lock lock{mutex_};

    if (auto const existing = byId_.find(spec.id_); existing != byId_.end()) {
        throw std::runtime_error{"command \"" + spec.id_ +
                                 "\" is already registered by owner \"" +
                                 entries_[existing->second].owner +
                                 "\"; second registration by \"" + spec.owner_ +
                                 "\""};
    }

    auto const index = entries_.size();
    entries_.push_back(CommandEntry{
        std::move(spec.id_), std::move(spec.owner_), std::move(spec.label_),
        std::move(spec.summary_), *spec.effect_,
        std::move(spec.capabilities_), spec.luaApi_, spec.initScript_,
        spec.argument_, std::move(spec.handler_), false});
    byId_.emplace(entries_[index].id, index);
    ++revision_;

    return commandHandleFromIndex(index);
}

CatalogRevision CommandCatalog::revision() const {
    std::shared_lock lock{mutex_};
    return revision_;
}

CommandEntry const* CommandCatalog::find(std::string_view id) const {
    std::shared_lock lock{mutex_};
    auto const found = byId_.find(std::string{id});
    if (found == byId_.end()) return nullptr;
    auto const* entry = &entries_[found->second];
    return entry->retired ? nullptr : entry;
}

CommandEntry const* CommandCatalog::find(CommandHandle command) const {
    if (!command.valid()) return nullptr;
    std::shared_lock lock{mutex_};
    if (command.index() >= entries_.size()) return nullptr;
    auto const* entry = &entries_[command.index()];
    return entry->retired ? nullptr : entry;
}

CommandHandle CommandCatalog::handleFor(std::string_view id) const {
    std::shared_lock lock{mutex_};
    auto const found = byId_.find(std::string{id});
    if (found == byId_.end()) return {};
    if (entries_[found->second].retired) return {};
    return commandHandleFromIndex(found->second);
}

std::vector<CommandEntry const*> CommandCatalog::commands() const {
    std::shared_lock lock{mutex_};
    std::vector<CommandEntry const*> live;
    live.reserve(entries_.size());
    for (auto const& entry : entries_) {
        if (!entry.retired) live.push_back(&entry);
    }
    return live;
}

std::vector<CommandEntry const*> CommandCatalog::ownedBy(
    std::string_view owner) const {
    std::shared_lock lock{mutex_};
    std::vector<CommandEntry const*> owned;
    for (auto const& entry : entries_) {
        if (!entry.retired && entry.owner == owner) owned.push_back(&entry);
    }
    return owned;
}

std::size_t CommandCatalog::size() const {
    std::shared_lock lock{mutex_};
    std::size_t live = 0;
    for (auto const& entry : entries_) {
        if (!entry.retired) ++live;
    }
    return live;
}

}  // namespace ssg
