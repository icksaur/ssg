#include "codec_detail.h"

namespace ssg::protocol_detail {

//
// Every composite decodePresent() below starts by rejecting a non-object
// wire value outright: value.field() already returns nullptr for every key
// when the value is not an object, which require_field() and
// decode_optional_field() both turn into "field absent" -- but a struct
// whose fields are *all* domain-optional (e.g. SessionTopology) would then
// wrongly decode a malformed non-object value (an array, a bare integer) as
// "every field absent" instead of rejecting it. The explicit as_object()
// check below closes that gap uniformly.

ProtocolValue toValue(DocumentPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("byte_offset", toValue(value.byteOffset));
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("cell", toValue(value.cell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DocumentPosition>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto byteOffset = requireField<ByteOffset>(value.field("byte_offset"));
    auto line = requireField<LineIndex>(value.field("line"));
    auto cell = requireField<CellIndex>(value.field("cell"));
    if (!byteOffset || !line || !cell) return false;
    out.emplace(DocumentPosition{*byteOffset, *line, *cell});
    return true;
}

ProtocolValue toValue(DocumentViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("text", toValue(value.text));
    fields.emplace_back("caret", toValue(value.caret));
    fields.emplace_back("diff_file_identity", toValue(value.diffFileIdentity));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DocumentViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto text = requireField<std::string>(value.field("text"));
    auto caret = requireField<ByteOffset>(value.field("caret"));
    std::optional<std::string> diffFileIdentity;
    if (!decodeOptionalField(value.field("diff_file_identity"),
                             diffFileIdentity)) {
        return false;
    }
    if (!revision || !text || !caret) return false;
    out.emplace(DocumentViewState{*revision, *text, *caret,
                                  std::move(diffFileIdentity)});
    return true;
}

ProtocolValue toValue(DocumentDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("start", toValue(value.start));
    fields.emplace_back("erased_bytes", toValue(value.erasedBytes));
    fields.emplace_back("inserted_text", toValue(value.insertedText));
    fields.emplace_back("diff_file_identity", toValue(value.diffFileIdentity));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DocumentDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto start = requireField<ByteOffset>(value.field("start"));
    auto erasedBytes = requireField<std::uint64_t>(value.field("erased_bytes"));
    auto insertedText = requireField<std::string>(value.field("inserted_text"));
    std::optional<std::string> diffFileIdentity;
    if (!decodeOptionalField(value.field("diff_file_identity"),
                             diffFileIdentity)) {
        return false;
    }
    if (!baseRevision || !revision || !start || !erasedBytes || !insertedText) {
        return false;
    }
    out.emplace(DocumentDelta{*baseRevision, *revision, *start, *erasedBytes,
                              *insertedText, std::move(diffFileIdentity)});
    return true;
}

ProtocolValue toValue(Selection const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("anchor", toValue(value.anchor));
    fields.emplace_back("active", toValue(value.active));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<Selection>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto anchor = requireField<DocumentPosition>(value.field("anchor"));
    auto active = requireField<DocumentPosition>(value.field("active"));
    if (!anchor || !active) return false;
    out.emplace(Selection{*anchor, *active});
    return true;
}

ProtocolValue toValue(SelectionSet const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("selections", toValue(value.items()));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSet>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto selections =
        requireField<std::vector<Selection>>(value.field("selections"));
    if (!selections || selections->empty()) return false;
    out.emplace(*selections);
    return true;
}

ProtocolValue toValue(SelectionViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("selections", toValue(value.selections));
    fields.emplace_back("first_visual_row", toValue(value.firstVisualRow));
    fields.emplace_back("first_visual_column", toValue(value.firstVisualColumn));
    fields.emplace_back("desired_cell", toValue(value.desiredCell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto selections = requireField<SelectionSet>(value.field("selections"));
    auto firstVisualRow =
        requireField<std::uint32_t>(value.field("first_visual_row"));
    auto firstVisualColumn =
        requireField<std::uint32_t>(value.field("first_visual_column"));
    if (!selections || !firstVisualRow || !firstVisualColumn) return false;
    std::optional<CellIndex> desiredCell;
    if (!decodeOptionalField(value.field("desired_cell"), desiredCell)) return false;
    out.emplace(SelectionViewState{*selections, *firstVisualRow,
                                   *firstVisualColumn, desiredCell});
    return true;
}

ProtocolValue toValue(SelectionViewDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionViewDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(SelectionNavigation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("first_visual_row", toValue(value.firstVisualRow));
    fields.emplace_back("first_visual_column", toValue(value.firstVisualColumn));
    fields.emplace_back("desired_cell", toValue(value.desiredCell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigation>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto firstVisualRow =
        requireField<std::uint32_t>(value.field("first_visual_row"));
    auto firstVisualColumn =
        requireField<std::uint32_t>(value.field("first_visual_column"));
    if (!firstVisualRow || !firstVisualColumn) return false;
    std::optional<CellIndex> desiredCell;
    if (!decodeOptionalField(value.field("desired_cell"), desiredCell)) return false;
    out.emplace(SelectionNavigation{*firstVisualRow, *firstVisualColumn,
                                    desiredCell});
    return true;
}

ProtocolValue toValue(SelectionNavigationDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigationDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionNavigation> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionNavigationDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(SelectionSetDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSetDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionSet> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionSetDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(HistoryViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("can_undo", toValue(value.canUndo));
    fields.emplace_back("can_redo", toValue(value.canRedo));
    fields.emplace_back("retained_bytes", toValue(value.retainedBytes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<HistoryViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto canUndo = requireField<bool>(value.field("can_undo"));
    auto canRedo = requireField<bool>(value.field("can_redo"));
    auto retainedBytes = requireField<std::uint64_t>(value.field("retained_bytes"));
    if (!canUndo || !canRedo || !retainedBytes) return false;
    out.emplace(HistoryViewState{*canUndo, *canRedo, *retainedBytes});
    return true;
}

ProtocolValue toValue(HistoryDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<HistoryDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<HistoryViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(HistoryDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(Rect const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("x", toValue(value.x));
    fields.emplace_back("y", toValue(value.y));
    fields.emplace_back("width", toValue(value.width));
    fields.emplace_back("height", toValue(value.height));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<Rect>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto x = requireField<int>(value.field("x"));
    auto y = requireField<int>(value.field("y"));
    auto width = requireField<int>(value.field("width"));
    auto height = requireField<int>(value.field("height"));
    if (!x || !y || !width || !height) return false;
    out.emplace(Rect{*x, *y, *width, *height});
    return true;
}

ProtocolValue toValue(GridSize const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("rows", toValue(value.rows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<GridSize>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto columns = requireField<int>(value.field("columns"));
    auto rows = requireField<int>(value.field("rows"));
    if (!columns || !rows) return false;
    out.emplace(GridSize{*columns, *rows});
    return true;
}

ProtocolValue toValue(PromptInput const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptInput>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto textValue = requireField<std::string>(value.field("value"));
    if (!id || !accessibleLabel || !textValue) return false;
    out.emplace(PromptInput{*id, *accessibleLabel, *textValue});
    return true;
}

ProtocolValue toValue(PromptToggle const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("width", toValue(value.width));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptToggle>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto toggleValue = requireField<bool>(value.field("value"));
    auto width = requireField<int>(value.field("width"));
    if (!id || !accessibleLabel || !toggleValue || !width) return false;
    out.emplace(PromptToggle{*id, *accessibleLabel, *toggleValue, *width});
    return true;
}

ProtocolValue toValue(PromptMatchCount const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptMatchCount>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto textValue = requireField<std::string>(value.field("value"));
    if (!id || !accessibleLabel || !textValue) return false;
    out.emplace(PromptMatchCount{*id, *accessibleLabel, *textValue});
    return true;
}

ProtocolValue toValue(PromptControl const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("checked", toValue(value.checked));
    fields.emplace_back("command", toValue(value.command));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptControl>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptControlKind>(value.field("kind"));
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto textValue = requireField<std::string>(value.field("value"));
    auto checked = requireField<bool>(value.field("checked"));
    auto command = requireField<std::string>(value.field("command"));
    if (!kind || !id || !accessibleLabel || !textValue || !checked || !command) {
        return false;
    }
    out.emplace(PromptControl{*kind, *id, *accessibleLabel, *textValue, *checked,
                              *command});
    return true;
}

ProtocolValue toValue(PromptView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("controls", toValue(value.controls));
    fields.emplace_back("active_input",
                        toValue(static_cast<std::uint64_t>(value.activeInput)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptKind>(value.field("kind"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto controls = requireField<std::vector<PromptControl>>(value.field("controls"));
    auto activeInput = requireField<std::uint64_t>(value.field("active_input"));
    if (!kind || !accessibleLabel || !controls || !activeInput) return false;
    std::size_t inputCount = 0;
    for (auto const& control : *controls) {
        if (control.kind == PromptControlKind::Input) ++inputCount;
    }
    if (inputCount == 0 ? *activeInput != 0 : *activeInput >= inputCount) {
        return false;
    }
    out.emplace(PromptView{*kind, *accessibleLabel, std::move(*controls),
                           static_cast<std::size_t>(*activeInput)});
    return true;
}


ProtocolValue toValue(StatusAction const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("command_id", toValue(value.commandId));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusAction>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto commandId = requireField<std::string>(value.field("command_id"));
    if (!id || !accessibleLabel || !commandId) return false;
    out.emplace(StatusAction{*id, *accessibleLabel, *commandId});
    return true;
}

ProtocolValue toValue(StatusItemView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("priority", toValue(value.priority));
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusItemView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<StatusId>(value.field("id"));
    auto priority = requireField<StatusPriority>(value.field("priority"));
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto actions = requireField<std::vector<StatusAction>>(value.field("actions"));
    if (!id || !priority || !generation || !accessibleLabel || !actions) {
        return false;
    }
    out.emplace(StatusItemView{*id, *priority, *generation, *accessibleLabel,
                               *actions});
    return true;
}

ProtocolValue toValue(StatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("items", toValue(value.items));
    fields.emplace_back("selected", toValue(static_cast<std::uint64_t>(value.selected)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto items = requireField<std::vector<StatusItemView>>(value.field("items"));
    auto selected = requireField<std::uint64_t>(value.field("selected"));
    if (!items || !selected) return false;
    out.emplace(StatusViewState{*items, static_cast<std::size_t>(*selected)});
    return true;
}

ProtocolValue toValue(PromptStatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("active_kind", toValue(value.activeKind));
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto status = requireField<StatusViewState>(value.field("status"));
    if (!status) return false;
    std::optional<PromptKind> activeKind;
    if (!decodeOptionalField(value.field("active_kind"), activeKind)) return false;
    out.emplace(PromptStatusViewState{*status, activeKind});
    return true;
}

ProtocolValue toValue(PromptStatusDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<PromptStatusViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(PromptStatusDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(ClipboardWrite const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("request_revision", toValue(value.requestRevision));
    fields.emplace_back("text", toValue(value.text));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardWrite>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::uint64_t>(value.field("id"));
    auto requestRevision = requireField<Revision>(value.field("request_revision"));
    auto text = requireField<std::string>(value.field("text"));
    if (!id || !requestRevision || !text) return false;
    out.emplace(ClipboardWrite{*id, *requestRevision, *text});
    return true;
}

ProtocolValue toValue(ClipboardViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("fragments", toValue(value.fragments));
    fields.emplace_back("plain_text", toValue(value.plainText));
    fields.emplace_back("system_write", toValue(value.systemWrite));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto fragments = requireField<std::vector<std::string>>(value.field("fragments"));
    auto plainText = requireField<std::string>(value.field("plain_text"));
    if (!fragments || !plainText) return false;
    ClipboardViewState result;
    result.fragments = *fragments;
    result.plainText = *plainText;
    if (!decodeOptionalField(value.field("system_write"), result.systemWrite)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(ClipboardDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<ClipboardViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ClipboardDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(SearchResult const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("column", toValue(static_cast<std::uint64_t>(value.column)));
    fields.emplace_back("score", toValue(value.score));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SearchResult>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto mode = requireField<SearchMode>(value.field("mode"));
    auto path = requireField<std::string>(value.field("path"));
    auto label = requireField<std::string>(value.field("label"));
    auto column = requireField<std::uint64_t>(value.field("column"));
    auto score = requireField<int>(value.field("score"));
    if (!mode || !path || !label || !column || !score) return false;
    SearchResult result;
    result.mode = *mode;
    result.path = *path;
    result.label = *label;
    if (!decodeOptionalField(value.field("line"), result.line)) return false;
    result.column = static_cast<std::size_t>(*column);
    result.score = *score;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(SearchViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("palette_open", toValue(value.paletteOpen));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("results", toValue(value.results));
    if (value.selectedIndex) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selectedIndex)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    fields.emplace_back("search_generation", toValue(value.searchGeneration));
    fields.emplace_back("searching", toValue(value.searching));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SearchViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto paletteOpen = requireField<bool>(value.field("palette_open"));
    auto query = requireField<std::string>(value.field("query"));
    auto mode = requireField<SearchMode>(value.field("mode"));
    auto results = requireField<std::vector<SearchResult>>(value.field("results"));
    auto searchGeneration = requireField<std::uint64_t>(value.field("search_generation"));
    auto searching = requireField<bool>(value.field("searching"));
    if (!revision || !paletteOpen || !query || !mode || !results ||
        !searchGeneration || !searching) {
        return false;
    }
    SearchViewState result;
    result.revision = *revision;
    result.paletteOpen = *paletteOpen;
    result.query = *query;
    result.mode = *mode;
    result.results = *results;
    std::optional<std::uint64_t> selectedIndex;
    if (!decodeOptionalField(value.field("selected_index"), selectedIndex)) {
        return false;
    }
    if (selectedIndex) {
        result.selectedIndex = static_cast<std::size_t>(*selectedIndex);
    }
    result.searchGeneration = *searchGeneration;
    result.searching = *searching;
    out.emplace(std::move(result));
    return true;
}



ProtocolValue toValue(ByteRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ByteRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(ByteRange{*begin, *end});
    return true;
}

ProtocolValue toValue(FindOptions const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("case_sensitive", toValue(value.caseSensitive));
    fields.emplace_back("whole_word", toValue(value.wholeWord));
    fields.emplace_back("regex", toValue(value.regex));
    fields.emplace_back("selection_only", toValue(value.selectionOnly));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindOptions>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto caseSensitive = requireField<bool>(value.field("case_sensitive"));
    auto wholeWord = requireField<bool>(value.field("whole_word"));
    auto regex = requireField<bool>(value.field("regex"));
    auto selectionOnly = requireField<bool>(value.field("selection_only"));
    if (!caseSensitive || !wholeWord || !regex || !selectionOnly) return false;
    out.emplace(FindOptions{*caseSensitive, *wholeWord, *regex, *selectionOnly});
    return true;
}

ProtocolValue toValue(FindRequest const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("selection", toValue(value.selection));
    fields.emplace_back("work_budget", toValue(value.workBudget));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindRequest>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto query = requireField<std::string>(value.field("query"));
    auto options = requireField<FindOptions>(value.field("options"));
    std::optional<ByteRange> selection;
    auto workBudget = requireField<std::uint64_t>(value.field("work_budget"));
    if (!query || !options ||
        !decodeOptionalField(value.field("selection"), selection) ||
        !workBudget) {
        return false;
    }
    out.emplace(FindRequest{*query, *options, std::move(selection),
                            *workBudget, nullptr});
    return true;
}

ProtocolValue toValue(FindMatch const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindMatch>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(FindMatch{*begin, *end});
    return true;
}

ProtocolValue toValue(WorkspaceFileReplacement const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("before", toValue(value.before));
    fields.emplace_back("after", toValue(value.after));
    fields.emplace_back("matches", toValue(value.matches));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto path = requireField<std::string>(value.field("path"));
    auto before = requireField<std::string>(value.field("before"));
    auto after = requireField<std::string>(value.field("after"));
    auto matches = requireField<std::vector<FindMatch>>(value.field("matches"));
    if (!path || !before || !after || !matches) return false;
    out.emplace(WorkspaceFileReplacement{*path, *before, *after, *matches});
    return true;
}

ProtocolValue toValue(WorkspaceReplacePreview const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("source_revision", toValue(value.sourceRevision));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("replacement", toValue(value.replacement));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("changes", toValue(value.changes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto sourceRevision = requireField<Revision>(value.field("source_revision"));
    auto query = requireField<std::string>(value.field("query"));
    auto replacement = requireField<std::string>(value.field("replacement"));
    auto options = requireField<FindOptions>(value.field("options"));
    auto changes = requireField<std::vector<WorkspaceFileReplacement>>(value.field("changes"));
    if (!sourceRevision || !query || !replacement || !options || !changes) return false;
    out.emplace(WorkspaceReplacePreview{*sourceRevision, *query,
                                        *replacement, *options, *changes});
    return true;
}

ProtocolValue toValue(FindReplaceViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("open", toValue(value.open));
    fields.emplace_back("replace_mode", toValue(value.replaceMode));
    fields.emplace_back("source_revision", toValue(value.sourceRevision));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("replacement", toValue(value.replacement));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("matches", toValue(value.matches));
    if (value.activeMatch) {
        fields.emplace_back("active_match",
                            toValue(static_cast<std::uint64_t>(*value.activeMatch)));
    } else {
        fields.emplace_back("active_match", ProtocolValue::makeNull());
    }
    fields.emplace_back("error", toValue(value.error));
    fields.emplace_back("message", toValue(value.message));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto open = requireField<bool>(value.field("open"));
    auto replaceMode = requireField<bool>(value.field("replace_mode"));
    auto sourceRevision = requireField<Revision>(value.field("source_revision"));
    auto query = requireField<std::string>(value.field("query"));
    auto replacement = requireField<std::string>(value.field("replacement"));
    auto options = requireField<FindOptions>(value.field("options"));
    auto matches = requireField<std::vector<FindMatch>>(value.field("matches"));
    auto error = requireField<FindReplaceError>(value.field("error"));
    auto message = requireField<std::string>(value.field("message"));
    if (!generation || !open || !replaceMode || !sourceRevision || !query ||
        !replacement || !options || !matches || !error || !message) {
        return false;
    }
    FindReplaceViewState result;
    result.generation = *generation;
    result.open = *open;
    result.replaceMode = *replaceMode;
    result.sourceRevision = *sourceRevision;
    result.query = *query;
    result.replacement = *replacement;
    result.options = *options;
    result.matches = *matches;
    std::optional<std::uint64_t> activeMatch;
    if (!decodeOptionalField(value.field("active_match"), activeMatch)) {
        return false;
    }
    if (activeMatch) result.activeMatch = static_cast<std::size_t>(*activeMatch);
    result.error = *error;
    result.message = *message;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(SettingValue const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("index", toValue(static_cast<std::uint64_t>(value.index())));
    std::visit(
        [&fields](auto const& alt) { fields.emplace_back("value", toValue(alt)); },
        value);
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingValue>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto index = requireField<std::uint64_t>(value.field("index"));
    if (!index) return false;
    auto const* altValue = value.field("value");
    if (!altValue) return false;
    switch (*index) {
        case 0: {
            auto decoded = requireField<bool>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<0>, *decoded});
            return true;
        }
        case 1: {
            auto decoded = requireField<std::uint32_t>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<1>, *decoded});
            return true;
        }
        case 2: {
            auto decoded = requireField<std::uint64_t>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<2>, *decoded});
            return true;
        }
        case 3: {
            auto decoded = requireField<IndentStyle>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<3>, *decoded});
            return true;
        }
        case 4: {
            auto decoded = requireField<LineEnding>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<4>, *decoded});
            return true;
        }
        case 5: {
            auto decoded = requireField<TextEncoding>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<5>, *decoded});
            return true;
        }
        case 6: {
            auto decoded = requireField<std::string>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<6>, *decoded});
            return true;
        }
        default:
            return false;
    }
}

ProtocolValue toValue(EffectiveSetting const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("source", toValue(value.source));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<EffectiveSetting>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto settingValue = requireField<SettingValue>(value.field("value"));
    auto source = requireField<SettingScope>(value.field("source"));
    if (!settingValue || !source) return false;
    out.emplace(EffectiveSetting{*settingValue, *source});
    return true;
}

ProtocolValue toValue(SettingViewEntry const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("key", toValue(value.key));
    fields.emplace_back("effective", toValue(value.effective));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingViewEntry>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto key = requireField<SettingKey>(value.field("key"));
    auto effective = requireField<EffectiveSetting>(value.field("effective"));
    if (!key || !effective) return false;
    out.emplace(SettingViewEntry{*key, *effective});
    return true;
}

ProtocolValue toValue(SettingsViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("entries", toValue(value.entries));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingsViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto entries =
        requireField<std::array<SettingViewEntry, kSettingKeyCount>>(
            value.field("entries"));
    if (!entries) return false;
    SettingsViewState result;
    result.entries = *entries;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(SettingsDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("key", toValue(value.key));
    fields.emplace_back("before", toValue(value.before));
    fields.emplace_back("after", toValue(value.after));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingsDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto key = requireField<SettingKey>(value.field("key"));
    auto before = requireField<EffectiveSetting>(value.field("before"));
    auto after = requireField<EffectiveSetting>(value.field("after"));
    if (!key || !before || !after) return false;
    out.emplace(SettingsDelta{*key, *before, *after});
    return true;
}

ProtocolValue toValue(SettingsSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changes", toValue(value.changes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changes = requireField<std::vector<SettingsDelta>>(value.field("changes"));
    if (!changes) return false;
    out.emplace(SettingsSectionDelta{*changes});
    return true;
}


ProtocolValue toValue(KeyStroke const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("code", toValue(std::string{keyCodeName(value.code)}));
    fields.emplace_back("control", toValue(value.control));
    fields.emplace_back("alt", toValue(value.alt));
    fields.emplace_back("meta", toValue(value.meta));
    fields.emplace_back("shift", toValue(value.shift));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeyStroke>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto code = requireField<std::string>(value.field("code"));
    auto control = requireField<bool>(value.field("control"));
    auto alt = requireField<bool>(value.field("alt"));
    auto meta = requireField<bool>(value.field("meta"));
    auto shift = requireField<bool>(value.field("shift"));
    if (!code || !control || !alt || !meta || !shift) return false;
    // The wire carries the key's NAME; a name outside the decoder's key set
    // names no key, so the stroke is undecodable rather than silently dead.
    auto const key = keyCodeFromName(*code);
    if (key == KeyCode::None) return false;
    out.emplace(KeyStroke{key, *control, *alt, *meta, *shift});
    return true;
}

ProtocolValue toValue(KeyBinding const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("sequence", toValue(value.sequence));
    fields.emplace_back("command_id", toValue(value.commandId));
    fields.emplace_back("context", toValue(value.context));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeyBinding>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto sequence = requireField<std::vector<KeyStroke>>(value.field("sequence"));
    auto commandId = requireField<std::string>(value.field("command_id"));
    auto context = requireField<std::string>(value.field("context"));
    if (!sequence || !commandId || !context) return false;
    out.emplace(KeyBinding{*sequence, *commandId, *context});
    return true;
}

ProtocolValue toValue(KeymapViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("name", toValue(value.name));
    fields.emplace_back("bindings", toValue(value.bindings));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeymapViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto name = requireField<std::string>(value.field("name"));
    auto bindings = requireField<std::vector<KeyBinding>>(value.field("bindings"));
    if (!name || !bindings) return false;
    out.emplace(KeymapViewState{*name, *bindings});
    return true;
}

ProtocolValue toValue(KeymapDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeymapDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<KeymapViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(KeymapDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(TextEncodingStatus const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", toValue(value.encoding));
    fields.emplace_back("line_ending", toValue(value.lineEnding));
    fields.emplace_back("had_bom", toValue(value.hadBom));
    fields.emplace_back("final_newline", toValue(value.finalNewline));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingStatus>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto encoding = requireField<TextEncoding>(value.field("encoding"));
    auto lineEnding = requireField<LineEnding>(value.field("line_ending"));
    auto hadBom = requireField<bool>(value.field("had_bom"));
    auto finalNewline = requireField<bool>(value.field("final_newline"));
    if (!encoding || !lineEnding || !hadBom || !finalNewline) return false;
    out.emplace(TextEncodingStatus{*encoding, *lineEnding, *hadBom, *finalNewline});
    return true;
}

ProtocolValue toValue(TextEncodingViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto status = requireField<TextEncodingStatus>(value.field("status"));
    if (!status) return false;
    out.emplace(TextEncodingViewState{*status});
    return true;
}

ProtocolValue toValue(TabState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("document", toValue(value.document));
    fields.emplace_back("document_key", toValue(value.documentKey));
    fields.emplace_back("content_identity", toValue(value.contentIdentity));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("dirty", toValue(value.dirty));
    fields.emplace_back("recovery", toValue(value.recovery));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TabState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<TabId>(value.field("id"));
    auto kind = requireField<TabKind>(value.field("kind"));
    auto contentIdentity = requireField<std::string>(value.field("content_identity"));
    auto label = requireField<std::string>(value.field("label"));
    auto mode = requireField<DocumentMode>(value.field("mode"));
    auto dirty = requireField<bool>(value.field("dirty"));
    auto recovery = requireField<TabRecoveryBadge>(value.field("recovery"));
    if (!id || !kind || !contentIdentity || !label || !mode || !dirty || !recovery) {
        return false;
    }
    TabState result;
    result.id = *id;
    result.kind = *kind;
    if (!decodeOptionalField(value.field("document"), result.document)) return false;
    if (!decodeOptionalField(value.field("document_key"), result.documentKey)) {
        return false;
    }
    result.contentIdentity = *contentIdentity;
    result.label = *label;
    result.mode = *mode;
    result.dirty = *dirty;
    result.recovery = *recovery;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(TabViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("tabs", toValue(value.tabs));
    fields.emplace_back("active", toValue(value.active));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TabViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto tabs = requireField<std::vector<TabState>>(value.field("tabs"));
    if (!tabs) return false;
    TabViewState result;
    result.tabs = *tabs;
    if (!decodeOptionalField(value.field("active"), result.active)) return false;
    out.emplace(std::move(result));
    return true;
}


}  // namespace ssg::protocol_detail
