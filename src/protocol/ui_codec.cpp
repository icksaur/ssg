#include "codec_detail.h"

namespace ssg::protocol_detail {

ProtocolValue toValue(NoticeViewSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<NoticeViewSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<NoticeView> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(NoticeViewSectionDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(NoticeAction const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("command", toValue(value.command));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<NoticeAction>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    auto command = requireField<std::string>(value.field("command"));
    if (!id || !label || !command) return false;
    if (id->empty() || label->empty() || command->empty()) return false;
    out.emplace(NoticeAction{*id, *label, *command});
    return true;
}

ProtocolValue toValue(NoticeView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("text", toValue(value.text));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<NoticeView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto text = requireField<std::string>(value.field("text"));
    auto actions = requireField<std::vector<NoticeAction>>(value.field("actions"));
    if (!text || !actions) return false;
    if (text->empty() || actions->empty()) return false;
    out.emplace(NoticeView{*text, std::move(*actions)});
    return true;
}

ProtocolValue toValue(SrgbColor const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("red", toValue(value.red));
    fields.emplace_back("green", toValue(value.green));
    fields.emplace_back("blue", toValue(value.blue));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SrgbColor>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto red = requireField<std::uint8_t>(value.field("red"));
    auto green = requireField<std::uint8_t>(value.field("green"));
    auto blue = requireField<std::uint8_t>(value.field("blue"));
    if (!red || !green || !blue) return false;
    out.emplace(
        SrgbColor::fromSerializedChannels(*red, *green, *blue));
    return true;
}

ProtocolValue toValue(ThemeSnapshot const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("role_colors", toValue(value.roleColors));
    fields.emplace_back("syntax_colors", toValue(value.syntaxColors));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSnapshot>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto roleColors = requireField<std::array<SrgbColor, kSemanticRoleCount>>(
        value.field("role_colors"));
    auto syntaxColors = requireField<std::array<SrgbColor, kSyntaxScopeCount>>(
        value.field("syntax_colors"));
    if (!roleColors || !syntaxColors) {
        return false;
    }
    out.emplace(ThemeSnapshot{*roleColors, *syntaxColors});
    return true;
}

// Style's glyph tables are Style-internal structs, so their fields are encoded
// flat here rather than through per-struct codecs nothing else would use.
ProtocolValue toValue(Style const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scrollbar_gutter", toValue(value.scrollbar.gutter));
    fields.emplace_back("scrollbar_track", toValue(value.scrollbar.track));
    fields.emplace_back("scrollbar_single", toValue(value.scrollbar.single));
    fields.emplace_back("scrollbar_top", toValue(value.scrollbar.top));
    fields.emplace_back("scrollbar_body", toValue(value.scrollbar.body));
    fields.emplace_back("scrollbar_bottom", toValue(value.scrollbar.bottom));
    fields.emplace_back("tree_expanded", toValue(value.tree.expanded));
    fields.emplace_back("tree_collapsed", toValue(value.tree.collapsed));
    fields.emplace_back("tree_indent", toValue(value.tree.indentPerDepth));
    fields.emplace_back("tab_dirty_suffix", toValue(value.tab.dirtySuffix));
    fields.emplace_back("tab_live_diff_prefix", toValue(value.tab.liveDiffPrefix));
    fields.emplace_back("tab_read_only_suffix", toValue(value.tab.readOnlySuffix));
    fields.emplace_back("tab_left_edge", toValue(value.tab.leftEdge));
    fields.emplace_back("tab_right_edge", toValue(value.tab.rightEdge));
    fields.emplace_back("tab_separator", toValue(value.tab.separator));
    fields.emplace_back("toggle_checked", toValue(value.toggle.checked));
    fields.emplace_back("toggle_unchecked", toValue(value.toggle.unchecked));
    fields.emplace_back("truncation", toValue(value.truncation));
    fields.emplace_back("input_line_sigil", toValue(value.inputLineSigil));
    fields.emplace_back("unrenderable", toValue(value.unrenderable));
    fields.emplace_back("prompt_label_separator",
                        toValue(value.promptLabelSeparator));
    fields.emplace_back("cwd_prefix", toValue(value.cwdPrefix));
    fields.emplace_back("dim_minimum_columns",
                        toValue(value.dimensions.minimumColumns));
    fields.emplace_back("dim_minimum_rows",
                        toValue(value.dimensions.minimumRows));
    fields.emplace_back("dim_panel_target_width",
                        toValue(value.dimensions.panelTargetWidth));
    fields.emplace_back("dim_panel_minimum_width",
                        toValue(value.dimensions.panelMinimumWidth));
    fields.emplace_back("dim_editor_minimum_width",
                        toValue(value.dimensions.editorMinimumWidth));
    fields.emplace_back("dim_scrollbar_gutter_width",
                        toValue(value.dimensions.scrollbarGutterWidth));
    fields.emplace_back("dim_header_height",
                        toValue(value.dimensions.headerHeight));
    fields.emplace_back("dim_tab_bar_height",
                        toValue(value.dimensions.tabBarHeight));
    fields.emplace_back("dim_footer_height",
                        toValue(value.dimensions.footerHeight));
    fields.emplace_back("dim_label_padding",
                        toValue(value.dimensions.labelPadding));
    fields.emplace_back("dim_input_line_separator",
                        toValue(value.dimensions.inputLineSeparator));
    fields.emplace_back("dim_input_line_query_budget",
                        toValue(value.dimensions.inputLineQueryBudget));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<Style>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto gutter = requireField<std::string>(value.field("scrollbar_gutter"));
    auto track = requireField<std::string>(value.field("scrollbar_track"));
    auto single = requireField<std::string>(value.field("scrollbar_single"));
    auto top = requireField<std::string>(value.field("scrollbar_top"));
    auto body = requireField<std::string>(value.field("scrollbar_body"));
    auto bottom = requireField<std::string>(value.field("scrollbar_bottom"));
    auto expanded = requireField<std::string>(value.field("tree_expanded"));
    auto collapsed = requireField<std::string>(value.field("tree_collapsed"));
    auto indent = requireField<int>(value.field("tree_indent"));
    auto dirtySuffix = requireField<std::string>(value.field("tab_dirty_suffix"));
    auto liveDiffPrefix =
        requireField<std::string>(value.field("tab_live_diff_prefix"));
    auto readOnlySuffix =
        requireField<std::string>(value.field("tab_read_only_suffix"));
    auto tabLeftEdge = requireField<std::string>(value.field("tab_left_edge"));
    auto tabRightEdge = requireField<std::string>(value.field("tab_right_edge"));
    auto tabSeparator = requireField<std::string>(value.field("tab_separator"));
    auto checked = requireField<std::string>(value.field("toggle_checked"));
    auto unchecked = requireField<std::string>(value.field("toggle_unchecked"));
    auto truncation = requireField<std::string>(value.field("truncation"));
    auto sigil = requireField<std::string>(value.field("input_line_sigil"));
    auto unrenderable = requireField<std::string>(value.field("unrenderable"));
    auto separator =
        requireField<std::string>(value.field("prompt_label_separator"));
    auto cwdPrefix = requireField<std::string>(value.field("cwd_prefix"));
    auto minCols = requireField<int>(value.field("dim_minimum_columns"));
    auto minRows = requireField<int>(value.field("dim_minimum_rows"));
    auto panelTarget = requireField<int>(value.field("dim_panel_target_width"));
    auto panelMin = requireField<int>(value.field("dim_panel_minimum_width"));
    auto editorMin = requireField<int>(value.field("dim_editor_minimum_width"));
    auto gutterWidth =
        requireField<int>(value.field("dim_scrollbar_gutter_width"));
    auto headerHeight = requireField<int>(value.field("dim_header_height"));
    auto tabBarHeight = requireField<int>(value.field("dim_tab_bar_height"));
    auto footerHeight = requireField<int>(value.field("dim_footer_height"));
    auto labelPadding = requireField<int>(value.field("dim_label_padding"));
    auto separatorWidth =
        requireField<int>(value.field("dim_input_line_separator"));
    auto queryBudget =
        requireField<int>(value.field("dim_input_line_query_budget"));
    if (!gutter || !track || !single || !top || !body || !bottom || !expanded ||
        !collapsed || !indent || !dirtySuffix || !liveDiffPrefix ||
        !readOnlySuffix || !checked ||
        !tabLeftEdge || !tabRightEdge || !tabSeparator ||
        !unchecked || !truncation || !sigil || !unrenderable || !separator ||
        !cwdPrefix ||
        !minCols || !minRows || !panelTarget || !panelMin || !editorMin ||
        !gutterWidth || !headerHeight || !tabBarHeight || !footerHeight ||
        !labelPadding || !separatorWidth || !queryBudget) {
        return false;
    }
    Style style;
    style.scrollbar = {*gutter, *track, *single, *top, *body, *bottom};
    style.tree = {*expanded, *collapsed, *indent};
    style.tab = {*dirtySuffix, *liveDiffPrefix, *readOnlySuffix,
                 *tabLeftEdge, *tabRightEdge, *tabSeparator};
    style.toggle = {*checked, *unchecked};
    style.truncation = *truncation;
    style.inputLineSigil = *sigil;
    style.unrenderable = *unrenderable;
    style.promptLabelSeparator = *separator;
    style.cwdPrefix = *cwdPrefix;
    style.dimensions = {*minCols,      *minRows,      *panelTarget,
                        *panelMin,     *editorMin,    *gutterWidth,
                        *headerHeight, *tabBarHeight, *footerHeight,
                        *labelPadding, *separatorWidth, *queryBudget};
    out.emplace(std::move(style));
    return true;
}


ProtocolValue encodeUiFrameVersion(UiFrameVersion version) {
    return ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(version.generation.value())},
         {"presence_basis",
          ProtocolValue::makeUint(version.presenceBasis.value())}});
}

