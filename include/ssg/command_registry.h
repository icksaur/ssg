#pragma once

#include <ssg/types.h>

#include <any>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace ssg {

struct ClientId {
    explicit constexpr ClientId(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(ClientId const&) const noexcept = default;

private:
    std::uint64_t value_;
};

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

class CapabilityId {
public:
    explicit CapabilityId(std::string value);

    [[nodiscard]] std::string_view value() const noexcept { return value_; }
    auto operator<=>(CapabilityId const&) const = default;

private:
    std::string value_;
};

enum class InvocationOrigin : std::uint8_t {
    in_process,
    websocket,
    lua,
    system,
};

class InvocationPrincipal {
public:
    InvocationPrincipal(ClientId client_id, InvocationOrigin origin,
                        std::vector<CapabilityId> capabilities = {});
    InvocationPrincipal(InvocationPrincipal const&) = default;
    InvocationPrincipal(InvocationPrincipal&&) noexcept = default;
    InvocationPrincipal& operator=(InvocationPrincipal const&) = delete;
    InvocationPrincipal& operator=(InvocationPrincipal&&) = delete;

    [[nodiscard]] ClientId client_id() const noexcept { return client_id_; }
    [[nodiscard]] InvocationOrigin origin() const noexcept { return origin_; }
    [[nodiscard]] std::vector<CapabilityId> const& capabilities() const noexcept {
        return capabilities_;
    }
    [[nodiscard]] bool has_capability(CapabilityId const& capability) const;

private:
    ClientId client_id_;
    InvocationOrigin origin_;
    std::vector<CapabilityId> capabilities_;
};

class EditorSession;
struct CommandHandlerResult;

class CommandServices {
public:
    virtual ~CommandServices() = default;

    template <typename State>
    [[nodiscard]] State& feature_state() {
        return std::any_cast<State&>(feature_state_value(typeid(State)));
    }

    template <typename Status>
    void publish_status(Status status) {
        publish_status_value(typeid(Status), std::any{std::move(status)});
    }

    template <typename Delta>
    void publish_delta(Delta delta) {
        publish_delta_value(typeid(Delta), std::any{std::move(delta)});
    }

    [[nodiscard]] virtual CommandHandlerResult run_transaction(
        std::function<CommandHandlerResult()> operation) = 0;

private:
    [[nodiscard]] virtual std::any& feature_state_value(
        std::type_index type) = 0;
    virtual void publish_status_value(std::type_index type,
                                      std::any status) = 0;
    virtual void publish_delta_value(std::type_index type,
                                     std::any delta) = 0;
};

class CommandContext {
public:
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] InvocationPrincipal const& principal() const noexcept {
        return principal_;
    }
    [[nodiscard]] CommandServices* services() const noexcept {
        return services_;
    }

    void set_active_workspace(WorkspaceId workspace) noexcept;
    void set_active_view(ViewId view) noexcept;

private:
    friend class EditorSession;

    CommandContext(Revision revision, InvocationPrincipal const& principal,
                   CommandServices* services)
        : revision_{revision}, principal_{principal}, services_{services} {}

    Revision revision_;
    InvocationPrincipal const& principal_;
    CommandServices* services_;
    bool workspace_changed_{false};
    WorkspaceId active_workspace_;
    bool view_changed_{false};
    ViewId active_view_;
};

enum class CommandEffect : std::uint8_t {
    observation,
    mutation,
};

struct CommandHandlerResult {
    bool accepted;
    std::string message;

    [[nodiscard]] static CommandHandlerResult success();
    [[nodiscard]] static CommandHandlerResult failure(std::string message);
};

using CommandHandler =
    std::function<CommandHandlerResult(CommandContext&, std::any const&)>;

struct CommandDescriptor {
    std::string id;
    CommandEffect effect;
    std::vector<CapabilityId> required_capabilities;
};

struct CommandRegistration {
    CommandDescriptor descriptor;
    CommandHandler handler;
};

class CommandSet {
public:
    explicit CommandSet(std::vector<CommandRegistration> commands);

    [[nodiscard]] std::vector<CommandRegistration> const& commands() const
        noexcept {
        return commands_;
    }

private:
    std::vector<CommandRegistration> commands_;
};

class CommandRegistry {
public:
    explicit CommandRegistry(std::vector<CommandSet> command_sets);
    ~CommandRegistry();

    CommandRegistry(CommandRegistry const&) = delete;
    CommandRegistry& operator=(CommandRegistry const&) = delete;
    CommandRegistry(CommandRegistry&&) noexcept;
    CommandRegistry& operator=(CommandRegistry&&) noexcept;

    [[nodiscard]] CommandRegistration const* find(
        std::string_view command_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
