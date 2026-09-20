#pragma once

#include <ssg/ViewAction.h>

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {

enum class CommandError : unsigned char {
    None,
    UnknownCommand,
    HandlerFailed,
};

struct CommandResult {
    CommandError error = CommandError::None;
    std::string message;
    std::optional<ViewAction> viewAction;

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::None;
    }
    [[nodiscard]] bool completed() const noexcept {
        return accepted() && !viewAction;
    }
};

inline constexpr std::string_view kNestedDispatchRefusal =
    "a command handler may not dispatch another command directly; ask for "
    "it instead, so commands remain serialized";

// CMD-1: every Command is invocable without a payload from every command surface.
struct Command {
    std::string label;
    std::function<CommandResult()> handler;
};

// CMD-2: the map key is a command's sole identity.
class Commands {
public:
    using Map = std::map<std::string, Command>;
    using Replacements = std::vector<std::pair<std::string, Command>>;

    // CMD-3: id, label, and handler are validated together before this map changes.
    void add(std::string id, std::string label,
             std::function<CommandResult()> handler) {
        rejectMutationDuringDispatch();
        validate(id, label, handler);
        if (commands_.contains(id)) {
            throw std::runtime_error{"command is already registered: " + id};
        }
        commands_.emplace(std::move(id),
                          Command{std::move(label), std::move(handler)});
    }

    [[nodiscard]] Command const* find(std::string_view id) const {
        const auto found = commands_.find(std::string{id});
        return found == commands_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] Map const& all() const noexcept { return commands_; }

    [[nodiscard]] bool dispatchInProgress() const noexcept {
        return dispatching_;
    }

    // CMD-7: this result, including its optional ViewAction, is authoritative.
    [[nodiscard]] CommandResult dispatch(std::string_view id) {
        if (dispatching_) {
            return {CommandError::HandlerFailed,
                    std::string{kNestedDispatchRefusal}, std::nullopt};
        }
        const auto* command = find(id);
        if (command == nullptr) {
            return {CommandError::UnknownCommand,
                    "command is not registered: " + std::string{id},
                    std::nullopt};
        }

        struct DispatchScope {
            explicit DispatchScope(bool& dispatching) : dispatching_{dispatching} {
                dispatching_ = true;
            }
            ~DispatchScope() { dispatching_ = false; }
            bool& dispatching_;
        } scope{dispatching_};
        try {
            return command->handler();
        } catch (std::exception const& exception) {
            return {CommandError::HandlerFailed,
                    "command handler threw: " + std::string{exception.what()},
                    std::nullopt};
        } catch (...) {
            return {CommandError::HandlerFailed,
                    "command handler threw an unknown exception", std::nullopt};
        }
    }

    // CMD-4: replacement validates a copied map before it becomes live.
    void replace(std::span<std::string const> oldIds,
                 Replacements replacements) {
        rejectMutationDuringDispatch();
        Map next = commands_;
        for (auto const& id : oldIds) next.erase(id);
        for (auto& [id, command] : replacements) {
            validate(id, command.label, command.handler);
            if (next.contains(id)) {
                throw std::runtime_error{"command is already registered: " + id};
            }
            next.emplace(std::move(id), std::move(command));
        }
        commands_ = std::move(next);
    }

private:
    void rejectMutationDuringDispatch() const {
        if (dispatching_) {
            throw std::logic_error{"commands cannot change during dispatch"};
        }
    }

    static void validate(std::string_view id, std::string_view label,
                         std::function<CommandResult()> const& handler) {
        if (id.empty()) throw std::runtime_error{"command ID must not be empty"};
        if (label.empty()) {
            throw std::runtime_error{"command label must not be empty: " +
                                     std::string{id}};
        }
        if (!handler) {
            throw std::runtime_error{"command handler must not be empty: " +
                                     std::string{id}};
        }
    }

    Map commands_;
    bool dispatching_ = false;
};

}  // namespace ssg
