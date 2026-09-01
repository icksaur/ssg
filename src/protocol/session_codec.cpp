#include "codec_detail.h"

namespace ssg::protocol_detail {

namespace {
inline constexpr auto& kSemanticSessionFields =
    detail::generated::kSemanticSnapshotFields;
inline constexpr auto& kSemanticSessionDeltaFields =
    detail::generated::kSemanticDeltaFields;
}  // namespace

ProtocolValue toValue(SessionTopology const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("active_workspace", toValue(value.activeWorkspace));
    fields.emplace_back("active_view", toValue(value.activeView));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SessionTopology>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    SessionTopology result;
    if (!decodeOptionalField(value.field("active_workspace"), result.activeWorkspace)) {
        return false;
    }
    if (!decodeOptionalField(value.field("active_view"), result.activeView)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(ClientSnapshotState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client_id", toValue(value.clientId));
    fields.emplace_back("view_id", toValue(value.viewId));
    fields.emplace_back("capabilities", toValue(value.capabilities));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClientSnapshotState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto clientId = requireField<ClientId>(value.field("client_id"));
    auto viewId = requireField<ViewId>(value.field("view_id"));
    auto capabilities = requireField<std::vector<CapabilityId>>(value.field("capabilities"));
    if (!clientId || !viewId || !capabilities) return false;
    out.emplace(ClientSnapshotState{*clientId, *viewId, *capabilities});
    return true;
}

ProtocolValue toValue(SessionSnapshotSections const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("document", toValue(value.document));
    fields.emplace_back("selection", toValue(value.selection));
    fields.emplace_back("history", toValue(value.history));
    fields.emplace_back("clipboard", toValue(value.clipboard));
    fields.emplace_back("prompt_status", toValue(value.promptStatus));
    fields.emplace_back("search", toValue(value.search));
    fields.emplace_back("find_replace", toValue(value.findReplace));
    fields.emplace_back("settings", toValue(value.settings));
    fields.emplace_back("keymap", toValue(value.keymap));
    fields.emplace_back("text_encoding", toValue(value.textEncoding));
    fields.emplace_back("tabs", toValue(value.tabs));
    fields.emplace_back("diff", toValue(value.diff));
    fields.emplace_back("external_modification",
                        toValue(value.externalModification));
    fields.emplace_back("follow_edits", toValue(value.followEdits));
    fields.emplace_back("tree", toValue(value.tree));
    fields.emplace_back("syntax", toValue(value.syntax));
    fields.emplace_back("lsp_sync", toValue(value.lspSync));
    fields.emplace_back("lsp_features", toValue(value.lspFeatures));
    fields.emplace_back("theme", toValue(value.theme));
    fields.emplace_back("palette", encodePalette(value.palette));
    fields.emplace_back("ui_frame", encodeUiFrame(value.uiFrame));
    fields.emplace_back("notice_view", value.noticeView
                           ? toValue(*value.noticeView)
                           : ProtocolValue::makeNull());
    fields.emplace_back("watcher_available",
                        toValue(value.watcherAvailable));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto document = requireField<DocumentViewState>(value.field("document"));
    auto selection = requireField<SelectionSet>(value.field("selection"));
    auto history = requireField<HistoryViewState>(value.field("history"));
    auto clipboard = requireField<ClipboardViewState>(value.field("clipboard"));
    auto promptStatus = requireField<PromptStatusViewState>(value.field("prompt_status"));
    auto search = requireField<SearchViewState>(value.field("search"));
    auto findReplace = requireField<FindReplaceViewState>(value.field("find_replace"));
    auto settings = requireField<SettingsViewState>(value.field("settings"));
    auto keymap = requireField<KeymapViewState>(value.field("keymap"));
    auto textEncoding = requireField<TextEncodingViewState>(value.field("text_encoding"));
    auto tabs = requireField<TabViewState>(value.field("tabs"));
    auto diff = requireField<DiffViewState>(value.field("diff"));
    auto externalModification =
        requireField<ExternalModificationViewState>(value.field("external_modification"));
    auto followEdits = requireField<FollowEditsViewState>(value.field("follow_edits"));
    auto tree = requireField<TreeViewState>(value.field("tree"));
    auto syntax = requireField<SyntaxViewState>(value.field("syntax"));
    auto lspSync = requireField<LspSyncViewState>(value.field("lsp_sync"));
    auto lspFeatures = requireField<LspFeatureViewState>(value.field("lsp_features"));
    auto theme = requireField<ThemeSnapshot>(value.field("theme"));
    auto palette = value.field("palette")
        ? decodePalette(*value.field("palette"))
        : std::optional<PaletteViewState>{};
    auto uiFrame = value.field("ui_frame")
        ? decodeUiFrame(*value.field("ui_frame"))
        : std::optional<UiFrame>{};
    // Additive: absent OR null decodes to no notice; present-but-malformed fails loud.
    std::optional<NoticeView> noticeView;
    if (const ProtocolValue* noticeViewField = value.field("notice_view")) {
        if (noticeViewField->kind() != ProtocolValue::Kind::NullValue) {
            if (!decodePresent(*noticeViewField, noticeView)) return false;
        }
    }
    // Additive: an absent watcher-availability field decodes to available (true), so
    // a frame from a peer that predates it is never shown as unwatched; a
    // present-but-malformed field fails loud.
    bool watcherAvailable = true;
    if (const ProtocolValue* watcherField = value.field("watcher_available")) {
        auto decoded = requireField<bool>(watcherField);
        if (!decoded) return false;
        watcherAvailable = *decoded;
    }
    if (!document || !selection || !history || !clipboard || !promptStatus || !search ||
        !findReplace || !settings || !keymap || !textEncoding || !tabs || !diff ||
        !externalModification || !followEdits || !tree || !syntax || !lspSync ||
        !lspFeatures || !theme || !uiFrame) {
        return false;
    }
    if (!palette) return false;
    out.emplace(SessionSnapshotSections{
        *document, *selection, *history, *clipboard, *promptStatus, *search,
        *findReplace, *settings, *keymap, *textEncoding, *tabs, *diff,
        *externalModification, *followEdits, *tree, std::move(*syntax), *lspSync,
        *lspFeatures, *theme});
    out->palette = std::move(*palette);
    out->uiFrame = std::move(*uiFrame);
    out->noticeView = std::move(noticeView);
    out->watcherAvailable = watcherAvailable;
    return true;
}

}  // namespace ssg::protocol_detail

namespace ssg {

using namespace protocol_detail;

std::string ProtocolCodec::encodeSessionSnapshot(SessionSnapshot const& snapshot) const {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(snapshot.revision()));
    fields.emplace_back("topology", toValue(snapshot.topology()));
    fields.emplace_back("client", toValue(snapshot.client()));
    fields.emplace_back("sections", toValue(snapshot.sections()));
    return encodeMessage(ProtocolMessageKind::SessionSnapshot,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeSessionSnapshotResult ProtocolCodec::decodeSessionSnapshot(std::string_view bytes,
                                                    ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::SessionSnapshot, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!detail::generated::validateSessionSnapshotWire(payload)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session snapshot payload is malformed"};
    }
    auto revision = requireField<Revision>(payload.field("revision"));
    auto topology = requireField<SessionTopology>(payload.field("topology"));
    auto client = requireField<ClientSnapshotState>(payload.field("client"));
    auto sections =
        requireField<SessionSnapshotSections>(payload.field("sections"));
    if (!revision || !topology || !client || !sections) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session snapshot payload is malformed"};
    }
    return {ProtocolError::None,
            SessionSnapshot{*revision, std::move(*topology),
                            std::move(*client), std::move(*sections)},
            {}};
}

std::string ProtocolCodec::encodeSessionDelta(SessionDelta const& delta) const {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(delta.baseRevision()));
    fields.emplace_back("revision", toValue(delta.revision()));
    fields.emplace_back("client_id", toValue(delta.clientId()));
    fields.emplace_back("view_id", toValue(delta.viewId()));
    fields.emplace_back("capabilities", toValue(delta.capabilities()));
    fields.emplace_back("topology", toValue(delta.topology()));
    fields.emplace_back("document", toValue(delta.document()));
    fields.emplace_back("document_caret", toValue(delta.documentCaret()));
    fields.emplace_back("selection", toValue(delta.selection()));
    fields.emplace_back("history", toValue(delta.history()));
    fields.emplace_back("clipboard", toValue(delta.clipboard()));
    fields.emplace_back("prompt_status", toValue(delta.promptStatus()));
    fields.emplace_back("search", toValue(delta.search()));
    fields.emplace_back("find_replace", toValue(delta.findReplace()));
    fields.emplace_back("settings", toValue(delta.settings()));
    fields.emplace_back("keymap", toValue(delta.keymap()));
    fields.emplace_back("text_encoding", toValue(delta.textEncoding()));
    fields.emplace_back("tabs", toValue(delta.tabs()));
    fields.emplace_back("diff", toValue(delta.diff()));
    fields.emplace_back("external_modification",
                        toValue(delta.externalModification()));
    fields.emplace_back("follow_edits", toValue(delta.followEdits()));
    fields.emplace_back("tree", toValue(delta.tree()));
    fields.emplace_back("syntax", toValue(delta.syntax()));
    fields.emplace_back("lsp_sync", toValue(delta.lspSync()));
    fields.emplace_back("lsp_features", toValue(delta.lspFeatures()));
    fields.emplace_back("theme", toValue(delta.theme()));
    fields.emplace_back("ui_frame_delta",
                        encodeUiFrameDelta(delta.uiFrameDelta()));
    fields.emplace_back("palette",
                        delta.palette().replacement
                            ? encodePalette(*delta.palette().replacement)
                            : ProtocolValue::makeNull());
    fields.emplace_back("notice_view", toValue(delta.noticeView()));
    fields.emplace_back("watcher_available", toValue(delta.watcherAvailable()));
    return encodeMessage(ProtocolMessageKind::SessionDelta,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeSessionDeltaResult ProtocolCodec::decodeSessionDelta(std::string_view bytes,
                                              ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::SessionDelta, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!detail::generated::validateSessionDeltaWire(payload)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session delta payload is malformed"};
    }

    auto baseRevision = requireField<Revision>(payload.field("base_revision"));
    auto revision = requireField<Revision>(payload.field("revision"));
    auto clientId = requireField<ClientId>(payload.field("client_id"));
    auto viewId = requireField<ViewId>(payload.field("view_id"));
    auto capabilities =
        requireField<std::vector<CapabilityId>>(payload.field("capabilities"));

    std::optional<SessionTopology> topology;
    std::optional<DocumentDelta> document;
    std::optional<ByteOffset> documentCaret;
    std::optional<SearchViewState> search;
    std::optional<FindReplaceViewState> findReplace;
    std::optional<TextEncodingViewState> textEncoding;
    std::optional<TabViewState> tabs;
    std::optional<FollowEditsViewState> followEdits;
    std::optional<SyntaxViewState> syntax;
    std::optional<LspSyncViewState> lspSync;
    std::optional<LspFeatureViewState> lspFeatures;
    std::optional<bool> watcherAvailable;
    bool const optionalOk =
        decodeOptionalField(payload.field("topology"), topology) &&
        decodeOptionalField(payload.field("document"), document) &&
        decodeOptionalField(payload.field("document_caret"), documentCaret) &&
        decodeOptionalField(payload.field("watcher_available"), watcherAvailable) &&
        decodeOptionalField(payload.field("search"), search) &&
        decodeOptionalField(payload.field("find_replace"), findReplace) &&
        decodeOptionalField(payload.field("text_encoding"), textEncoding) &&
        decodeOptionalField(payload.field("tabs"), tabs) &&
        decodeOptionalField(payload.field("follow_edits"), followEdits) &&
        decodeOptionalField(payload.field("syntax"), syntax) &&
        decodeOptionalField(payload.field("lsp_sync"), lspSync) &&
        decodeOptionalField(payload.field("lsp_features"), lspFeatures);

    auto selection = requireField<SelectionSetDelta>(payload.field("selection"));
    auto history = requireField<HistoryDelta>(payload.field("history"));
    auto clipboard = requireField<ClipboardDelta>(payload.field("clipboard"));
    auto promptStatus =
        requireField<PromptStatusDelta>(payload.field("prompt_status"));
    auto settings =
        requireField<SettingsSectionDelta>(payload.field("settings"));
    auto keymap = requireField<KeymapDelta>(payload.field("keymap"));
    auto diff = requireField<DiffDelta>(payload.field("diff"));
    auto externalModification = requireField<ExternalModificationDelta>(
        payload.field("external_modification"));
    auto tree = requireField<TreeDelta>(payload.field("tree"));
    auto theme = requireField<ThemeSectionDelta>(payload.field("theme"));
    const ProtocolValue* frameDeltaField = payload.field("ui_frame_delta");
    std::optional<UiFrameDelta> uiFrameDelta;
    if (frameDeltaField) {
        uiFrameDelta = decodeUiFrameDelta(*frameDeltaField);
        if (!uiFrameDelta) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "session delta UI frame is malformed"};
        }
    }
    if (!uiFrameDelta) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session delta UI frame is missing"};
    }
    PaletteSectionDelta paletteDelta;
    if (const ProtocolValue* paletteField = payload.field("palette")) {
        if (paletteField->kind() != ProtocolValue::Kind::NullValue) {
            auto palette = decodePalette(*paletteField);
            if (!palette) {
                return {ProtocolError::MalformedMessage, std::nullopt,
                        "session delta payload is malformed"};
            }
            paletteDelta.replacement = std::move(*palette);
        }
    }
    // Additive: an absent notice_view field means "unchanged" (changed=false), so a
    // delta from a peer that predates the field never spuriously clears the notice; a
    // present-but-malformed field fails loud.
    NoticeViewSectionDelta noticeViewDelta;
    if (const ProtocolValue* noticeViewField = payload.field("notice_view")) {
        std::optional<NoticeViewSectionDelta> decoded;
        if (!decodePresent(*noticeViewField, decoded)) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "session delta payload is malformed"};
        }
        noticeViewDelta = std::move(*decoded);
    }

    if (!optionalOk || !baseRevision || !revision || !clientId || !viewId ||
        !capabilities || !selection || !history || !clipboard ||
        !promptStatus || !settings || !keymap ||
        !diff || !externalModification || !tree || !theme) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session delta payload is malformed"};
    }

    return {ProtocolError::None,
            SessionSnapshotCodec{}.decodeWire(
                *baseRevision, *revision, *clientId, *viewId,
                std::move(*capabilities), std::move(topology),
                std::move(document), std::move(documentCaret),
                std::move(*selection), std::move(*history),
                std::move(*clipboard), std::move(*promptStatus),
                std::move(search), std::move(findReplace),
                std::move(*settings), std::move(*keymap),
                std::move(textEncoding), std::move(tabs), std::move(*diff),
                std::move(*externalModification), std::move(followEdits),
                std::move(*tree), std::move(syntax), std::move(lspSync),
                std::move(lspFeatures), std::move(*theme),
                std::move(*uiFrameDelta), std::move(paletteDelta),
                std::move(noticeViewDelta), watcherAvailable),
            {}};
}


}  // namespace ssg
