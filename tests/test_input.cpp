#include <ssg/CommandCatalog.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/focus.h>
#include <ssg/Keymap.h>

#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef SSG_SOURCE_PATH
#error "SSG_SOURCE_PATH must name the backend source directory"
#endif

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

TEST(keyStrokesHaveACanonicalRoundTrip) {
    for (const auto text : {"KeyA", "Ctrl+Shift+KeyM", "Meta+BracketLeft",
                            "Alt+ArrowRight", "F5"}) {
        const auto parsed = ssg::KeyCodec{}.parseStroke(text);
        ASSERT_TRUE(parsed.has_value());
        if (parsed) {
            ASSERT_EQ(ssg::KeyCodec{}.formatStroke(*parsed), std::string{text});
        }
    }
    ASSERT_FALSE(ssg::KeyCodec{}.parseStroke("").has_value());
    ASSERT_FALSE(ssg::KeyCodec{}.parseStroke("Ctrl+Ctrl+KeyA").has_value());
    ASSERT_FALSE(ssg::KeyCodec{}.parseStroke("Hyper+KeyA").has_value());
    ASSERT_FALSE(ssg::KeyCodec{}.parseStroke("Ctrl+").has_value());
}

TEST(validateKeymapFlagsDuplicateAndUnreachableBindings) {
    const auto sequence = *ssg::KeyCodec{}.parseSequence(
        {"Ctrl+Shift+KeyM", "KeyA", "KeyA"});
    ssg::KeymapViewState duplicate{
        "bad", {{sequence, "cursor.left", "editor"},
                {sequence, "cursor.right", "editor"}}};
    const auto duplicateErrors = ssg::KeymapMatcher{duplicate}.validate();
    ASSERT_TRUE(std::ranges::any_of(duplicateErrors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::DuplicateBinding;
    }));

    ssg::KeymapViewState unreachable{
        "bad", {{sequence, "cursor.left", "*"},
                {sequence, "cursor.right", "editor"}}};
    const auto unreachableErrors = ssg::KeymapMatcher{unreachable}.validate();
    ASSERT_TRUE(std::ranges::any_of(unreachableErrors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::UnreachableBinding;
    }));

}

TEST(keymapContextsAreStarPlusFocusNames) {
    const auto contexts = ssg::keymapContexts();
    std::set<std::string_view> actual{contexts.begin(), contexts.end()};
    const std::set<std::string_view> expected{"*", "editor", "panel", "prompt",
                                               "external"};
    ASSERT_TRUE(actual == expected);
    ASSERT_EQ(ssg::focusTargetName(ssg::FocusTarget::Editor),
              std::string_view{"editor"});
    ASSERT_EQ(ssg::focusTargetName(ssg::FocusTarget::Panel),
              std::string_view{"panel"});
    ASSERT_EQ(ssg::focusTargetName(ssg::FocusTarget::Prompt),
              std::string_view{"prompt"});
    ASSERT_EQ(ssg::focusTargetName(ssg::FocusTarget::ExternalModification),
              std::string_view{"external"});
}

namespace {

bool hasError(const std::vector<ssg::KeymapError>& errors,
               ssg::KeymapErrorCode code) {
    return std::ranges::any_of(
        errors, [&](const auto& error) { return error.code == code; });
}

}  // namespace

TEST(validateKeymapRejectsModifiedEnterBindings) {
    // Every Enter keypress is normalized to a bare Enter at the input decoder,
    // so a modified-Enter binding could never fire.  The validator refuses it
    // rather than accept a dead binding.
    const auto bareEnter = *ssg::KeyCodec{}.parseSequence({"Enter"});
    ssg::KeymapViewState ok{"m", {{bareEnter, "text.newline", "editor"}}};
    ASSERT_FALSE(hasError(ssg::KeymapMatcher{ok}.validate(),
                          ssg::KeymapErrorCode::ModifiedEnterBinding));

    for (const auto* modified : {"Shift+Enter", "Ctrl+Enter", "Alt+Enter",
                                 "Meta+Enter"}) {
        const auto sequence = *ssg::KeyCodec{}.parseSequence({modified});
        ssg::KeymapViewState bad{"m", {{sequence, "text.newline", "editor"}}};
        ASSERT_TRUE(hasError(ssg::KeymapMatcher{bad}.validate(),
                             ssg::KeymapErrorCode::ModifiedEnterBinding));
    }

    // The live rebind path (keymap.bind) surfaces the same rejection.
    const auto settingsSeq = *ssg::KeyCodec{}.parseSequence({"Alt+KeyS"});
    ssg::KeymapViewState base{"m", {{settingsSeq, "settings.open", "*"}}};
    ASSERT_FALSE(
        ssg::applyKeymapBind(base, {"Shift+Enter", "text.newline", "editor"})
            .accepted());
    ASSERT_TRUE(
        ssg::applyKeymapBind(base, {"Enter", "text.newline", "editor"})
            .accepted());
}

