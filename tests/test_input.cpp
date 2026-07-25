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

TEST(validateKeymapFlagsDuplicateUnreachableAndReservedBindings) {
    const auto sequence = *ssg::KeyCodec{}.parseSequence(
        {"Ctrl+Shift+KeyM", "KeyA", "KeyA"});
    ssg::KeymapViewState duplicate{
        "bad", {{sequence, "cursor.left", "editor"},
                {sequence, "cursor.right", "editor"}}};
    const auto duplicateErrors = ssg::KeymapMatcher{duplicate}.validate({});
    ASSERT_TRUE(std::ranges::any_of(duplicateErrors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::DuplicateBinding;
    }));

    ssg::KeymapViewState unreachable{
        "bad", {{sequence, "cursor.left", "*"},
                {sequence, "cursor.right", "editor"}}};
    const auto unreachableErrors = ssg::KeymapMatcher{unreachable}.validate({});
    ASSERT_TRUE(std::ranges::any_of(unreachableErrors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::UnreachableBinding;
    }));

    const auto reserved = *ssg::KeyCodec{}.parseSequence({"Ctrl+KeyL"});
    ssg::KeymapViewState reservedMap{
        "bad", {{reserved, "cursor.left", "*"}}};
    const auto reservedErrors =
        ssg::KeymapMatcher{reservedMap}.validate({&reserved, 1});
    ASSERT_TRUE(std::ranges::any_of(reservedErrors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::ReservedBinding;
    }));
    const auto longer = *ssg::KeyCodec{}.parseSequence({"Ctrl+KeyL", "KeyA"});
    ssg::KeymapViewState reservedPrefixMap{
        "bad", {{longer, "cursor.left", "*"}}};
    const auto prefixErrors =
        ssg::KeymapMatcher{reservedPrefixMap}.validate({&reserved, 1});
    ASSERT_TRUE(std::ranges::any_of(prefixErrors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::ReservedBinding;
    }));
}

TEST(keymapContextsAreStarPlusFocusNames) {
    const auto contexts = ssg::keymapContexts();
    std::set<std::string_view> actual{contexts.begin(), contexts.end()};
    const std::set<std::string_view> expected{"*", "editor", "panel", "prompt"};
    ASSERT_TRUE(actual == expected);
    ASSERT_EQ(ssg::focusTargetName(ssg::FocusTarget::Editor),
              std::string_view{"editor"});
    ASSERT_EQ(ssg::focusTargetName(ssg::FocusTarget::Panel),
              std::string_view{"panel"});
    ASSERT_EQ(ssg::focusTargetName(ssg::FocusTarget::Prompt),
              std::string_view{"prompt"});
}

namespace {

bool hasError(const std::vector<ssg::KeymapError>& errors,
               ssg::KeymapErrorCode code) {
    return std::ranges::any_of(
        errors, [&](const auto& error) { return error.code == code; });
}

}  // namespace

TEST(validateKeymapRejectsUnknownContext) {
    const auto seq = *ssg::KeyCodec{}.parseSequence({"ArrowDown"});
    ssg::KeymapViewState bad{"bad", {{seq, "cursor.line_down", "sidebar"}}};
    ASSERT_TRUE(
        hasError(ssg::KeymapMatcher{bad}.validate({}), ssg::KeymapErrorCode::UnknownContext));

    for (const auto context : {"*", "editor", "panel", "prompt"}) {
        ssg::KeymapViewState good{"ok", {{seq, "cursor.line_down", context}}};
        ASSERT_FALSE(hasError(ssg::KeymapMatcher{good}.validate({}),
                               ssg::KeymapErrorCode::UnknownContext));
    }
}

