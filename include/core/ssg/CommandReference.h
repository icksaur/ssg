#pragma once

// The command reference, rendered from the catalog.
//
// The library describes itself: given a catalog, this produces the whole of
// `doc/commands.md`.  It holds formatting only -- no command data -- so the
// catalog stays the single source.
//
// This is not a build step.  Populating a catalog means running registration,
// which needs a runtime, and constructing one creates directories; a build must
// not do filesystem work.  `test_commands` renders the reference from a runtime
// in a temporary workspace and asserts the committed file matches.

#include <string>

namespace ssg {

class CommandCatalog;

// Formats the catalog as `doc/commands.md`.  Stateless: it is an object only so
// the rendering is a named operation rather than a free function.
class CommandReferenceRenderer {
public:
    // Throws std::invalid_argument when a command declares an argument type with
    // no human name, which would otherwise render as a silently blank column.
    [[nodiscard]] std::string render(CommandCatalog const& catalog) const;
};

}  // namespace ssg