TEST(validateKeymapRejectsUnknownContext) {
    const auto seq = *ssg::KeyCodec{}.parseSequence({"ArrowDown"});
    ssg::KeymapViewState bad{"bad", {{seq, "cursor.line_down", "sidebar"}}};
    ASSERT_TRUE(
        hasError(ssg::KeymapMatcher{bad}.validate(), ssg::KeymapErrorCode::UnknownContext));

    for (const auto context : {"*", "editor", "panel", "prompt"}) {
        ssg::KeymapViewState good{"ok", {{seq, "cursor.line_down", context}}};
        ASSERT_FALSE(hasError(ssg::KeymapMatcher{good}.validate(),
                               ssg::KeymapErrorCode::UnknownContext));
    }
}

TEST(validateKeymapRejectsMultiStrokeBindings) {
    const auto single = *ssg::KeyCodec{}.parseSequence({"Alt+KeyF"});
    const auto multi = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF"});

    // A single-stroke binding is fine; any longer sequence is rejected -- the
    // multi-stroke chord model is gone.
    ssg::KeymapViewState ok{"m", {{single, "a", "*"}}};
    ASSERT_FALSE(hasError(ssg::KeymapMatcher{ok}.validate(),
                          ssg::KeymapErrorCode::MultiStrokeBinding));
    ssg::KeymapViewState twoStroke{"m", {{multi, "b", "*"}}};
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{twoStroke}.validate(),
                         ssg::KeymapErrorCode::MultiStrokeBinding));
}

TEST(resolveKeySequenceMapsSameKeyPerContext) {
    const auto down = *ssg::KeyCodec{}.parseSequence({"ArrowDown"});
    ssg::KeymapViewState keymap{
        "default",
        {{down, "cursor.line_down", "editor"},
         {down, "tree.select_next", "panel"}}};

    const auto inEditor = ssg::KeymapMatcher{keymap}.resolveSequence(down, "editor");
    ASSERT_EQ(inEditor.kind, ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(inEditor.commandId, std::string{"cursor.line_down"});

    const auto inPanel = ssg::KeymapMatcher{keymap}.resolveSequence(down, "panel");
    ASSERT_EQ(inPanel.kind, ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(inPanel.commandId, std::string{"tree.select_next"});

    // No eligible binding in prompt context.
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "prompt").kind,
              ssg::KeymapMatchKind::None);
}

TEST(resolveKeySequenceStarBeatsFocusAndResolvesEverywhere) {
    const auto save = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    // A "*" binding and a same-sequence focus binding; "*" must win regardless of
    // which is listed first, and resolve in every context.
    ssg::KeymapViewState focusFirst{
        "m", {{save, "focus.only", "editor"}, {save, "file.save", "*"}}};
    ssg::KeymapViewState starFirst{
        "m", {{save, "file.save", "*"}, {save, "focus.only", "editor"}}};
    for (const auto* keymap : {&focusFirst, &starFirst}) {
        for (const auto context : {"editor", "panel", "prompt"}) {
            const auto r = ssg::KeymapMatcher{*keymap}.resolveSequence(save, context);
            ASSERT_EQ(r.kind, ssg::KeymapMatchKind::Resolved);
            ASSERT_EQ(r.commandId, std::string{"file.save"});
        }
    }
}

