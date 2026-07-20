#pragma once

#include <ssg/search.h>
#include <ssg/viewport.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct PaletteCandidate {
    std::string id;
    std::string label;
    std::string detail;

    friend bool operator==(const PaletteCandidate&, const PaletteCandidate&) = default;
};

struct PaletteViewState {
    SearchMode mode = SearchMode::Command;
    std::vector<PaletteCandidate> candidates;

    friend bool operator==(const PaletteViewState&, const PaletteViewState&) = default;
};

struct PaletteExecuteArguments {
    std::string commandId;

    friend bool operator==(const PaletteExecuteArguments&, const PaletteExecuteArguments&) = default;
};

struct PaletteReport {
    std::string query;
    std::string ghost;
    std::vector<PaletteCandidate> rows;
    std::optional<std::uint32_t> selected;
    std::uint32_t firstVisible = 0;
    ScrollbarMetrics scrollbar{};

    friend bool operator==(const PaletteReport&, const PaletteReport&) = default;
};

struct PaletteWindowState {
    std::string query;
    std::size_t selected = 0;
    std::uint32_t firstVisible = 0;
    std::uint32_t paneRows = 1;
};

class PaletteSearcher {
public:
    [[nodiscard]] std::vector<std::size_t> rank(
        std::vector<PaletteCandidate> const& candidates,
        std::string_view query) const;

    [[nodiscard]] std::string ghost(std::string_view topLabel,
                                    std::string_view query) const;

    [[nodiscard]] PaletteReport report(
        std::vector<PaletteCandidate> const& candidates,
        PaletteWindowState& window) const;
};

}  // namespace ssg