TEST(validateKeymapRejectsAmbiguousPrefixOrderIndependently) {
    const auto escF = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF"});
    const auto escFT = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});

    // Same context (both "*"): a strict prefix pair is ambiguous, in either order.
    ssg::KeymapViewState forward{
        "m", {{escF, "a", "*"}, {escFT, "b", "*"}}};
    ssg::KeymapViewState reversed{
        "m", {{escFT, "b", "*"}, {escF, "a", "*"}}};
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{forward}.validate({}),
                          ssg::KeymapErrorCode::AmbiguousPrefix));
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{reversed}.validate({}),
                          ssg::KeymapErrorCode::AmbiguousPrefix));

    // "*"/focus overlap: a global prefix and a focus continuation collide.
    ssg::KeymapViewState starFocus{
        "m", {{escF, "a", "*"}, {escFT, "b", "editor"}}};
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{starFocus}.validate({}),
                          ssg::KeymapErrorCode::AmbiguousPrefix));

    // focus/focus in the SAME context collide.
    ssg::KeymapViewState focusFocus{
        "m", {{escF, "a", "editor"}, {escFT, "b", "editor"}}};
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{focusFocus}.validate({}),
                          ssg::KeymapErrorCode::AmbiguousPrefix));

    // DIFFERENT focus contexts do not overlap, so a prefix pair is allowed.
    ssg::KeymapViewState disjoint{
        "m", {{escF, "a", "editor"}, {escFT, "b", "panel"}}};
    ASSERT_FALSE(hasError(ssg::KeymapMatcher{disjoint}.validate({}),
                           ssg::KeymapErrorCode::AmbiguousPrefix));
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

TEST(resolveKeySequenceReportsPendingAndNone) {
    const auto esc = *ssg::KeyCodec{}.parseSequence({"Escape"});
    const auto escS = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    const auto escX = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyX"});
    ssg::KeymapViewState keymap{"m", {{escS, "file.save", "*"}}};

    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(esc, "editor").kind,
              ssg::KeymapMatchKind::Pending);
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(escS, "editor").kind,
              ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(escX, "editor").kind,
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

TEST(hasGlobalBindingRequiresUnreservedUnshadowedStar) {
    const auto seq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});

    ssg::KeymapViewState present{"m", {{seq, "settings.open", "*"}}};
    ASSERT_TRUE(ssg::KeymapMatcher{present}.hasGlobalBinding("settings.open", {}));

    // Focus-context (not global) does not count.
    ssg::KeymapViewState contextual{"m", {{seq, "settings.open", "editor"}}};
    ASSERT_FALSE(ssg::KeymapMatcher{contextual}.hasGlobalBinding("settings.open", {}));

    // Reserved sequence does not count.
    ASSERT_FALSE(ssg::KeymapMatcher{present}.hasGlobalBinding("settings.open", {&seq, 1}));

    // Shadowed by an earlier "*" binding of the same sequence does not count.
    ssg::KeymapViewState shadowed{
        "m", {{seq, "other.command", "*"}, {seq, "settings.open", "*"}}};
    ASSERT_FALSE(ssg::KeymapMatcher{shadowed}.hasGlobalBinding("settings.open", {}));

    // Absent command.
    ASSERT_FALSE(ssg::KeymapMatcher{present}.hasGlobalBinding("file.save", {}));
}