TEST(resolveKeySequenceResolvesOrReportsNone) {
    const auto save = *ssg::KeyCodec{}.parseSequence({"Alt+KeyS"});
    const auto other = *ssg::KeyCodec{}.parseSequence({"Alt+KeyX"});
    ssg::KeymapViewState keymap{"m", {{save, "file.save", "*"}}};

    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(save, "editor").kind,
              ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(other, "editor").kind,
              ssg::KeymapMatchKind::None);
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence({}, "editor").kind,
              ssg::KeymapMatchKind::None);
}

TEST(textRoutingIsPerContext) {
    ASSERT_EQ(ssg::SemanticInputRouter{}.textRouting("editor"), ssg::TextRouting::Insert);
    ASSERT_EQ(ssg::SemanticInputRouter{}.textRouting("prompt"), ssg::TextRouting::PromptQuery);
    ASSERT_EQ(ssg::SemanticInputRouter{}.textRouting("panel"), ssg::TextRouting::Ignore);
    ASSERT_EQ(ssg::SemanticInputRouter{}.textRouting("*"), ssg::TextRouting::Ignore);
}

TEST(hasGlobalBindingRequiresUnshadowedStar) {
    const auto seq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});

    ssg::KeymapViewState present{"m", {{seq, "settings.open", "*"}}};
    ASSERT_TRUE(ssg::KeymapMatcher{present}.hasGlobalBinding("settings.open"));

    // Focus-context (not global) does not count.
    ssg::KeymapViewState contextual{"m", {{seq, "settings.open", "editor"}}};
    ASSERT_FALSE(ssg::KeymapMatcher{contextual}.hasGlobalBinding("settings.open"));

    // Shadowed by an earlier "*" binding of the same sequence does not count.
    ssg::KeymapViewState shadowed{
        "m", {{seq, "other.command", "*"}, {seq, "settings.open", "*"}}};
    ASSERT_FALSE(ssg::KeymapMatcher{shadowed}.hasGlobalBinding("settings.open"));

    // Absent command.
    ASSERT_FALSE(ssg::KeymapMatcher{present}.hasGlobalBinding("file.save"));
}

TEST(validateKeymapFlagsGlobalShadowRegardlessOfOrder) {
    const auto seq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    // Global-then-focus and focus-then-global must both flag the focus binding.
    ssg::KeymapViewState globalFirst{
        "m", {{seq, "file.save", "*"}, {seq, "focus.only", "editor"}}};
    ssg::KeymapViewState focusFirst{
        "m", {{seq, "focus.only", "editor"}, {seq, "file.save", "*"}}};
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{globalFirst}.validate(),
                          ssg::KeymapErrorCode::UnreachableBinding));
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{focusFirst}.validate(),
                          ssg::KeymapErrorCode::UnreachableBinding));
}

TEST(resolverAndHasGlobalBindingAgreeOnDuplicateGlobals) {
    const auto seq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});
    // An invalid map with two "*" bindings for one sequence: the resolver's
    // winner must be the same command has_global_binding calls authoritative.
    for (const auto& first : {std::string{"settings.open"}, std::string{"other.cmd"}}) {
        const std::string second =
            first == "settings.open" ? "other.cmd" : "settings.open";
        ssg::KeymapViewState keymap{
            "m", {{seq, first, "*"}, {seq, second, "*"}}};
        const auto resolved = ssg::KeymapMatcher{keymap}.resolveSequence(seq, "editor");
        ASSERT_EQ(resolved.kind, ssg::KeymapMatchKind::Resolved);
        // First "*" binding wins in both functions.
        ASSERT_EQ(resolved.commandId, first);
        ASSERT_EQ(ssg::KeymapMatcher{keymap}.hasGlobalBinding(first), true);
        ASSERT_EQ(ssg::KeymapMatcher{keymap}.hasGlobalBinding(second), false);
    }
}

TEST(imeAcceptsOnlyCommittedUtf8Text) {
    const auto committed =
        ssg::CommittedText::fromUtf8("e\xCC\x81 \xF0\x9F\x98\x80");
    ASSERT_TRUE(committed.has_value());
    if (committed) {
        const auto semantic = ssg::SemanticInputRouter{}.semanticInput(*committed);
        ASSERT_EQ(semantic.commandId, std::string{"text.insert"});
        ASSERT_EQ(std::get<ssg::TextInputArguments>(semantic.arguments).text,
                  committed->utf8());
    }
    ASSERT_FALSE(ssg::CommittedText::fromUtf8(std::string{"\xC0\xAF", 2})
                     .has_value());
    ASSERT_FALSE(ssg::CommittedText::fromUtf8(std::string{"a\0b", 3})
                     .has_value());
    ASSERT_FALSE(ssg::CommittedText::fromUtf8("").has_value());
}

