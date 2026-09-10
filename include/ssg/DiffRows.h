#pragma once

#include <ssg/DiffModel.h>
#include <ssg/types.h>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ssg {

struct RealRow {
    uint32_t bufferLine;
    uint32_t bufferVisualRow;
    uint32_t startByteOffset = 0;
    uint32_t endByteOffset = 0;
    uint32_t startCell = 0;
    uint32_t endCell = 0;
    // Non-empty only for an unwrapped modified line rendered as one merged row.
    // The renderer paints these segments verbatim.
    std::vector<InlineWordSegment> mergedSegments;

    bool operator==(const RealRow&) const noexcept = default;
};

struct PhantomRow {
    uint32_t baselineLine;
    std::string text;
    uint32_t followingByteOffset;
    // Specific removed words for a modified pair; empty for a fully removed line.
    std::vector<DiffWordRange> removedWordRanges;

    bool operator==(const PhantomRow&) const noexcept = default;
};

using ProjectedRow = std::variant<RealRow, PhantomRow>;

class RowProjection {
public:
    explicit RowProjection(std::vector<ProjectedRow> rows);

    [[nodiscard]] const ProjectedRow& row(uint32_t visualRow) const;
    [[nodiscard]] uint32_t totalRows() const noexcept;
    [[nodiscard]] uint32_t visualRowForPosition(
        const DocumentPosition& position) const;
    [[nodiscard]] uint32_t movedRealRow(uint32_t visualRow,
                                        int64_t visualDistance) const;

private:
    std::vector<ProjectedRow> rows_;
};

}  // namespace ssg