std::optional<UiFrameVersion> decodeUiFrameVersion(
    const ProtocolValue& value) {
    if (!detail::generated::validateUiFrameVersionWire(value)) {
        return std::nullopt;
    }
    const auto* generation = value.field("generation");
    const auto* presenceBasis = value.field("presence_basis");
    if (!value.asObject() || !generation || !generation->asUint() ||
        !presenceBasis || !presenceBasis->asUint()) {
        return std::nullopt;
    }
    return UiFrameVersion{Generation{*generation->asUint()},
                          PresenceBasis{*presenceBasis->asUint()}};
}

ProtocolValue encodeUiFrame(const UiFrame& frame) {
    return ProtocolValue::makeObject(
        {{"version", encodeUiFrameVersion(frame.version())},
         {"schema", encodeUiSchema(frame.schema())},
         {"state", encodeUiState(frame.state())},
         {"presence", encodeUiPresence(frame.presence())}});
}

std::optional<UiFrame> decodeUiFrame(const ProtocolValue& value) {
    if (!detail::generated::validateUiFrameWire(value)) return std::nullopt;
    const auto version =
        value.field("version")
            ? decodeUiFrameVersion(*value.field("version"))
            : std::nullopt;
    const auto schema =
        value.field("schema") ? decodeUiSchema(*value.field("schema"))
                              : std::nullopt;
    const auto state =
        value.field("state") ? decodeUiState(*value.field("state"))
                             : std::nullopt;
    const auto presence =
        value.field("presence")
            ? decodeUiPresence(*value.field("presence"))
            : std::nullopt;
    if (!version || !schema || !state || !presence) return std::nullopt;
    auto frame = UiFrame::create(*schema, *state, *presence);
    if (!frame || frame->version() != *version) return std::nullopt;
    return frame;
}