TEST(hitTargetsRoundTripTypedSemanticArguments) {
    ssg::SemanticCommand command{
        "cursor.set_position",
        ssg::SelectionCommandArguments{ssg::DocumentPosition{
                                           ssg::ByteOffset{7},
                                           ssg::LineIndex{2},
                                           ssg::CellIndex{4}},
                                       std::nullopt}};
    const ssg::SemanticHitTarget target{
        42, ssg::HitTargetKind::EditorCell, "document cell", command};
    ASSERT_EQ(ssg::SemanticInputRouter{}.activateHitTarget(target), command);

    const auto& arguments =
        std::get<ssg::SelectionCommandArguments>(target.command.arguments);
    ASSERT_TRUE(arguments.position.has_value());
    if (arguments.position) {
        ASSERT_EQ(arguments.position->byteOffset, ssg::ByteOffset{7});
        ASSERT_EQ(arguments.position->line, ssg::LineIndex{2});
        ASSERT_EQ(arguments.position->cell, ssg::CellIndex{4});
    }

    const ssg::SemanticCommand scroll{
        "view.scroll_to_fraction", ssg::ScrollFractionArguments{3, 7}};
    const ssg::SemanticHitTarget scrollbar{
        43, ssg::HitTargetKind::Scrollbar, "scrollbar", scroll};
    ASSERT_EQ(ssg::SemanticInputRouter{}.activateHitTarget(scrollbar), scroll);
}

TEST(applyKeymapBindAddsRebindsAndRejectsInvalidRequests) {
    const auto settingsSeq = *ssg::KeyCodec{}.parseSequence({"Alt+KeyS"});
    ssg::KeymapViewState base{"m", {{settingsSeq, "settings.open", "*"}}};

    // Fresh bind: adds a new global binding.
    {
        auto result = ssg::applyKeymapBind(
            base, {"Alt+KeyG", "find.open", ""});
        ASSERT_TRUE(result.accepted());
        const auto boundSeq = *ssg::KeyCodec{}.parseSequence({"Alt+KeyG"});
        ssg::KeymapViewState expected{
            "m", {{settingsSeq, "settings.open", "*"},
                  {boundSeq, "find.open", "*"}}};
        ASSERT_EQ(result.keymap, expected);
    }

    // Rebind: same (context, sequence) replaces rather than duplicates.
    {
        auto once = ssg::applyKeymapBind(
            base, {"Alt+KeyG", "find.open", "editor"});
        ASSERT_TRUE(once.accepted());
        auto twice = ssg::applyKeymapBind(
            once.keymap, {"Alt+KeyG", "find.replace", "editor"});
        ASSERT_TRUE(twice.accepted());
        const auto boundSeq = *ssg::KeyCodec{}.parseSequence({"Alt+KeyG"});
        ssg::KeymapViewState expected{
            "m", {{settingsSeq, "settings.open", "*"},
                  {boundSeq, "find.replace", "editor"}}};
        ASSERT_EQ(twice.keymap, expected);
    }

    // Rejections leave the input keymap conceptually untouched (caller
    // never applies .keymap on a rejected result).
    ASSERT_FALSE(ssg::applyKeymapBind(base, {"NotAKey", "find.open", ""})
                     .accepted());
    // A multi-stroke sequence no longer parses -- the chord model is gone.
    ASSERT_FALSE(ssg::applyKeymapBind(base, {"Alt+KeyF KeyT", "find.open", ""})
                     .accepted());
    ASSERT_FALSE(ssg::applyKeymapBind(base, {"Alt+KeyF", "", ""})
                     .accepted());
    ASSERT_FALSE(
        ssg::applyKeymapBind(base, {"Alt+KeyF", "find.open", "bogus"})
            .accepted());
    // Rebinding the sole settings.open global binding to something else
    // must reject: K6's escape hatch must survive.
    ASSERT_FALSE(
        ssg::applyKeymapBind(base, {"Alt+KeyS", "other.command", ""})
            .accepted());
}

