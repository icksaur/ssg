// LF-1 open-path measurement (Milestone 13, doc/spec-large-files-loading.md).
//
// Drives the REAL library open path (Workspace::open_file + Workspace::state)
// on a 10 MiB fixture in-process, attributing the cost to the cross-layer phase
// seam (read / NUL-scan / decode+validate / EOL-scan / document-build /
// state-dirty-check).  Also runs an isolated size-hinted buffered-read
// calibration so the retained "intrinsic read" term is known before LF-2
// introduces it, and reports the UTF-8 validation count (2 today) and the
// fresh-open tree-materialization count (1 today).  Baseline only; the enforce
// gate lands at LF-4b.

#include <ssg/open_metrics.h>
#include <ssg/recovery.h>
#include <ssg/workspace.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::size_t repetitions = 12;
constexpr std::size_t discard = 2;
constexpr std::size_t target_bytes = 10U * 1024U * 1024U;

constexpr std::array<const char*, 6> phase_names{
    "read", "nul_scan", "decode_validate", "eol_scan", "document_build",
    "state_dirty_check"};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    auto const index = static_cast<std::size_t>(
        std::ceil(p * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1'000'000.0; }

fs::path make_fixture(const fs::path& root) {
    fs::create_directories(root);
    auto file = root / "big.txt";
    std::ofstream out{file, std::ios::binary};
    std::string line(80, 'a');
    line.push_back('\n');
    std::size_t written = 0;
    while (written < target_bytes) {
        out << line;
        written += line.size();
    }
    return file;
}

// Isolated size-hinted buffered read: stat the size, read the whole file in one
// sized pass.  This is the intrinsic-read term the SLO formula retains.
double calibrated_read_ms(const fs::path& file) {
    auto const size = fs::file_size(file);
    auto const start = Clock::now();
    std::ifstream stream{file, std::ios::binary};
    std::string buffer;
    buffer.resize(static_cast<std::size_t>(size));
    stream.read(buffer.data(), static_cast<std::streamsize>(size));
    auto const elapsed = Clock::now() - start;
    if (static_cast<std::size_t>(stream.gcount()) != size)
        throw std::runtime_error{"calibration read short"};
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

struct Sample {
    std::array<double, phase_names.size()> phase_ms{};
    double wall_ms = 0.0;
};

Sample open_once(const fs::path& root, const std::string& name) {
    // A fresh workspace per rep so open_file does not de-duplicate the path.
    auto recovery = ssg::RecoveryActions::create(root / ".recovery");
    auto workspace = ssg::Workspace::create(root, recovery);

    ssg::reset_open_phase_timing();
    auto const start = Clock::now();
    auto opened = workspace.open_file(name);
    if (!opened.accepted()) throw std::runtime_error{"open failed"};
    auto state = workspace.state(*opened.document);
    if (!state.has_value()) throw std::runtime_error{"state failed"};
    auto const wall = Clock::now() - start;

    Sample sample;
    sample.wall_ms = std::chrono::duration<double, std::milli>(wall).count();
    for (std::size_t i = 0; i < phase_names.size(); ++i)
        sample.phase_ms[i] =
            ms(ssg::open_phase_ns(static_cast<ssg::OpenPhase>(i)));
    return sample;
}

}  // namespace

int main() {
    try {
        auto root = fs::temp_directory_path() /
                    ("ssg-open-bench-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(root, ec);
        auto const file = make_fixture(root);
        auto const name = std::string{"big.txt"};

        std::vector<double> calib;
        for (std::size_t rep = 0; rep < repetitions; ++rep) {
            auto const value = calibrated_read_ms(file);
            if (rep >= discard) calib.push_back(value);
        }

        std::vector<Sample> samples;
        std::uint64_t validation_calls = 0;
        std::uint64_t tree_text_calls = 0;
        for (std::size_t rep = 0; rep < repetitions; ++rep) {
            ssg::reset_utf8_validation_calls();
            ssg::reset_piece_tree_text_calls();
            auto sample = open_once(root, name);
            if (rep == discard) {
                validation_calls = ssg::utf8_validation_calls();
                tree_text_calls = ssg::piece_tree_text_calls();
            }
            if (rep >= discard) samples.push_back(sample);
        }

        std::array<std::vector<double>, phase_names.size()> phase_samples;
        std::vector<double> wall_samples;
        for (auto const& sample : samples) {
            wall_samples.push_back(sample.wall_ms);
            for (std::size_t i = 0; i < phase_names.size(); ++i)
                phase_samples[i].push_back(sample.phase_ms[i]);
        }

        std::size_t dominant = 0;
        double dominant_p50 = 0.0;
        for (std::size_t i = 0; i < phase_names.size(); ++i) {
            auto const p50 = percentile(phase_samples[i], 0.50);
            if (p50 > dominant_p50) {
                dominant_p50 = p50;
                dominant = i;
            }
        }

        std::ostringstream report;
        report << std::fixed << std::setprecision(3)
               << "fixture big_10MiB samples=" << samples.size() << "\n"
               << "  open_wall            p50=" << percentile(wall_samples, 0.50)
               << "ms p99=" << percentile(wall_samples, 0.99) << "ms\n";
        for (std::size_t i = 0; i < phase_names.size(); ++i) {
            report << "  " << std::left << std::setw(20) << phase_names[i]
                   << " p50=" << percentile(phase_samples[i], 0.50) << "ms"
                   << " p99=" << percentile(phase_samples[i], 0.99) << "ms\n";
        }
        report << "  dominant_phase=" << phase_names[dominant]
               << " (p50=" << dominant_p50 << "ms)\n"
               << "  calibrated_buffered_read p50=" << percentile(calib, 0.50)
               << "ms p99=" << percentile(calib, 0.99) << "ms\n"
               << "  utf8_validation_calls=" << validation_calls
               << " piece_tree_text_calls_on_state=" << tree_text_calls << "\n";

        fs::remove_all(root, ec);

        std::cout << report.str();
        if (char const* out_path = std::getenv("SSG_OPEN_REPORT")) {
            std::ofstream out{out_path};
            out << report.str();
            std::cout << "open-path baseline written to " << out_path << "\n";
        }
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "open_path_benchmark: " << error.what() << "\n";
        return 1;
    }
}
