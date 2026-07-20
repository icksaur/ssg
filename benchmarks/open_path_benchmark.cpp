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
#include <ssg/RecoveryActions.h>
#include <ssg/Workspace.h>

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

constexpr std::size_t kRepetitions = 12;
constexpr std::size_t kDiscard = 2;
constexpr std::size_t kTargetBytes = 10U * 1024U * 1024U;

constexpr std::array<const char*, 6> kPhaseNames{
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

fs::path makeFixture(const fs::path& root) {
    fs::create_directories(root);
    auto file = root / "big.txt";
    std::ofstream out{file, std::ios::binary};
    std::string line(80, 'a');
    line.push_back('\n');
    std::size_t written = 0;
    while (written < kTargetBytes) {
        out << line;
        written += line.size();
    }
    return file;
}

// Isolated size-hinted buffered read: stat the size, read the whole file in one
// sized pass.  This is the intrinsic-read term the SLO formula retains.
double calibratedReadMs(const fs::path& file) {
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

// Isolated "validate UTF-8 + copy" single pass: the intrinsic decode floor the
// fused LF-2b decoder approaches (one walk that checks lead bytes and copies each
// sequence straight through).  Representative, not a full validator.
double calibratedValidateCopyMs(const std::string& bytes) {
    auto const start = Clock::now();
    std::string out;
    out.reserve(bytes.size());
    const std::size_t n = bytes.size();
    std::size_t index = 0;
    while (index < n) {
        const auto c = static_cast<unsigned char>(bytes[index]);
        if (c < 0x80) {
            out.push_back(bytes[index]);
            ++index;
        } else {
            const std::size_t cont = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            const std::size_t len = std::min(cont + 1, n - index);
            out.append(bytes, index, len);
            index += len;
        }
    }
    auto const elapsed = Clock::now() - start;
    if (out.size() != bytes.size())
        throw std::runtime_error{"validate-copy calibration mismatch"};
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

std::string readWhole(const fs::path& file) {
    std::ifstream stream{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

struct Sample {
    std::array<double, kPhaseNames.size()> phaseMs{};
    double wallMs = 0.0;
};

Sample openOnce(const fs::path& root, const std::string& name) {
    // A fresh workspace per rep so open_file does not de-duplicate the path.
    auto recovery = ssg::RecoveryActions::create(root / ".recovery");
    auto workspace = ssg::Workspace::create(root, recovery);

    ssg::resetOpenPhaseTiming();
    auto const start = Clock::now();
    auto opened = workspace.openFile(name);
    if (!opened.accepted()) throw std::runtime_error{"open failed"};
    auto state = workspace.state(*opened.document);
    if (!state.has_value()) throw std::runtime_error{"state failed"};
    auto const wall = Clock::now() - start;

    Sample sample;
    sample.wallMs = std::chrono::duration<double, std::milli>(wall).count();
    for (std::size_t i = 0; i < kPhaseNames.size(); ++i)
        sample.phaseMs[i] =
            ms(ssg::openPhaseNs(static_cast<ssg::OpenPhase>(i)));
    return sample;
}

}  // namespace

int main() {
    try {
        auto root = fs::temp_directory_path() /
                    ("ssg-open-bench-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(root, ec);
        auto const file = makeFixture(root);
        auto const name = std::string{"big.txt"};

        std::vector<double> calib;
        for (std::size_t rep = 0; rep < kRepetitions; ++rep) {
            auto const value = calibratedReadMs(file);
            if (rep >= kDiscard) calib.push_back(value);
        }

        const std::string whole = readWhole(file);
        std::vector<double> calibDecode;
        for (std::size_t rep = 0; rep < kRepetitions; ++rep) {
            auto const value = calibratedValidateCopyMs(whole);
            if (rep >= kDiscard) calibDecode.push_back(value);
        }

        std::vector<Sample> samples;
        std::uint64_t validationCalls = 0;
        std::uint64_t treeTextCalls = 0;
        for (std::size_t rep = 0; rep < kRepetitions; ++rep) {
            ssg::resetUtf8ValidationCalls();
            ssg::resetPieceTreeTextCalls();
            auto sample = openOnce(root, name);
            if (rep == kDiscard) {
                validationCalls = ssg::utf8ValidationCalls();
                treeTextCalls = ssg::pieceTreeTextCalls();
            }
            if (rep >= kDiscard) samples.push_back(sample);
        }

        std::array<std::vector<double>, kPhaseNames.size()> phaseSamples;
        std::vector<double> wallSamples;
        for (auto const& sample : samples) {
            wallSamples.push_back(sample.wallMs);
            for (std::size_t i = 0; i < kPhaseNames.size(); ++i)
                phaseSamples[i].push_back(sample.phaseMs[i]);
        }

        std::size_t dominant = 0;
        double dominantP50 = 0.0;
        for (std::size_t i = 0; i < kPhaseNames.size(); ++i) {
            auto const p50 = percentile(phaseSamples[i], 0.50);
            if (p50 > dominantP50) {
                dominantP50 = p50;
                dominant = i;
            }
        }

        std::ostringstream report;
        report << std::fixed << std::setprecision(3)
               << "fixture big_10MiB samples=" << samples.size() << "\n"
               << "  open_wall            p50=" << percentile(wallSamples, 0.50)
               << "ms p99=" << percentile(wallSamples, 0.99) << "ms\n";
        for (std::size_t i = 0; i < kPhaseNames.size(); ++i) {
            report << "  " << std::left << std::setw(20) << kPhaseNames[i]
                   << " p50=" << percentile(phaseSamples[i], 0.50) << "ms"
                   << " p99=" << percentile(phaseSamples[i], 0.99) << "ms\n";
        }
        report << "  dominant_phase=" << kPhaseNames[dominant]
               << " (p50=" << dominantP50 << "ms)\n"
               << "  calibrated_buffered_read p50=" << percentile(calib, 0.50)
               << "ms p99=" << percentile(calib, 0.99) << "ms\n"
               << "  calibrated_validate_copy p50=" << percentile(calibDecode, 0.50)
               << "ms p99=" << percentile(calibDecode, 0.99) << "ms\n"
               << "  utf8_validation_calls=" << validationCalls
               << " piece_tree_text_calls_on_state=" << treeTextCalls << "\n";

        fs::remove_all(root, ec);

        std::cout << report.str();
        if (char const* outPath = std::getenv("SSG_OPEN_REPORT")) {
            std::ofstream out{outPath};
            out << report.str();
            std::cout << "open-path baseline written to " << outPath << "\n";
        }
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "open_path_benchmark: " << error.what() << "\n";
        return 1;
    }
}