TEST(applyKeymapUnbindRemovesOrNoOpsAndRejectsBadSequence) {
    const auto settingsSeq = *ssg::KeyCodec{}.parseSequence({"Alt+KeyS"});
    const auto findSeq = *ssg::KeyCodec{}.parseSequence({"Alt+KeyG"});
    ssg::KeymapViewState base{
        "m", {{settingsSeq, "settings.open", "*"},
              {findSeq, "find.open", "editor"}}};

    auto removed = ssg::applyKeymapUnbind(base, {"Alt+KeyG", "editor"});
    ASSERT_TRUE(removed.accepted());
    ssg::KeymapViewState expected{"m", {{settingsSeq, "settings.open", "*"}}};
    ASSERT_EQ(removed.keymap, expected);

    // Absent binding: no-op success, unchanged keymap.
    auto noOp = ssg::applyKeymapUnbind(base, {"Alt+KeyQ", ""});
    ASSERT_TRUE(noOp.accepted());
    ASSERT_EQ(noOp.keymap, base);

    ASSERT_FALSE(ssg::applyKeymapUnbind(base, {"NotAKey", ""}).accepted());
    ASSERT_FALSE(
        ssg::applyKeymapUnbind(base, {"Alt+KeyF", "bogus"}).accepted());
    // Removing the sole settings.open global binding must reject: K6's
    // escape hatch must survive unbind, same as bind.
    ASSERT_FALSE(
        ssg::applyKeymapUnbind(base, {"Alt+KeyS", ""}).accepted());
}

TEST(backendHasNoPlatformInputCaptureDependency) {
    const std::vector<std::string> forbidden{
        "KeyboardEvent", "keydown", "compositionstart", "compositionupdate",
        "navigator.clipboard", "addEventListener", "GetAsyncKeyState",
        "ReadConsoleInput"};
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator{SSG_SOURCE_PATH}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
            continue;
        }
        const auto source = readFile(entry.path());
        for (const auto& token : forbidden) {
            ASSERT_TRUE(source.find(token) == std::string::npos);
        }
    }
}

} // namespace

// The compiled keymap is a DERIVED index of the authored one, so the only thing
// that makes it safe is that it decides exactly what KeymapMatcher decides.  It
// duplicates the precedence and prefix rules, so this compares the two
// resolvers directly rather than asserting either one's answers: an oracle here
// would pin the rules twice and still not prove they agree.
//
// The keymaps below are deliberately adversarial -- duplicate globals, a global
// shadowing a focus binding, prefix chains, and an unknown context -- because
// those are the cases where a reimplementation diverges.
TEST(compiledKeymapResolvesIdenticallyToTheAuthoredMatcher) {
    const auto stroke = [](ssg::KeyCode code, bool control = false) {
        ssg::KeyStroke result;
        result.code = code;
        result.control = control;
        return result;
    };
    const auto binding = [](ssg::KeySequence sequence, std::string command,
                            std::string context) {
        return ssg::KeyBinding{std::move(sequence), std::move(command),
                               std::move(context)};
    };

    const auto escape = stroke(ssg::KeyCode::Escape);
    const auto keyS = stroke(ssg::KeyCode::KeyS);
    const auto keyQ = stroke(ssg::KeyCode::KeyQ);
    const auto ctrlS = stroke(ssg::KeyCode::KeyS, true);

    std::vector<ssg::KeymapViewState> keymaps;
    keymaps.push_back({"focus-vs-global",
                       {binding({escape, keyS}, "file.save", "editor"),
                        binding({escape, keyS}, "file.save_as", "*"),
                        binding({escape, keyQ}, "edit.undo", "prompt")}});
    keymaps.push_back({"duplicate-globals",
                       {binding({escape, keyS}, "file.save", "*"),
                        binding({escape, keyS}, "file.save_as", "*")}});
    keymaps.push_back({"prefix-chain",
                       {binding({escape}, "edit.undo", "editor"),
                        binding({escape, keyS}, "file.save", "editor"),
                        binding({escape, keyS, keyQ}, "edit.redo", "editor")}});
    keymaps.push_back({"unknown-context",
                       {binding({escape, keyS}, "file.save", "nonsense"),
                        binding({ctrlS}, "file.save_as", "panel")}});
    keymaps.push_back({"empty", {}});

    // Exact matches resolve; everything else is None.  Both resolvers must
    // agree on the same corpus, including former multi-stroke inputs.
    const std::vector<ssg::KeySequence> inputs{
        {}, {escape}, {keyS}, {ctrlS}, {escape, keyS}, {escape, keyQ},
        {escape, ctrlS}, {escape, keyS, keyQ}, {escape, keyS, keyS},
        {keyS, escape}};

    // An empty catalog is enough: the rule under test is which BINDING wins,
    // which does not depend on whether the command it names exists.
    const ssg::CommandCatalog catalog;
    for (const auto& keymap : keymaps) {
        const ssg::CompiledKeymap compiled{keymap, catalog};
        for (const auto focus : {ssg::FocusTarget::Editor,
                                 ssg::FocusTarget::Panel,
                                 ssg::FocusTarget::Prompt}) {
            for (const auto& input : inputs) {
                std::vector<ssg::CompiledStroke> compiledInput;
                for (const auto& key : input) {
                    compiledInput.push_back(ssg::CompiledKeymap::compile(key));
                }
                const auto authored = ssg::KeymapMatcher{keymap}.resolveSequence(
                    input, ssg::focusTargetName(focus));
                const auto fast = compiled.resolve(compiledInput, focus);
                ASSERT_TRUE(authored.kind == fast.kind);
                ASSERT_EQ(std::string{fast.command.name()},
                          std::string{authored.commandId});
            }
        }
    }
}

