#include <ssg/CommandCatalog.h>

#include <cctype>
#include <mutex>
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

// Title-case a lowercase segment: "line_down" -> "Line Down". Underscores become
// spaces; each word's first letter is uppercased.
std::string humanizeSegment(std::string_view segment) {
    std::string result;
    bool wordStart = true;
    for (char raw : segment) {
        if (raw == '_') {
            result += ' ';
            wordStart = true;
            continue;
        }
        auto ch = static_cast<unsigned char>(raw);
        if (wordStart) {
            result += static_cast<char>(std::toupper(ch));
            wordStart = false;
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result;
}

std::string humanize(std::string_view commandId) {
    std::string result;
    std::size_t begin = 0;
    while (begin <= commandId.size()) {
        auto dot = commandId.find('.', begin);
        auto end = dot == std::string_view::npos ? commandId.size() : dot;
        if (!result.empty()) result += ' ';
        result += humanizeSegment(commandId.substr(begin, end - begin));
        if (dot == std::string_view::npos) break;
        begin = dot + 1;
    }
    return result;
}

// A registration is rejected for exactly one reason at a time, named, so a
// component author is told which field they left out rather than that
// "registration failed".
void requireField(bool present, std::string_view id, std::string_view field) {
    if (present) return;
    throw std::runtime_error{"command \"" + std::string{id} +
                             "\" is missing its " + std::string{field}};
}

}  // namespace

std::string CommandEntry::displayLabel() const {
    return label.empty() ? humanize(id) : label;
}


CommandCatalog::CommandCatalog() = default;
CommandCatalog::~CommandCatalog() = default;

// A spec, checked and with its capabilities converted, ready to become an
// entry.  Producing one cannot fail; everything that can reject a registration
// has already happened.
struct CommandCatalog::ValidatedSpec {
    std::string id;
    std::string owner;
    std::string label;
    std::string summary;
    CommandEffect effect;
    std::vector<CapabilityId> requiredCapabilities;
    bool luaApi;
    bool initScript;
    CommandArgumentType argument;
    CommandHandler handler;
};

// Everything that can refuse a registration, in one place.
//
// Separated from applying it so a batch can be checked as a whole before any of
// it takes effect: validating as it went would let a late spec fail after
// earlier ones had already been installed (doc/spec-lua-commands.md).
CommandCatalog::ValidatedSpec CommandCatalog::validate(
    CommandSpecBuilder spec, std::unordered_set<std::string> const& alsoTaken,
    std::unordered_set<std::string> const& beingFreed) const {
    requireField(!spec.id_.empty(), "<unnamed>", "id");
    requireField(!spec.owner_.empty(), spec.id_, "owner");
    requireField(!spec.summary_.empty(), spec.id_, "summary");
    requireField(spec.effect_.has_value(), spec.id_,
                 "effect (call mutates() or observes())");
    requireField(static_cast<bool>(spec.handler_), spec.id_, "handler");

    // A name the same batch is retiring counts as free: re-registering its own
    // ids is what an ordinary script reload does.
    if (auto const existing = byId_.find(spec.id_);
        existing != byId_.end() && !beingFreed.contains(spec.id_)) {
        throw std::runtime_error{"command \"" + spec.id_ +
                                 "\" is already registered by owner \"" +
                                 entries_[existing->second].owner +
                                 "\"; second registration by \"" + spec.owner_ +
                                 "\""};
    }
    if (alsoTaken.contains(spec.id_)) {
        throw std::runtime_error{"command \"" + spec.id_ +
                                 "\" is registered twice in one batch"};
    }

    // Capabilities become CapabilityId here, so a malformed one is refused at
    // registration -- where the author can see it -- rather than throwing from
    // dispatch, which no caller expects to throw.
    std::vector<CapabilityId> capabilities;
    capabilities.reserve(spec.capabilities_.size());
    for (auto& capability : spec.capabilities_) {
        if (capability.empty()) {
            throw std::runtime_error{"command \"" + spec.id_ +
                                     "\" declares an empty capability"};
        }
        capabilities.emplace_back(std::move(capability));
    }

    return ValidatedSpec{std::move(spec.id_),      std::move(spec.owner_),
                         std::move(spec.label_),   std::move(spec.summary_),
                         *spec.effect_,            std::move(capabilities),
                         spec.luaApi_,             spec.initScript_,
                         spec.argument_,           std::move(spec.handler_)};
}

CommandHandle CommandCatalog::appendValidated(ValidatedSpec spec) {
    auto const index = entries_.size();
    entries_.push_back(CommandEntry{
        std::move(spec.id), std::move(spec.owner), std::move(spec.label),
        std::move(spec.summary), spec.effect,
        std::move(spec.requiredCapabilities), spec.luaApi, spec.initScript,
        spec.argument, std::move(spec.handler), false});
    byId_.emplace(entries_[index].id, index);
    ++revision_;
    return CommandHandle::fromIndex(index);
}

CommandHandle CommandCatalog::add(CommandSpecBuilder spec) {
    std::unique_lock lock{mutex_};
    // A handle is a 16-bit index, so a catalog cannot exceed that space.
    // Truncating would alias a new command onto an existing handle and
    // misdispatch silently, which is the one failure mode handles must not
    // have (doc/spec-command-registry.md, R4).
    requireCapacity(1);
    return appendValidated(validate(std::move(spec), {}, {}));
}

std::vector<CommandHandle> CommandCatalog::replaceGeneration(
    std::span<CommandHandle const> retire,
    std::vector<CommandSpecBuilder> add) {
    std::unique_lock lock{mutex_};

    // Validate phase.  A retired slot is not reclaimed, so the batch must fit
    // in what remains regardless of how much is being retired.
    requireCapacity(add.size());

    std::unordered_set<std::string> freed;
    for (auto const handle : retire) {
        if (!handle.valid() || handle.index() >= entries_.size()) continue;
        auto const& entry = entries_[handle.index()];
        if (!entry.retired) freed.insert(entry.id);
    }

    std::unordered_set<std::string> batchIds;
    std::vector<ValidatedSpec> validated;
    validated.reserve(add.size());
    for (auto& spec : add) {
        auto checked = validate(std::move(spec), batchIds, freed);
        batchIds.insert(checked.id);
        validated.push_back(std::move(checked));
    }

    // Apply phase.  Nothing below can refuse the batch.
    for (auto const handle : retire) {
        if (!handle.valid() || handle.index() >= entries_.size()) continue;
        auto& entry = entries_[handle.index()];
        if (entry.retired) continue;
        entry.retired = true;
        // Free the NAME but keep the slot: the id becomes registerable again,
        // while this handle stays permanently dead.
        byId_.erase(entry.id);
        ++revision_;
    }

    std::vector<CommandHandle> added;
    added.reserve(validated.size());
    for (auto& spec : validated) {
        added.push_back(appendValidated(std::move(spec)));
    }
    return added;
}

void CommandCatalog::requireCapacity(std::size_t additional) const {
    if (entries_.size() + additional > kMaximumCommands) {
        throw std::runtime_error{
            "command catalog is full: at most " +
            std::to_string(kMaximumCommands) + " commands may be registered"};
    }
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
    return CommandHandle::fromIndex(found->second);
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
