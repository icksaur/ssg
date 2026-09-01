#pragma once

#include <ssg/LineLayoutCache.h>
#include <ssg/ViewportProjection.h>

#include <optional>
#include <vector>

namespace ssg {

struct ViewportProjectionState::Impl {
    LineLayoutCache viewportLineCache;
    std::optional<Revision> lineCountRevision;
    std::optional<FileDocumentId> lineCountDocument;
    std::uint32_t lineCountCache = 1;
    std::optional<Revision> cellRunsRevision;
    std::optional<FileDocumentId> cellRunsDocument;
    std::vector<CellRun> cellRunsCache;
};

}  // namespace ssg
