#include "codec_detail.h"

namespace ssg::protocol_detail {

ProtocolValue toValue(DiffWordRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back(
        "byte_start", toValue(static_cast<std::uint64_t>(value.byteStart)));
    fields.emplace_back(
        "byte_length", toValue(static_cast<std::uint64_t>(value.byteLength)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<DiffWordRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto byteStart = requireField<std::uint64_t>(value.field("byte_start"));
    auto byteLength = requireField<std::uint64_t>(value.field("byte_length"));
    if (!byteStart || !byteLength) return false;
    out.emplace(DiffWordRange{static_cast<std::size_t>(*byteStart),
                              static_cast<std::size_t>(*byteLength)});
    return true;
}

ProtocolValue toValue(DiffLineChange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    if (value.baselineLine) {
        fields.emplace_back("baseline_line",
                            toValue(static_cast<std::uint64_t>(*value.baselineLine)));
    } else {
        fields.emplace_back("baseline_line", ProtocolValue::makeNull());
    }
    if (value.targetLine) {
        fields.emplace_back("target_line",
                            toValue(static_cast<std::uint64_t>(*value.targetLine)));
    } else {
        fields.emplace_back("target_line", ProtocolValue::makeNull());
    }
    fields.emplace_back("target_added_word_ranges",
                        toValue(value.targetAddedWordRanges));
    fields.emplace_back("baseline_removed_word_ranges",
                        toValue(value.baselineRemovedWordRanges));
    fields.emplace_back("target_modified_word_ranges",
                        toValue(value.targetModifiedWordRanges));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffLineChange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<DiffLineKind>(value.field("kind"));
    if (!kind) return false;
    DiffLineChange result;
    result.kind = *kind;
    std::optional<std::uint64_t> baselineLine;
    if (!decodeOptionalField(value.field("baseline_line"), baselineLine)) return false;
    if (baselineLine) result.baselineLine = static_cast<std::size_t>(*baselineLine);
    std::optional<std::uint64_t> targetLine;
    if (!decodeOptionalField(value.field("target_line"), targetLine)) return false;
    if (targetLine) result.targetLine = static_cast<std::size_t>(*targetLine);
    if (auto const* field = value.field("target_added_word_ranges")) {
        auto ranges = requireField<std::vector<DiffWordRange>>(field);
        if (!ranges) return false;
        result.targetAddedWordRanges = std::move(*ranges);
    }
    if (auto const* field = value.field("baseline_removed_word_ranges")) {
        auto ranges = requireField<std::vector<DiffWordRange>>(field);
        if (!ranges) return false;
        result.baselineRemovedWordRanges = std::move(*ranges);
    }
    if (auto const* field = value.field("target_modified_word_ranges")) {
        auto ranges = requireField<std::vector<DiffWordRange>>(field);
        if (!ranges) return false;
        result.targetModifiedWordRanges = std::move(*ranges);
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(DiffHunk const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("baseline_start",
                        toValue(static_cast<std::uint64_t>(value.baselineStart)));
    fields.emplace_back("target_start",
                        toValue(static_cast<std::uint64_t>(value.targetStart)));
    fields.emplace_back("baseline_lines", toValue(value.baselineLines));
    fields.emplace_back("target_lines", toValue(value.targetLines));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffHunk>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baselineStart = requireField<std::uint64_t>(value.field("baseline_start"));
    auto targetStart = requireField<std::uint64_t>(value.field("target_start"));
    auto baselineLines = requireField<std::vector<std::string>>(value.field("baseline_lines"));
    auto targetLines = requireField<std::vector<std::string>>(value.field("target_lines"));
    if (!baselineStart || !targetStart || !baselineLines || !targetLines) {
        return false;
    }
    DiffHunk result;
    result.baselineStart = static_cast<std::size_t>(*baselineStart);
    result.targetStart = static_cast<std::size_t>(*targetStart);
    result.baselineLines = *baselineLines;
    result.targetLines = *targetLines;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(DiffFileView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("previous_path", toValue(value.previousPath));
    fields.emplace_back("deleted", toValue(value.deleted));
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("baseline_identity", toValue(value.baselineIdentity));
    fields.emplace_back("current_content", toValue(value.currentContent));
    fields.emplace_back("hunks", toValue(value.hunks));
    fields.emplace_back("changed_lines", toValue(value.changedLines));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto deleted = requireField<bool>(value.field("deleted"));
    auto status = requireField<DiffFileStatus>(value.field("status"));
    auto baselineIdentity = requireField<std::string>(value.field("baseline_identity"));
    auto currentContent = requireField<std::string>(value.field("current_content"));
    auto hunks = requireField<std::vector<DiffHunk>>(value.field("hunks"));
    auto changedLines = requireField<std::vector<DiffLineChange>>(value.field("changed_lines"));
    if (!id || !path || !deleted || !baselineIdentity || !currentContent || !hunks ||
        !changedLines) {
        return false;
    }
    std::optional<std::filesystem::path> previousPath;
    if (!decodeOptionalField(value.field("previous_path"), previousPath)) {
        return false;
    }
    auto const inferLegacyStatus = [&]() {
        if (*deleted) {
            return DiffFileStatus::Deleted;
        }
        bool baselineLooksAbsent = true;
        std::string addedOnlyReconstruction;
        for (const auto& hunk : *hunks) {
            if (!hunk.baselineLines.empty()) {
                baselineLooksAbsent = false;
                break;
            }
            for (const auto& line : hunk.targetLines) {
                addedOnlyReconstruction += line;
            }
        }
        if (baselineLooksAbsent) {
            for (const auto& change : *changedLines) {
                if (change.kind != DiffLineKind::Added ||
                    change.baselineLine.has_value()) {
                    baselineLooksAbsent = false;
                    break;
                }
            }
        }
        if (baselineLooksAbsent && addedOnlyReconstruction == *currentContent) {
            return DiffFileStatus::Added;
        }
        if (previousPath.has_value()) {
            return DiffFileStatus::Renamed;
        }
        return DiffFileStatus::Modified;
    };
    const auto decodedStatus = status.value_or(inferLegacyStatus());
    out.emplace(DiffFileView{.id = *id,
                             .path = *path,
                             .previousPath = std::move(previousPath),
                             .deleted = *deleted,
                             .status = decodedStatus,
                             .baselineIdentity = *baselineIdentity,
                             .currentContent = *currentContent,
                             .hunks = *hunks,
                             .changedLines = *changedLines});
    return true;
}

ProtocolValue toValue(DiffViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("files", toValue(value.files));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto files = requireField<std::vector<DiffFileView>>(value.field("files"));
    if (!revision || !files) return false;
    out.emplace(DiffViewState{*revision, *files});
    return true;
}

ProtocolValue toValue(DiffDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("upserted", toValue(value.upserted));
    fields.emplace_back("removed", toValue(value.removed));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto upserted = requireField<std::vector<DiffFileView>>(value.field("upserted"));
    auto removed = requireField<std::vector<DiffFileId>>(value.field("removed"));
    if (!baseRevision || !revision || !upserted || !removed) return false;
    out.emplace(DiffDelta{*baseRevision, *revision, *upserted, *removed});
    return true;
}


ProtocolValue toValue(ExternalActionAffordance const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("action", toValue(value.action));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("command", toValue(value.command));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<ExternalActionAffordance>& out) {
    if (!value.asObject()) return false;
    auto action = requireField<ExternalAction>(value.field("action"));
    auto label = requireField<std::string>(value.field("label"));
    auto command = requireField<std::string>(value.field("command"));
    if (!action || !label || !command) return false;
    auto const expected = externalActionAffordance(*action);
    if (*label != expected.label || *command != expected.command) return false;
    out.emplace(std::move(expected));
    return true;
}

ProtocolValue toValue(ExternalDocumentView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("accessible_status", toValue(value.accessibleStatus));
    fields.emplace_back("status_label", toValue(value.statusLabel));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto status = requireField<ExternalDocumentStatus>(value.field("status"));
    auto accessibleStatus = requireField<std::string>(value.field("accessible_status"));
    auto statusLabel = requireField<std::string>(value.field("status_label"));
    auto actions = requireField<std::vector<ExternalActionAffordance>>(
        value.field("actions"));
    if (!id || !path || !status || !accessibleStatus || !statusLabel ||
        !actions) return false;
    out.emplace(ExternalDocumentView{*id, *path, *status, *accessibleStatus,
                                     *statusLabel, *actions});
    return true;
}

ProtocolValue toValue(ExternalModificationViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("message", toValue(value.message));
    fields.emplace_back("files", toValue(value.files));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto message = requireField<std::string>(value.field("message"));
    auto files = requireField<std::vector<ExternalDocumentView>>(value.field("files"));
    if (!revision || !message || !files) return false;
    std::optional<DiffFileId> selected;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    // A present selection MUST name a file in this view -- a dangling selection is
    // rejected loud, never silently carried.
    if (selected.has_value() &&
        std::none_of(files->begin(), files->end(),
                     [&](auto const& file) { return file.id == *selected; })) {
        return false;
    }
    out.emplace(ExternalModificationViewState{*revision, *message, *files,
                                               selected});
    return true;
}

ProtocolValue toValue(ExternalModificationDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("message", toValue(value.message));
    fields.emplace_back("upserted", toValue(value.upserted));
    fields.emplace_back("removed", toValue(value.removed));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto message = requireField<std::string>(value.field("message"));
    auto upserted = requireField<std::vector<ExternalDocumentView>>(value.field("upserted"));
    auto removed = requireField<std::vector<DiffFileId>>(value.field("removed"));
    if (!baseRevision || !revision || !message || !upserted || !removed)
        return false;
    std::optional<DiffFileId> selected;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    out.emplace(ExternalModificationDelta{*baseRevision, *revision, *message,
                                           *upserted, *removed, selected});
    return true;
}


ProtocolValue toValue(ViewportDimensions const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("rows", toValue(value.rows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDimensions>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto columns = requireField<std::uint32_t>(value.field("columns"));
    auto rows = requireField<std::uint32_t>(value.field("rows"));
    if (!columns || !rows) return false;
    out.emplace(*columns, *rows);
    return true;
}

ProtocolValue toValue(VisualRow const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("logical_line", toValue(value.logicalLine));
    fields.emplace_back("first_span", toValue(value.firstSpan));
    fields.emplace_back("span_count", toValue(value.spanCount));
    fields.emplace_back("start_cell", toValue(value.startCell));
    fields.emplace_back("content_cells", toValue(value.contentCells));
    fields.emplace_back("visible_cells", toValue(value.visibleCells));
    fields.emplace_back("end_byte_offset", toValue(value.endByteOffset));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<VisualRow>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto logicalLine = requireField<std::uint32_t>(value.field("logical_line"));
    auto firstSpan = requireField<std::uint32_t>(value.field("first_span"));
    auto spanCount = requireField<std::uint32_t>(value.field("span_count"));
    auto startCell = requireField<CellIndex>(value.field("start_cell"));
    auto contentCells = requireField<std::uint32_t>(value.field("content_cells"));
    auto visibleCells = requireField<std::uint32_t>(value.field("visible_cells"));
    auto endByteOffset = requireField<std::uint32_t>(value.field("end_byte_offset"));
    if (!logicalLine || !firstSpan || !spanCount || !startCell || !contentCells ||
        !visibleCells || !endByteOffset) {
        return false;
    }
    out.emplace(VisualRow{*logicalLine, *firstSpan, *spanCount, *startCell,
                          *contentCells, *visibleCells, *endByteOffset});
    return true;
}

ProtocolValue toValue(ProjectedRow const& value) {
    std::vector<ProtocolValue::Field> fields;
    if (const auto* real = std::get_if<RealRow>(&value)) {
        fields.emplace_back("kind", toValue(std::string{"real"}));
        fields.emplace_back("buffer_line", toValue(real->bufferLine));
        fields.emplace_back("buffer_visual_row",
                            toValue(real->bufferVisualRow));
        fields.emplace_back("start_byte_offset",
                            toValue(real->startByteOffset));
        fields.emplace_back("end_byte_offset", toValue(real->endByteOffset));
        fields.emplace_back("start_cell", toValue(real->startCell));
        fields.emplace_back("end_cell", toValue(real->endCell));
    } else {
        const auto& phantom = std::get<PhantomRow>(value);
        fields.emplace_back("kind", toValue(std::string{"phantom"}));
        fields.emplace_back("baseline_line", toValue(phantom.baselineLine));
        fields.emplace_back("text", toValue(phantom.text));
        fields.emplace_back("following_byte_offset",
                            toValue(phantom.followingByteOffset));
        fields.emplace_back("removed_word_ranges",
                            toValue(phantom.removedWordRanges));
    }
    return ProtocolValue::makeObject(std::move(fields));
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<ProjectedRow>& out) {
    auto kind = requireField<std::string>(value.field("kind"));
    if (!kind) return false;
    if (*kind == "real") {
        auto bufferLine =
            requireField<std::uint32_t>(value.field("buffer_line"));
        auto bufferVisualRow =
            requireField<std::uint32_t>(value.field("buffer_visual_row"));
        auto startByteOffset =
            requireField<std::uint32_t>(value.field("start_byte_offset"));
        auto endByteOffset =
            requireField<std::uint32_t>(value.field("end_byte_offset"));
        auto startCell =
            requireField<std::uint32_t>(value.field("start_cell"));
        auto endCell =
            requireField<std::uint32_t>(value.field("end_cell"));
        if (!bufferLine || !bufferVisualRow || !startByteOffset ||
            !endByteOffset || !startCell || !endCell) {
            return false;
        }
        out.emplace(RealRow{
            *bufferLine, *bufferVisualRow, *startByteOffset, *endByteOffset,
            *startCell, *endCell});
        return true;
    }
    if (*kind == "phantom") {
        auto baselineLine =
            requireField<std::uint32_t>(value.field("baseline_line"));
        auto text = requireField<std::string>(value.field("text"));
        auto followingByteOffset = requireField<std::uint32_t>(
            value.field("following_byte_offset"));
        auto removedWordRanges = requireField<std::vector<DiffWordRange>>(
            value.field("removed_word_ranges"));
        if (!baselineLine || !text || !followingByteOffset ||
            !removedWordRanges) {
            return false;
        }
        out.emplace(PhantomRow{
            *baselineLine, std::move(*text), *followingByteOffset,
            std::move(*removedWordRanges)});
        return true;
    }
    return false;
}

ProtocolValue toValue(CellHitTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("viewport_row", toValue(value.viewportRow));
    fields.emplace_back("viewport_column", toValue(value.viewportColumn));
    fields.emplace_back("logical_line", toValue(value.logicalLine));
    fields.emplace_back("cell", toValue(value.cell));
    fields.emplace_back("byte_offset", toValue(value.byteOffset));
    fields.emplace_back("byte_len", toValue(value.byteLen));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<CellHitTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto viewportRow = requireField<std::uint32_t>(value.field("viewport_row"));
    auto viewportColumn = requireField<std::uint32_t>(value.field("viewport_column"));
    auto logicalLine = requireField<std::uint32_t>(value.field("logical_line"));
    auto cell = requireField<CellIndex>(value.field("cell"));
    auto byteOffset = requireField<std::uint32_t>(value.field("byte_offset"));
    auto byteLen = requireField<std::uint32_t>(value.field("byte_len"));
    if (!viewportRow || !viewportColumn || !logicalLine || !cell || !byteOffset ||
        !byteLen) {
        return false;
    }
    out.emplace(CellHitTarget{*viewportRow, *viewportColumn, *logicalLine, *cell,
                              *byteOffset, *byteLen});
    return true;
}

ProtocolValue toValue(ScrollbarMetrics const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("total_rows", toValue(value.totalRows));
    fields.emplace_back("viewport_rows", toValue(value.viewportRows));
    fields.emplace_back("first_row", toValue(value.firstRow));
    fields.emplace_back("maximum_first_row", toValue(value.maximumFirstRow));
    fields.emplace_back("thumb_start", toValue(value.thumbStart));
    fields.emplace_back("thumb_size", toValue(value.thumbSize));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto totalRows = requireField<std::uint32_t>(value.field("total_rows"));
    auto viewportRows = requireField<std::uint32_t>(value.field("viewport_rows"));
    auto firstRow = requireField<std::uint32_t>(value.field("first_row"));
    auto maximumFirstRow = requireField<std::uint32_t>(value.field("maximum_first_row"));
    auto thumbStart = requireField<std::uint32_t>(value.field("thumb_start"));
    auto thumbSize = requireField<std::uint32_t>(value.field("thumb_size"));
    if (!totalRows || !viewportRows || !firstRow || !maximumFirstRow || !thumbStart ||
        !thumbSize) {
        return false;
    }
    out.emplace(ScrollbarMetrics{*totalRows, *viewportRows, *firstRow,
                                 *maximumFirstRow, *thumbStart, *thumbSize});
    return true;
}

ProtocolValue toValue(ViewportViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("dimensions", toValue(value.dimensions));
    fields.emplace_back("first_visual_row", toValue(value.firstVisualRow));
    fields.emplace_back("first_visual_column", toValue(value.firstVisualColumn));
    fields.emplace_back("total_visual_rows", toValue(value.totalVisualRows));
    fields.emplace_back("visible_rows", toValue(value.visibleRows));
    fields.emplace_back("row_projection", toValue(value.rowProjection));
    fields.emplace_back("hit_targets", toValue(value.hitTargets));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ViewportViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto dimensions = requireField<ViewportDimensions>(value.field("dimensions"));
    auto firstVisualRow = requireField<std::uint32_t>(value.field("first_visual_row"));
    auto firstVisualColumn = requireField<std::uint32_t>(value.field("first_visual_column"));
    auto totalVisualRows = requireField<std::uint32_t>(value.field("total_visual_rows"));
    auto visibleRows = requireField<std::vector<VisualRow>>(value.field("visible_rows"));
    auto rowProjection =
        requireField<std::vector<ProjectedRow>>(value.field("row_projection"));
    auto hitTargets = requireField<std::vector<CellHitTarget>>(value.field("hit_targets"));
    auto scrollbar = requireField<ScrollbarMetrics>(value.field("scrollbar"));
    if (!dimensions || !firstVisualRow || !firstVisualColumn ||
        !totalVisualRows || !visibleRows || !rowProjection || !hitTargets ||
        !scrollbar) {
        return false;
    }
    out.emplace(ViewportViewState{*dimensions, *firstVisualRow,
                                  *firstVisualColumn, *totalVisualRows,
                                  *visibleRows, *rowProjection, *hitTargets,
                                  *scrollbar});
    return true;
}

ProtocolValue toValue(ViewportDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<ViewportViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ViewportDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(FollowScrollOffset const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("first_row", toValue(value.firstRow));
    fields.emplace_back("first_column", toValue(value.firstColumn));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowScrollOffset>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto firstRow = requireField<std::uint64_t>(value.field("first_row"));
    auto firstColumn = requireField<std::uint64_t>(value.field("first_column"));
    if (!firstRow || !firstColumn) return false;
    out.emplace(FollowScrollOffset{*firstRow, *firstColumn});
    return true;
}

ProtocolValue toValue(FollowTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("deleted", toValue(value.deleted));
    fields.emplace_back("newest_hunk_line",
                        toValue(static_cast<std::uint64_t>(value.newestHunkLine)));
    fields.emplace_back("source_revision", toValue(value.sourceRevision));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto deleted = requireField<bool>(value.field("deleted"));
    auto newestHunkLine = requireField<std::uint64_t>(value.field("newest_hunk_line"));
    auto sourceRevision = requireField<Revision>(value.field("source_revision"));
    if (!id || !path || !deleted || !newestHunkLine || !sourceRevision) return false;
    out.emplace(FollowTarget{*id, *path, *deleted,
                             static_cast<std::size_t>(*newestHunkLine),
                             *sourceRevision});
    return true;
}

ProtocolValue toValue(FollowClientView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client", toValue(value.client));
    fields.emplace_back("dimensions", toValue(value.dimensions));
    fields.emplace_back("offset", toValue(value.offset));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowClientView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto client = requireField<ClientId>(value.field("client"));
    auto dimensions = requireField<ViewportDimensions>(value.field("dimensions"));
    auto offset = requireField<FollowScrollOffset>(value.field("offset"));
    if (!client || !dimensions || !offset) return false;
    out.emplace(FollowClientView{*client, *dimensions, *offset});
    return true;
}

ProtocolValue toValue(ResolvedSelectionRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("anchor", toValue(value.anchor));
    fields.emplace_back("active", toValue(value.active));
    return ProtocolValue::makeObject(std::move(fields));
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<ResolvedSelectionRange>& out) {
    if (!detail::generated::validateResolvedSelectionRangeWire(value)) {
        return false;
    }
    auto anchor = requireField<ByteOffset>(value.field("anchor"));
    auto active = requireField<ByteOffset>(value.field("active"));
    if (!anchor || !active) return false;
    out.emplace(ResolvedSelectionRange{*anchor, *active});
    return true;
}

ProtocolValue toValue(FollowEditsViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("active_pane", toValue(value.activePane));
    fields.emplace_back("active_target", toValue(value.activeTarget));
    fields.emplace_back("queued_targets", toValue(value.queuedTargets));
    fields.emplace_back("clients", toValue(value.clients));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowEditsViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto mode = requireField<FollowMode>(value.field("mode"));
    auto activePane = requireField<PaneId>(value.field("active_pane"));
    auto queuedTargets = requireField<std::vector<FollowTarget>>(value.field("queued_targets"));
    auto clients = requireField<std::vector<FollowClientView>>(value.field("clients"));
    if (!generation || !mode || !activePane || !queuedTargets || !clients) return false;
    FollowEditsViewState result;
    result.generation = *generation;
    result.mode = *mode;
    result.activePane = *activePane;
    if (!decodeOptionalField(value.field("active_target"), result.activeTarget)) {
        return false;
    }
    result.queuedTargets = *queuedTargets;
    result.clients = *clients;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(TreeNodeCommand const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("label", toValue(value.label));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeCommand>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    if (!id || !label) return false;
    out.emplace(TreeNodeCommand{*id, *label});
    return true;
}

ProtocolValue toValue(GitTreeAffordance const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("short_label", toValue(value.shortLabel));
    fields.emplace_back("role", toValue(value.role));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<GitTreeAffordance>& out) {
    if (!value.asObject()) return false;
    auto status = requireField<GitTreeStatus>(value.field("status"));
    auto shortLabel = requireField<std::string>(value.field("short_label"));
    auto role = requireField<SemanticRole>(value.field("role"));
    if (!status || !shortLabel || !role) return false;
    auto const expected = gitTreeAffordance(*status);
    if (*shortLabel != expected.shortLabel || *role != expected.role) {
        return false;
    }
    out.emplace(std::move(expected));
    return true;
}

ProtocolValue toValue(TreeNode const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("parent_id", toValue(value.parentId));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("icon", toValue(value.icon));
    fields.emplace_back("commands", toValue(value.commands));
    fields.emplace_back("git_status", toValue(value.gitStatus));
    fields.emplace_back("workspace_path", toValue(value.workspacePath));
    if (value.sourceLine) {
        fields.emplace_back("source_line", toValue(*value.sourceLine));
    } else {
        fields.emplace_back("source_line", ProtocolValue::makeNull());
    }
    fields.emplace_back("expandable", toValue(value.expandable));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeNode>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<TreeNodeId>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    auto kind = requireField<TreeNodeKind>(value.field("kind"));
    auto commands = requireField<std::vector<TreeNodeCommand>>(value.field("commands"));
    auto expandable = requireField<bool>(value.field("expandable"));
    if (!id || !label || !kind || !commands || !expandable) return false;
    std::optional<TreeNodeId> parentId;
    if (!decodeOptionalField(value.field("parent_id"), parentId)) return false;
    std::optional<std::string> icon;
    if (!decodeOptionalField(value.field("icon"), icon)) return false;
    std::optional<GitTreeAffordance> gitStatus;
    if (!decodeOptionalField(value.field("git_status"), gitStatus)) return false;
    std::optional<std::string> workspacePath;
    if (!decodeOptionalField(value.field("workspace_path"), workspacePath)) return false;
    std::optional<std::uint32_t> sourceLine;
    if (!decodeOptionalField(value.field("source_line"), sourceLine)) return false;
    out.emplace(TreeNode{*id, std::move(parentId), *label, *kind, std::move(icon),
                         *commands, std::move(gitStatus), std::move(workspacePath),
                         std::move(sourceLine), *expandable});
    return true;
}

ProtocolValue toValue(TreeNodeView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("node", toValue(value.node));
    fields.emplace_back("depth", toValue(static_cast<std::uint64_t>(value.depth)));
    fields.emplace_back("expanded", toValue(value.expanded));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto node = requireField<TreeNode>(value.field("node"));
    auto depth = requireField<std::uint64_t>(value.field("depth"));
    auto expanded = requireField<bool>(value.field("expanded"));
    if (!node || !depth || !expanded) return false;
    out.emplace(TreeNodeView{*node, static_cast<std::size_t>(*depth), *expanded});
    return true;
}

ProtocolValue toValue(TreeProviderView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("provider_id", toValue(value.providerId));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("nodes", toValue(value.nodes));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto providerId = requireField<TreeProviderId>(value.field("provider_id"));
    auto kind = requireField<TreeProviderKind>(value.field("kind"));
    auto nodes = requireField<std::vector<TreeNodeView>>(value.field("nodes"));
    std::optional<TreeNodeId> selected;
    if (!providerId || !kind || !nodes) return false;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    out.emplace(TreeProviderView{*providerId, *kind, *nodes, selected});
    return true;
}

ProtocolValue toValue(TreeWindow const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("first_visible", toValue(value.firstVisible));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    fields.emplace_back("visible_node_ids", toValue(value.visibleNodeIds));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeWindow>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto firstVisible = requireField<std::uint32_t>(value.field("first_visible"));
    auto scrollbar = requireField<ScrollbarMetrics>(value.field("scrollbar"));
    auto visibleNodeIds =
        requireField<std::vector<TreeNodeId>>(value.field("visible_node_ids"));
    if (!firstVisible || !scrollbar || !visibleNodeIds) return false;
    out.emplace(TreeWindow{*firstVisible, *scrollbar,
                           std::move(*visibleNodeIds)});
    return true;
}

ProtocolValue toValue(TreeViewState const& value) {
    if (!isValidTreeViewState(value)) {
        throw std::invalid_argument("cannot encode an invalid TreeViewState");
    }
    auto providers = value.providers;
    if (value.activeBinding) {
        const auto active = std::find_if(
            providers.begin(), providers.end(), [&](const TreeProviderView& provider) {
                return provider.providerId == value.activeBinding->id &&
                       provider.kind == value.activeBinding->kind;
            });
        std::rotate(providers.begin(), active, std::next(active));
    }
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("providers", toValue(providers));
    fields.emplace_back("active_binding", toValue(value.activeBinding));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<TreeRevision>(value.field("revision"));
    auto providers = requireField<std::vector<TreeProviderView>>(value.field("providers"));
    if (!revision || !providers) return false;
    std::optional<TreeProviderBinding> activeBinding;
    const bool hasActiveBinding = value.field("active_binding") != nullptr;
    if (!decodeOptionalField(value.field("active_binding"), activeBinding)) return false;
    if (!hasActiveBinding && !providers->empty()) {
        activeBinding = TreeProviderBinding{providers->front().providerId,
                                            providers->front().kind};
    }
    TreeViewState decoded{*revision, std::move(*providers),
                          std::move(activeBinding)};
    if (!isValidTreeViewState(decoded)) return false;
    out.emplace(std::move(decoded));
    return true;
}

ProtocolValue toValue(TreeProviderDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("provider_id", toValue(value.providerId));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("remove_provider", toValue(value.removeProvider));
    fields.emplace_back("start", toValue(static_cast<std::uint64_t>(value.start)));
    fields.emplace_back("erase_count", toValue(static_cast<std::uint64_t>(value.eraseCount)));
    fields.emplace_back("insert", toValue(value.insert));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto providerId = requireField<TreeProviderId>(value.field("provider_id"));
    auto kind = requireField<TreeProviderKind>(value.field("kind"));
    auto removeProvider = requireField<bool>(value.field("remove_provider"));
    auto start = requireField<std::uint64_t>(value.field("start"));
    auto eraseCount = requireField<std::uint64_t>(value.field("erase_count"));
    auto insert = requireField<std::vector<TreeNodeView>>(value.field("insert"));
    if (!providerId || !kind || !removeProvider || !start || !eraseCount || !insert) {
        return false;
    }
    std::optional<TreeNodeId> selected;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    out.emplace(TreeProviderDelta{*providerId, *kind, *removeProvider,
                                  static_cast<std::size_t>(*start),
                                  static_cast<std::size_t>(*eraseCount), *insert,
                                  selected});
    return true;
}

ProtocolValue toValue(TreeDelta const& value) {
    auto providerOrder = value.providerOrder;
    if (value.activeBinding) {
        const auto active =
            std::ranges::find(providerOrder, value.activeBinding->id);
        if (active == providerOrder.end()) {
            throw std::invalid_argument(
                "cannot encode a TreeDelta whose active binding is absent");
        }
        std::rotate(providerOrder.begin(), active, std::next(active));
    }
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("snapshot_required", toValue(value.snapshotRequired));
    fields.emplace_back("providers", toValue(value.providers));
    fields.emplace_back("provider_order", toValue(providerOrder));
    if (value.activeBinding || providerOrder.empty()) {
        fields.emplace_back("active_binding", toValue(value.activeBinding));
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<TreeRevision>(value.field("base_revision"));
    auto revision = requireField<TreeRevision>(value.field("revision"));
    auto snapshotRequired = requireField<bool>(value.field("snapshot_required"));
    auto providers = requireField<std::vector<TreeProviderDelta>>(value.field("providers"));
    auto providerOrder =
        requireField<std::vector<TreeProviderId>>(value.field("provider_order"));
    if (!baseRevision || !revision || !snapshotRequired || !providers ||
        !providerOrder) return false;
    std::optional<TreeProviderBinding> activeBinding;
    if (!decodeOptionalField(value.field("active_binding"), activeBinding)) return false;
    out.emplace(TreeDelta{*baseRevision, *revision, *snapshotRequired, *providers,
                          *providerOrder, std::move(activeBinding)});
    return true;
}


ProtocolValue toValue(SyntaxRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(SyntaxRange{*begin, *end});
    return true;
}

ProtocolValue toValue(SyntaxSpan const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    fields.emplace_back("scope", toValue(value.scope));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxSpan>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    auto scope = requireField<SyntaxScope>(value.field("scope"));
    if (!begin || !end || !scope) return false;
    out.emplace(SyntaxSpan{*begin, *end, *scope});
    return true;
}

ProtocolValue toValue(SyntaxBracketPair const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("open", toValue(value.open));
    fields.emplace_back("close", toValue(value.close));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("depth", toValue(value.depth));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto open = requireField<ByteOffset>(value.field("open"));
    auto close = requireField<ByteOffset>(value.field("close"));
    auto kind = requireField<BracketKind>(value.field("kind"));
    auto depth = requireField<std::uint32_t>(value.field("depth"));
    if (!open || !close || !kind || !depth) return false;
    out.emplace(SyntaxBracketPair{*open, *close, *kind, *depth});
    return true;
}

ProtocolValue toValue(UnmatchedBracket const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("offset", toValue(value.offset));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("role", toValue(value.role));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<UnmatchedBracket>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto offset = requireField<ByteOffset>(value.field("offset"));
    auto kind = requireField<BracketKind>(value.field("kind"));
    auto role = requireField<BracketRole>(value.field("role"));
    if (!offset || !kind || !role) return false;
    out.emplace(UnmatchedBracket{*offset, *kind, *role});
    return true;
}

ProtocolValue toValue(CommentToken const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", toValue(value.range));
    fields.emplace_back("role", toValue(value.role));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<CommentToken>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto range = requireField<SyntaxRange>(value.field("range"));
    auto role = requireField<CommentTokenRole>(value.field("role"));
    if (!range || !role) return false;
    out.emplace(CommentToken{*range, *role});
    return true;
}

ProtocolValue toValue(CommentRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", toValue(value.range));
    fields.emplace_back("kind", toValue(value.kind));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<CommentRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto range = requireField<SyntaxRange>(value.field("range"));
    auto kind = requireField<CommentKind>(value.field("kind"));
    if (!range || !kind) return false;
    out.emplace(CommentRange{*range, *kind});
    return true;
}

ProtocolValue toValue(LineIndentation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("line_start", toValue(value.lineStart));
    fields.emplace_back("content_start", toValue(value.contentStart));
    fields.emplace_back("spaces", toValue(value.spaces));
    fields.emplace_back("tabs", toValue(value.tabs));
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("blank", toValue(value.blank));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LineIndentation>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto line = requireField<LineIndex>(value.field("line"));
    auto lineStart = requireField<ByteOffset>(value.field("line_start"));
    auto contentStart = requireField<ByteOffset>(value.field("content_start"));
    auto spaces = requireField<std::uint32_t>(value.field("spaces"));
    auto tabs = requireField<std::uint32_t>(value.field("tabs"));
    auto columns = requireField<std::uint32_t>(value.field("columns"));
    auto blank = requireField<bool>(value.field("blank"));
    if (!line || !lineStart || !contentStart || !spaces || !tabs || !columns || !blank) {
        return false;
    }
    out.emplace(LineIndentation{*line, *lineStart, *contentStart, *spaces, *tabs,
                                *columns, *blank});
    return true;
}

ProtocolValue toValue(SyntaxViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision()));
    fields.emplace_back("language", toValue(value.language()));
    fields.emplace_back("text_bytes", toValue(value.textBytes()));
    fields.emplace_back("spans", toValue(value.spans()));
    fields.emplace_back("bracket_pairs", toValue(value.bracketPairs()));
    fields.emplace_back("unmatched_brackets", toValue(value.unmatchedBrackets()));
    fields.emplace_back("comment_tokens", toValue(value.commentTokens()));
    fields.emplace_back("comment_ranges", toValue(value.commentRanges()));
    fields.emplace_back("indentation", toValue(value.indentation()));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto language = requireField<LanguageId>(value.field("language"));
    auto textBytes = requireField<std::uint64_t>(value.field("text_bytes"));
    auto spans = requireField<std::vector<SyntaxSpan>>(value.field("spans"));
    auto bracketPairs = requireField<std::vector<SyntaxBracketPair>>(value.field("bracket_pairs"));
    auto unmatchedBrackets =
        requireField<std::vector<UnmatchedBracket>>(value.field("unmatched_brackets"));
    auto commentTokens = requireField<std::vector<CommentToken>>(value.field("comment_tokens"));
    auto commentRanges = requireField<std::vector<CommentRange>>(value.field("comment_ranges"));
    auto indentation = requireField<std::vector<LineIndentation>>(value.field("indentation"));
    if (!revision || !language || !textBytes || !spans || !bracketPairs ||
        !unmatchedBrackets || !commentTokens || !commentRanges || !indentation) {
        return false;
    }
    out.emplace(*revision, *language, *textBytes, *spans, *bracketPairs,
               *unmatchedBrackets, *commentTokens, *commentRanges, *indentation);
    return true;
}


ProtocolValue toValue(LspPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("character", toValue(value.character));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspPosition>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto line = requireField<std::uint64_t>(value.field("line"));
    auto character = requireField<std::uint64_t>(value.field("character"));
    if (!line || !character) return false;
    out.emplace(LspPosition{*line, *character});
    return true;
}

ProtocolValue toValue(LspRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("start", toValue(value.start));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto start = requireField<LspPosition>(value.field("start"));
    auto end = requireField<LspPosition>(value.field("end"));
    if (!start || !end) return false;
    out.emplace(LspRange{*start, *end});
    return true;
}

ProtocolValue toValue(LspDiagnostic const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", toValue(value.range));
    fields.emplace_back("severity", toValue(value.severity));
    fields.emplace_back("code", toValue(value.code));
    fields.emplace_back("message", toValue(value.message));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnostic>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto range = requireField<LspRange>(value.field("range"));
    auto code = requireField<std::string>(value.field("code"));
    auto message = requireField<std::string>(value.field("message"));
    if (!range || !code || !message) return false;
    LspDiagnostic result;
    result.range = *range;
    if (!decodeOptionalField(value.field("severity"), result.severity)) return false;
    result.code = *code;
    result.message = *message;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspDocumentDiagnostics const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("uri", toValue(value.uri));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("diagnostics", toValue(value.diagnostics));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto uri = requireField<std::string>(value.field("uri"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto diagnostics = requireField<std::vector<LspDiagnostic>>(value.field("diagnostics"));
    if (!uri || !revision || !diagnostics) return false;
    out.emplace(LspDocumentDiagnostics{*uri, *revision, *diagnostics});
    return true;
}

ProtocolValue toValue(LspSyncViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("documents", toValue(value.documents));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspSyncViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto documents = requireField<std::vector<LspDocumentDiagnostics>>(value.field("documents"));
    if (!revision || !documents) return false;
    out.emplace(LspSyncViewState{*revision, *documents});
    return true;
}

ProtocolValue toValue(LspCompletionItem const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("detail", toValue(value.detail));
    fields.emplace_back("sort_text", toValue(value.sortText));
    fields.emplace_back("insert_text", toValue(value.insertText));
    fields.emplace_back("replacement_range", toValue(value.replacementRange));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionItem>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto label = requireField<std::string>(value.field("label"));
    auto detail = requireField<std::string>(value.field("detail"));
    auto sortText = requireField<std::string>(value.field("sort_text"));
    auto insertText = requireField<std::string>(value.field("insert_text"));
    if (!label || !detail || !sortText || !insertText) return false;
    LspCompletionItem result;
    result.label = *label;
    result.detail = *detail;
    result.sortText = *sortText;
    result.insertText = *insertText;
    if (!decodeOptionalField(value.field("replacement_range"), result.replacementRange)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspCompletionViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("visible", toValue(value.visible));
    fields.emplace_back("loading", toValue(value.loading));
    fields.emplace_back("items", toValue(value.items));
    if (value.selectedIndex) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selectedIndex)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto visible = requireField<bool>(value.field("visible"));
    auto loading = requireField<bool>(value.field("loading"));
    auto items = requireField<std::vector<LspCompletionItem>>(value.field("items"));
    if (!visible || !loading || !items) return false;
    LspCompletionViewState result;
    result.visible = *visible;
    result.loading = *loading;
    result.items = *items;
    std::optional<std::uint64_t> selectedIndex;
    if (!decodeOptionalField(value.field("selected_index"), selectedIndex)) return false;
    if (selectedIndex) result.selectedIndex = static_cast<std::size_t>(*selectedIndex);
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspHover const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("contents", toValue(value.contents));
    fields.emplace_back("range", toValue(value.range));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspHover>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto contents = requireField<std::string>(value.field("contents"));
    if (!contents) return false;
    LspHover result;
    result.contents = *contents;
    if (!decodeOptionalField(value.field("range"), result.range)) return false;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspNavigationTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("uri", toValue(value.uri));
    fields.emplace_back("range", toValue(value.range));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto uri = requireField<std::string>(value.field("uri"));
    auto range = requireField<LspRange>(value.field("range"));
    if (!uri || !range) return false;
    out.emplace(LspNavigationTarget{*uri, *range});
    return true;
}

ProtocolValue toValue(LspNavigationViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("targets", toValue(value.targets));
    if (value.selectedIndex) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selectedIndex)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    fields.emplace_back("user_navigation", toValue(value.userNavigation));
    fields.emplace_back("reveal_primary_caret", toValue(value.revealPrimaryCaret));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto targets = requireField<std::vector<LspNavigationTarget>>(value.field("targets"));
    auto userNavigation = requireField<bool>(value.field("user_navigation"));
    auto revealPrimaryCaret = requireField<bool>(value.field("reveal_primary_caret"));
    if (!targets || !userNavigation || !revealPrimaryCaret) return false;
    LspNavigationViewState result;
    result.targets = *targets;
    std::optional<std::uint64_t> selectedIndex;
    if (!decodeOptionalField(value.field("selected_index"), selectedIndex)) return false;
    if (selectedIndex) result.selectedIndex = static_cast<std::size_t>(*selectedIndex);
    result.userNavigation = *userNavigation;
    result.revealPrimaryCaret = *revealPrimaryCaret;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspFeatureViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("completion", toValue(value.completion));
    fields.emplace_back("hover", toValue(value.hover));
    fields.emplace_back("navigation", toValue(value.navigation));
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspFeatureViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto completion = requireField<LspCompletionViewState>(value.field("completion"));
    auto navigation = requireField<LspNavigationViewState>(value.field("navigation"));
    auto status = requireField<std::string>(value.field("status"));
    if (!revision || !completion || !navigation || !status) return false;
    LspFeatureViewState result;
    result.revision = *revision;
    result.completion = *completion;
    if (!decodeOptionalField(value.field("hover"), result.hover)) return false;
    result.navigation = *navigation;
    result.status = *status;
    out.emplace(std::move(result));
    return true;
}


}  // namespace ssg::protocol_detail
