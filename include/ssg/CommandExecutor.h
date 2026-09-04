#pragma once

#include <ssg/CommandCatalog.h>
#include <ssg/PaneTopology.h>

#include <memory>
#include <string_view>

namespace ssg {

class CommandCatalog;

struct ExecutorResult {
    CommandError error;
    std::string message;
    std::optional<ViewAction> viewAction;

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::None;
    }
};

// The aggregate checks this before acquiring its non-recursive operation mutex.
// A caller meeting the refusal needs the supported alternative, not only the
// prohibition.
inline constexpr std::string_view kNestedDispatchRefusal =
    "a command handler may not dispatch another command directly; ask for "
    "it instead, so commands remain serialized";

class CommandExecutor {
public:
    explicit CommandExecutor(std::shared_ptr<CommandCatalog> catalog);
    ~CommandExecutor();

    CommandExecutor(CommandExecutor const&) = delete;
    CommandExecutor& operator=(CommandExecutor const&) = delete;
    CommandExecutor(CommandExecutor&&) = delete;
    CommandExecutor& operator=(CommandExecutor&&) = delete;

    // The catalog this session dispatches from.  Held rather than copied, so a
    // command registered later is visible here with no propagation step.
    [[nodiscard]] std::shared_ptr<CommandCatalog> const& catalog() const;

    [[nodiscard]] bool dispatchInProgress() const noexcept;

    [[nodiscard]] ExecutorResult dispatch(ClientCommand const& command);

    [[nodiscard]] SessionTopology topology() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