ProtocolValue encodeUiFrameDelta(const UiFrameDelta& delta) {
    std::vector<ProtocolValue::Field> fields{
        {"base", encodeUiFrameVersion(delta.base())},
        {"target", encodeUiFrameVersion(delta.target())}};
    if (const auto* replacement =
            std::get_if<UiFrameReplacement>(&delta.body())) {
        fields.emplace_back("kind", ProtocolValue::makeText("replacement"));
        fields.emplace_back("frame", encodeUiFrame(replacement->frame));
        return ProtocolValue::makeObject(std::move(fields));
    }
    const auto* changes = std::get_if<UiFrameChanges>(&delta.body());
    if (!changes) {
        throw std::invalid_argument(
            "legacy UI frame changes cannot be encoded");
    }
    UiStateSection stateChanges{
        delta.target().generation, changes->state,
        changes->focusPathChanged ? changes->focusPath : std::nullopt};
    UiPresenceSection presenceChanges{delta.target().generation,
                                      delta.target().presenceBasis,
                                      changes->presence};
    fields.emplace_back("kind", ProtocolValue::makeText("changes"));
    fields.emplace_back("state", encodeUiState(stateChanges));
    fields.emplace_back("presence", encodeUiPresence(presenceChanges));
    return ProtocolValue::makeObject(std::move(fields));
}

std::optional<UiFrameDelta> decodeUiFrameDelta(const ProtocolValue& value) {
    if (!detail::generated::validateUiFrameDeltaWire(value)) {
        return std::nullopt;
    }
    const auto base =
        value.field("base") ? decodeUiFrameVersion(*value.field("base"))
                            : std::nullopt;
    const auto target =
        value.field("target") ? decodeUiFrameVersion(*value.field("target"))
                              : std::nullopt;
    const auto* kindField = value.field("kind");
    if (!base || !target || !kindField || !kindField->asText()) {
        return std::nullopt;
    }
    if (*kindField->asText() == "replacement") {
        const auto frame =
            value.field("frame") ? decodeUiFrame(*value.field("frame"))
                                 : std::nullopt;
        if (!frame || frame->version() != *target) return std::nullopt;
        return UiFrameDelta::replacement(*base, *frame);
    }
    if (*kindField->asText() != "changes") return std::nullopt;
    const auto state =
        value.field("state") ? decodeUiState(*value.field("state"))
                             : std::nullopt;
    const auto presence =
        value.field("presence")
            ? decodeUiPresence(*value.field("presence"))
            : std::nullopt;
    if (!state || !presence || state->generation != target->generation ||
        presence->generation != target->generation ||
        presence->basis != target->presenceBasis) {
        return std::nullopt;
    }
    UiFrameChanges changes;
    changes.state = state->nodes;
    changes.presence = presence->nodes;
    changes.focusPathChanged = state->focusPath.has_value();
    changes.focusPath = state->focusPath;
    return UiFrameDelta::changes(*base, *target, std::move(changes));
}

ProtocolValue toValue(ThemeSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    std::optional<ThemeSnapshot> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ThemeSectionDelta{std::move(replacement)});
    return true;
}

}  // namespace ssg::protocol_detail
