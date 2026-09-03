#pragma once

#include <ssg/ViewAction.h>

#include <ssg/CommandHandle.h>

#include <ssg/types.h>

#include <any>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace ssg {

struct WorkspaceId {
    explicit constexpr WorkspaceId(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(WorkspaceId const&) const noexcept = default;

private:
    std::uint64_t value_;
};

struct ViewId {
    explicit constexpr ViewId(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(ViewId const&) const noexcept = default;

private:
    std::uint64_t value_;
};

struct ViewActionRequest {
    ViewId viewId;
    Revision semanticRevision;
    ViewAction action;

    friend bool operator==(const ViewActionRequest&,
                           const ViewActionRequest&) = default;
};

class CommandExecutor;
struct CommandHandlerResult;

class CommandServices {
public:
    virtual ~CommandServices() = default;

    template <typename State>
    [[nodiscard]] State& featureState() {
        return std::any_cast<State&>(featureStateValue(typeid(State)));
    }

    template <typename Status>
    void publishStatus(Status status) {
        publishStatusValue(typeid(Status), std::any{std::move(status)});
    }

    template <typename Delta>
    void publishDelta(Delta delta) {
        publishDeltaValue(typeid(Delta), std::any{std::move(delta)});
    }

    [[nodiscard]] virtual CommandHandlerResult runTransaction(
        std::function<CommandHandlerResult()> operation) = 0;

private:
    [[nodiscard]] virtual std::any& featureStateValue(
        std::type_index type) = 0;
    virtual void publishStatusValue(std::type_index type,
                                      std::any status) = 0;
    virtual void publishDeltaValue(std::type_index type,
                                     std::any delta) = 0;
};

class CommandContext {
public:
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] ViewId viewId() const noexcept { return viewId_; }
    [[nodiscard]] CommandServices* services() const noexcept {
        return services_;
    }

    void setActiveWorkspace(WorkspaceId workspace) noexcept;
    void setActiveView(ViewId view) noexcept;

private:
    friend class CommandExecutor;

    CommandContext(Revision revision, ViewId viewId, CommandServices* services)
        : revision_{revision}, viewId_{viewId}, services_{services} {}

    Revision revision_;
    ViewId viewId_;
    CommandServices* services_;
    bool workspaceChanged_{false};
    WorkspaceId activeWorkspace_;
    bool viewChanged_{false};
    ViewId activeView_;
};

enum class CommandEffect : std::uint8_t {
    Observation,
    Mutation,
    ViewAction,
    Routing,
};

enum class CommandRevisionPolicy : std::uint8_t {
    Exact,
    StateValidated,
};

struct CommandHandlerResult {
    bool accepted;
    std::string message;
    std::optional<ViewAction> viewAction;

    [[nodiscard]] static CommandHandlerResult success();
    [[nodiscard]] static CommandHandlerResult failure(std::string message);
    [[nodiscard]] static CommandHandlerResult requireView(ViewAction action);
};

using CommandHandler =
    std::function<CommandHandlerResult(CommandContext&, std::any const&)>;

struct ClientCommand {
    // The command to invoke, named however the caller most cheaply can: a name
    // at the protocol, Lua and palette boundaries, a handle on the keystroke
    // path.  One field, so a dispatch cannot carry two different commands.
    CommandName id;
    Revision baseRevision;
    std::any payload;
};

enum class CommandError : std::uint8_t {
    None,
    UnknownCommand,
    StaleRevision,
    HandlerFailed,
    RevisionExhausted,
};

struct CommandResult {
    CommandError error;
    // Default-constructed to the null sentinel.  Without the initializer,
    // `CommandResult{}` aggregate-initializes this member from `{}`, which
    // reaches Revision's EXPLICIT constructor -- legal but warned about, and the
    // warning is the honest one: an implicit conversion is being performed
    // through a constructor written to forbid exactly that.
    Revision revision{};
    std::string message;
    std::optional<ViewActionRequest> viewAction;

    enum class Outcome : std::uint8_t {
        Completed,
        ViewActionRequired,
        Rejected,
    };

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::None;
    }
    [[nodiscard]] bool completed() const noexcept {
        return outcome() == Outcome::Completed;
    }
    [[nodiscard]] Outcome outcome() const noexcept {
        if (error != CommandError::None) return Outcome::Rejected;
        return viewAction ? Outcome::ViewActionRequired : Outcome::Completed;
    }
};

}  // namespace ssg