// keymap.bind accepts any non-empty command id, so a binding may name a command
// the catalog does not have -- a typo, or a command removed since the config was
// written.  Resolution must carry that NAME out, because it is the only thing a
// rejected dispatch can report; a bare handle would be invalid and nameless.
TEST(compiledKeymapCarriesTheNameOfAnUncataloguedCommand) {
    ssg::KeyStroke escape;
    escape.code = ssg::KeyCode::Escape;

    const ssg::KeymapViewState keymap{
        "typo", {ssg::KeyBinding{{escape}, "file.saev", "*"}}};
    const ssg::CommandCatalog catalog;
    const ssg::CompiledKeymap compiled{keymap, catalog};
    const std::vector<ssg::CompiledStroke> input{
        ssg::CompiledKeymap::compile(escape)};

    const auto resolution = compiled.resolve(input, ssg::FocusTarget::Editor);
    ASSERT_TRUE(resolution.kind == ssg::KeymapMatchKind::Resolved);
    ASSERT_FALSE(resolution.command.handle().valid());
    ASSERT_EQ(std::string{resolution.command.name()}, std::string{"file.saev"});
}

SSG_TEST_SUITE(test_input) {
    RUN(keyStrokesHaveACanonicalRoundTrip);
    RUN(validateKeymapFlagsDuplicateAndUnreachableBindings);
    RUN(keymapContextsAreStarPlusFocusNames);
    RUN(validateKeymapRejectsModifiedEnterBindings);
    RUN(validateKeymapRejectsUnknownContext);
    RUN(validateKeymapRejectsMultiStrokeBindings);
    RUN(resolveKeySequenceMapsSameKeyPerContext);
    RUN(resolveKeySequenceStarBeatsFocusAndResolvesEverywhere);
    RUN(resolveKeySequenceResolvesOrReportsNone);
    RUN(compiledKeymapResolvesIdenticallyToTheAuthoredMatcher);
    RUN(compiledKeymapCarriesTheNameOfAnUncataloguedCommand);
    RUN(textRoutingIsPerContext);
    RUN(hasGlobalBindingRequiresUnshadowedStar);
    RUN(validateKeymapFlagsGlobalShadowRegardlessOfOrder);
    RUN(resolverAndHasGlobalBindingAgreeOnDuplicateGlobals);
    RUN(imeAcceptsOnlyCommittedUtf8Text);
    RUN(hitTargetsRoundTripTypedSemanticArguments);
    RUN(applyKeymapBindAddsRebindsAndRejectsInvalidRequests);
    RUN(applyKeymapUnbindRemovesOrNoOpsAndRejectsBadSequence);
    RUN(backendHasNoPlatformInputCaptureDependency);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
