#pragma once

#include <ssg/EditorClient.h>

#include <memory>
#include <string_view>

namespace ssg {

class CommandCatalog;

struct ExecutorResult {
    CommandError error;
    Revision revision;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::None;
    }
};

// The aggregate checks this before acquiring its non-recursive operation mutex.
// A caller meeting the refusal needs the supported alternative, not only the
// prohibition.
inline constexpr std::string_view kNestedDispatchRefusal =
    "a command handler may not dispatch another command directly; ask for "
    "it instead, so each command still advances the revision exactly once";

class CommandExecutor {
public:
    explicit CommandExecutor(std::shared_ptr<CommandCatalog> catalog,
                             CommandServices* services = nullptr);
    ~CommandExecutor();

    CommandExecutor(CommandExecutor const&) = delete;
    CommandExecutor& operator=(CommandExecutor const&) = delete;
    CommandExecutor(CommandExecutor&&) = delete;
    CommandExecutor& operator=(CommandExecutor&&) = delete;

    // The catalog this session dispatches from.  Held rather than copied, so a
    // command registered later is visible here with no propagation step.
    [[nodiscard]] std::shared_ptr<CommandCatalog> const& catalog() const;

    [[nodiscard]] AttachResult attach(InvocationPrincipal principal,
                                      ViewId viewId);
    [[nodiscard]] bool detach(ClientId clientId);

    // The revision of the dispatch in progress on THIS thread, if there is
    // one.  A handler runs with the session locked, so anything it calls that
    // would take that lock has to ask this first rather than block on a lock
    // its own call already holds.
    [[nodiscard]] std::optional<Revision> activeDispatchRevision()
        const noexcept;

    [[nodiscard]] ExecutorResult dispatch(ClientId clientId,
                                          ClientCommand const& command);

    [[nodiscard]] Revision revision() const;
    // CONTRACT
    // CommandExecutor::advanceRevision advances the revision only for the runtime's
    //   own out-of-band authoritative mutations that do not flow through
    //   dispatch; a client-visible command must reach the revision through
    //   dispatch, so this is never a substitute dispatch path.
    // Used for library-internal, out-of-band authoritative state changes (M10
    // deferred enrichment: the tree scan and syntax highlighting run by
    // prime_deferred after the first frame) so delta clients observe them.
    // Throws on overflow.
    Revision advanceRevision();
    [[nodiscard]] SessionTopology topology() const;
    [[nodiscard]] std::optional<AttachedClient> attachedClient(
        ClientId clientId) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
