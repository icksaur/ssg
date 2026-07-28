#pragma once

// The command reference, rendered from the catalog.
//
// The library describes itself: given a catalog, this produces the whole of
// `doc/commands.md`.  It holds formatting only -- no command data -- so the
// catalog stays the single source (doc/spec-command-registry.md).
//
// This is not a build step.  Populating a catalog means running registration,
// which needs a runtime, and constructing one creates directories; a build must
// not do filesystem work.  `test_commands` renders the reference from a runtime
// in a temporary workspace and asserts the committed file matches.

#include <string>
#include <string_view>

namespace ssg {

class CommandCatalog;

// Throws std::invalid_argument when a command declares an argument type with no
// human name, which would otherwise render as a silently blank column.
[[nodiscard]] std::string renderCommandReference(CommandCatalog const& catalog);

// How the reference names a command's argument shape.  Exposed so the
// migration's transition oracle can compare shapes in the same terms a reader
// sees.  Throws for a type with no name.
struct CommandEntry;
[[nodiscard]] std::string_view commandArgumentName(CommandEntry const& command);

}  // namespace ssg
