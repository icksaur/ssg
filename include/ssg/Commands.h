#pragma once

// The command catalog: the one authored declaration of every SSG command.
//
// A command's id, effect, capabilities, argument shape, exposure surfaces and
// documentation all live in ONE row of the table in Commands.cpp.  Everything
// else -- the assembled registry, the protocol argument codecs, palette labels,
// Lua host grants, and the generated command reference -- is a projection of
// that table (doc/spec-commands.md).
//
// This catalog is hand-authored and reviewed as data.  No tool derives it from
// handlers, bindings or registries; the implementation must satisfy the
// catalog, not the reverse (doc/spec.md, Commands).

#include <ssg/CommandRegistry.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ssg {

// The wire shape of a command's arguments.  Replaces an id-matching if-chain in
// the protocol codec: a command declares its argument shape here, and the codec
// registry maps shape to codec once.  A command that takes no arguments is
// `None` -- which is a declaration, not a fallback.
enum class ArgumentKind : std::uint8_t {
    None,
    TextInput,
    SelectionCommand,
    ScrollLines,
    ScrollPages,
    ScrollFraction,
    DroppedContent,
    ReopenWithEncoding,
    SetEncoding,
    SetLineEnding,
    SetFinalNewline,
    SettingSet,
    SettingReset,
    SettingResetScope,
    WorkspaceReplace,
    WorkspaceApply,
    PaletteExecute,
    TreeSelect,
    FindQuery,
    PromptValue,
};

// Where a command may be invoked from.
//
// `luaApi` and `initScript` are deliberately SEPARATE axes.  `luaApi` is I20
// eligibility: the versioned Lua API may expose this command to a host.
// `initScript` is a grant: `init.lua` at startup may call it, which additionally
// requires argument marshalling to exist for it.  Collapsing the two would
// either shrink Lua parity to the startup set or grant startup authority to
// every eligible command.  `initScript` implies `luaApi`.
//
// There are deliberately no `keymap` or `palette` fields.  Both existed here
// and in the catalog they replaced, and both were read by nothing: key chords
// are the curated `defaultTerminalKeymap()` table, and every command is a
// palette candidate.  A declaration nothing enforces drifts into fiction, so
// the surfaces recorded here are only the ones with a consumer.
struct CommandSurfaces {
    bool luaApi = false;
    bool initScript = false;
};

struct CommandSpec {
    std::string_view id;
    // The feature that implements the command; groups the generated reference.
    std::string_view owner;
    // Palette display text.  Empty means "humanise the id" (see commandLabel).
    std::string_view label;
    // One line, for the generated command reference.
    std::string_view summary;
    CommandEffect effect = CommandEffect::Mutation;
    ArgumentKind argument = ArgumentKind::None;
    CommandSurfaces surfaces;
    // Capability ids required to invoke the command; empty for most.
    std::span<std::string_view const> requiredCapabilities;
};

// Every command, exactly once.
[[nodiscard]] std::span<CommandSpec const> commandCatalog();

// The catalog entry for `id`, or nullptr when no such command exists.
[[nodiscard]] CommandSpec const* findCommand(std::string_view id);

// The commands a feature owns, in catalog order.  Handler-binding loops iterate
// this instead of a per-feature id array, so a feature's command list is not
// restated beside the catalog.
//
// Features whose commands carry extra data (FileCommands' path-prompt and
// live-diff rules, for instance) keep their own descriptor tables: those
// ANNOTATE commands rather than declaring them, and a test asserts their ids
// are exactly this set.
[[nodiscard]] std::vector<CommandSpec const*> commandsOwnedBy(
    std::string_view owner);

}  // namespace ssg