TEST(validateKeymapFlagsGlobalShadowRegardlessOfOrder) {
    const auto seq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    // Global-then-focus and focus-then-global must both flag the focus binding.
    ssg::KeymapViewState globalFirst{
        "m", {{seq, "file.save", "*"}, {seq, "focus.only", "editor"}}};
    ssg::KeymapViewState focusFirst{
        "m", {{seq, "focus.only", "editor"}, {seq, "file.save", "*"}}};
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{globalFirst}.validate({}),
                          ssg::KeymapErrorCode::UnreachableBinding));
    ASSERT_TRUE(hasError(ssg::KeymapMatcher{focusFirst}.validate({}),
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
        ASSERT_EQ(ssg::KeymapMatcher{keymap}.hasGlobalBinding(first, {}), true);
        ASSERT_EQ(ssg::KeymapMatcher{keymap}.hasGlobalBinding(second, {}), false);
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
    const auto settingsSeq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    ssg::KeymapViewState base{"m", {{settingsSeq, "settings.open", "*"}}};

    // Fresh bind: adds a new global binding.
    {
        auto result = ssg::applyKeymapBind(
            base, {"Escape KeyF KeyT", "find.open", ""});
        ASSERT_TRUE(result.accepted());
        const auto boundSeq =
            *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});
        ssg::KeymapViewState expected{
            "m", {{settingsSeq, "settings.open", "*"},
                  {boundSeq, "find.open", "*"}}};
        ASSERT_EQ(result.keymap, expected);
    }

    // Rebind: same (context, sequence) replaces rather than duplicates.
    {
        auto once = ssg::applyKeymapBind(
            base, {"Escape KeyF KeyT", "find.open", "editor"});
        ASSERT_TRUE(once.accepted());
        auto twice = ssg::applyKeymapBind(
            once.keymap, {"Escape KeyF KeyT", "find.replace", "editor"});
        ASSERT_TRUE(twice.accepted());
        const auto boundSeq =
            *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});
        ssg::KeymapViewState expected{
            "m", {{settingsSeq, "settings.open", "*"},
                  {boundSeq, "find.replace", "editor"}}};
        ASSERT_EQ(twice.keymap, expected);
    }

    // Rejections leave the input keymap conceptually untouched (caller
    // never applies .keymap on a rejected result).
    ASSERT_FALSE(ssg::applyKeymapBind(base, {"NotAKey", "find.open", ""})
                     .accepted());
    ASSERT_FALSE(ssg::applyKeymapBind(base, {"Escape KeyF", "", ""})
                     .accepted());
    ASSERT_FALSE(
        ssg::applyKeymapBind(base, {"Escape KeyF", "find.open", "bogus"})
            .accepted());
    // Rebinding the sole settings.open global binding to something else
    // must reject: K6's escape hatch must survive.
    ASSERT_FALSE(
        ssg::applyKeymapBind(base, {"Escape KeyS", "other.command", ""})
            .accepted());
}

TEST(applyKeymapUnbindRemovesOrNoOpsAndRejectsBadSequence) {
    const auto settingsSeq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    const auto findSeq =
        *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});
    ssg::KeymapViewState base{
        "m", {{settingsSeq, "settings.open", "*"},
              {findSeq, "find.open", "editor"}}};

    auto removed = ssg::applyKeymapUnbind(base, {"Escape KeyF KeyT", "editor"});
    ASSERT_TRUE(removed.accepted());
    ssg::KeymapViewState expected{"m", {{settingsSeq, "settings.open", "*"}}};
    ASSERT_EQ(removed.keymap, expected);

    // Absent binding: no-op success, unchanged keymap.
    auto noOp = ssg::applyKeymapUnbind(base, {"Escape KeyQ", ""});
    ASSERT_TRUE(noOp.accepted());
    ASSERT_EQ(noOp.keymap, base);

    ASSERT_FALSE(ssg::applyKeymapUnbind(base, {"NotAKey", ""}).accepted());
    ASSERT_FALSE(
        ssg::applyKeymapUnbind(base, {"Escape KeyF", "bogus"}).accepted());
    // Removing the sole settings.open global binding must reject: K6's
    // escape hatch must survive unbind, same as bind.
    ASSERT_FALSE(
        ssg::applyKeymapUnbind(base, {"Escape KeyS", ""}).accepted());
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

int main() {
    RUN(keyStrokesHaveACanonicalRoundTrip);
    RUN(validateKeymapFlagsDuplicateUnreachableAndReservedBindings);
    RUN(keymapContextsAreStarPlusFocusNames);
    RUN(validateKeymapRejectsUnknownContext);
    RUN(validateKeymapRejectsAmbiguousPrefixOrderIndependently);
    RUN(resolveKeySequenceMapsSameKeyPerContext);
    RUN(resolveKeySequenceStarBeatsFocusAndResolvesEverywhere);
    RUN(resolveKeySequenceReportsPendingAndNone);
    RUN(textRoutingIsPerContext);
    RUN(hasGlobalBindingRequiresUnreservedUnshadowedStar);
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
