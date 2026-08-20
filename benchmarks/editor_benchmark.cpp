#include <ssg/CommandCatalog.h>
#include <ssg/CommandSpecBuilder.h>
#include <ssg/CommandInvocation.h>
#include <ssg/Document.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/snapshot.h>

#include "../src/runtime/command_executor.h"
#include <ssg/Viewport.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kTargetBytes = 10U * 1024U * 1024U;
constexpr std::size_t kOperationCount = 10'000;
constexpr std::size_t kWarmupCount = 1'000;
constexpr std::size_t kRepetitions = 5;

struct Operation {
    bool insert;
    std::uint64_t offset;
    std::string text;
    std::uint64_t erasedBytes;
};

struct Timings {
    std::vector<double> editMicroseconds;
    std::vector<double> commandDeltaMicroseconds;
    std::vector<double> openViewportMilliseconds;
    double idleCpuMilliseconds{};
};

[[nodiscard]] double processCpuMilliseconds() {
#ifdef _WIN32
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error{"GetProcessTimes failed"};
    auto ticks = [](FILETIME value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
               value.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000.0;
#else
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0)
        throw std::runtime_error{"CLOCK_PROCESS_CPUTIME_ID failed"};
    return static_cast<double>(value.tv_sec) * 1'000.0 +
           static_cast<double>(value.tv_nsec) / 1'000'000.0;
#endif
}

[[nodiscard]] std::string readFile(std::string const& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"cannot read " + path};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

// Small self-contained SHA-256 keeps the corpus oracle portable to Windows
// without requiring an external crypto package or shell utility.
[[nodiscard]] std::string sha256(std::string_view input) {
    constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::vector<std::uint8_t> bytes(input.begin(), input.end());
    auto const bitLength = static_cast<std::uint64_t>(bytes.size()) * 8U;
    bytes.push_back(0x80);
    while ((bytes.size() % 64U) != 56U) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(bitLength >> shift));

    std::array<std::uint32_t, 8> h{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (std::size_t block = 0; block < bytes.size(); block += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            auto const at = block + i * 4;
            w[i] = (static_cast<std::uint32_t>(bytes[at]) << 24U) |
                   (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) |
                   bytes[at + 3];
        }
        for (std::size_t i = 16; i < 64; ++i) {
            auto const s0 = std::rotr(w[i - 15], 7) ^
                            std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            auto const s1 = std::rotr(w[i - 2], 17) ^
                            std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            auto const s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                            std::rotr(e, 25);
            auto const choice = (e & f) ^ (~e & g);
            auto const t1 = hh + s1 + choice + k[i] + w[i];
            auto const s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                            std::rotr(a, 22);
            auto const majority = (a & b) ^ (a & c) ^ (b & c);
            auto const t2 = s0 + majority;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        std::array<std::uint32_t, 8> const result{
            a, b, c, d, e, f, g, hh};
        for (std::size_t i = 0; i < h.size(); ++i) h[i] += result[i];
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (auto value : h) result << std::setw(8) << value;
    return result.str();
}

[[nodiscard]] std::string manifestHash(std::string const& manifest,
                                        std::string const& path) {
    std::regex pattern{"\"path\"\\s*:\\s*\"" + path +
                       "\"[^}]*\"sha256\"\\s*:\\s*\"([0-9a-f]{64})\""};
    std::smatch match;
    if (!std::regex_search(manifest, match, pattern))
        throw std::runtime_error{"manifest has no hash for " + path};
    return match[1].str();
}

[[nodiscard]] std::vector<Operation> loadOperations(std::string const& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"cannot read " + path};
    std::vector<Operation> operations;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream fields{line};
        std::string kind;
        std::string offset;
        std::string value;
        if (!std::getline(fields, kind, '\t') ||
            !std::getline(fields, offset, '\t') ||
            !std::getline(fields, value))
            throw std::runtime_error{"malformed operation line"};
        if (kind == "I")
            operations.push_back(
                {true, std::stoull(offset), value, std::uint64_t{0}});
        else if (kind == "D")
            operations.push_back(
                {false, std::stoull(offset), {}, std::stoull(value)});
        else
            throw std::runtime_error{"unknown operation kind"};
    }
    return operations;
}

[[nodiscard]] std::string expandCorpus(std::string const& seed) {
    if (seed.empty()) throw std::runtime_error{"empty benchmark corpus"};
    std::string result;
    result.reserve(kTargetBytes);
    while (result.size() + seed.size() <= kTargetBytes)
        result.append(seed);
    auto const remaining = kTargetBytes - result.size();
    std::size_t prefix = remaining;
    while (prefix < seed.size() &&
           (static_cast<unsigned char>(seed[prefix]) & 0xc0U) == 0x80U) {
        --prefix;
    }
    result.append(seed.data(), prefix);
    result.append(kTargetBytes - result.size(), ' ');
    return result;
}

[[nodiscard]] ssg::TransactionResult apply(ssg::Document& document,
                                           Operation const& operation) {
    return document.apply(
        {document.revision(),
         {{ssg::ByteOffset{operation.offset},
           operation.insert ? 0U : operation.erasedBytes,
           operation.insert ? operation.text : std::string{}}}});
}

void verifyInputs(std::string const& seed, std::string const& script) {
    auto const manifest = readFile(SSG_PERFORMANCE_MANIFEST);
    if (sha256(seed) != manifestHash(manifest, "mixed-code.txt"))
        throw std::runtime_error{"mixed-code.txt SHA-256 mismatch"};
    if (sha256(script) != manifestHash(manifest, "operations.tsv"))
        throw std::runtime_error{"operations.tsv SHA-256 mismatch"};
}

[[nodiscard]] std::vector<ssg::CellRun> firstViewportRuns(
    std::string_view text) {
    std::vector<ssg::CellRun> runs;
    std::size_t begin = 0;
    while (runs.size() < 80 && begin < text.size()) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        runs.push_back(ssg::GraphemeLayout{}.computeRun(text.substr(begin, end - begin)));
        begin = end + (end < text.size() ? 1U : 0U);
    }
    return runs;
}

void verifyCorrectness(std::string const& base,
                        std::vector<Operation> const& operations,
                        Timings& timings) {
    if (base.size() != kTargetBytes)
        throw std::runtime_error{"expanded corpus is not exactly 10 MiB"};
    if (operations.size() != kOperationCount)
        throw std::runtime_error{"operation script must contain exactly 10000 operations"};

    ssg::Document document{base};
    for (auto const& operation : operations) {
        auto result = apply(document, operation);
        if (!result.accepted())
            throw std::runtime_error{"operation script rejected: " + result.message};
    }
    if (document.snapshot().text != base)
        throw std::runtime_error{"operation script does not restore canonical text"};

    auto runs = firstViewportRuns(base);
    auto view = ssg::Viewport{}.compute(runs, ssg::ViewportDimensions{120, 40});
    auto unchanged = ssg::Viewport{}.deriveDelta(view, view);
    if (unchanged.changed || unchanged.replacement.has_value())
        throw std::runtime_error{"unchanged viewport emitted a payload"};

    ssg::CommandExecutor idle{std::make_shared<ssg::CommandCatalog>()};
    double const cpuStart = processCpuMilliseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    timings.idleCpuMilliseconds = processCpuMilliseconds() - cpuStart;
    if (timings.idleCpuMilliseconds > 10.0)
        throw std::runtime_error{"idle session consumed polling CPU"};
}

void measureEdits(std::string const& base,
                   std::vector<Operation> const& operations,
                   Timings& timings) {
    for (std::size_t repetition = 0; repetition < kRepetitions; ++repetition) {
        ssg::Document document{base};
        for (std::size_t i = 0; i < operations.size(); ++i) {
            auto const start = Clock::now();
            auto result = apply(document, operations[i]);
            auto const elapsed = Clock::now() - start;
            if (!result.accepted()) throw std::runtime_error{result.message};
            if (i >= kWarmupCount)
                timings.editMicroseconds.push_back(
                    std::chrono::duration<double, std::micro>(elapsed).count());
        }
        if (document.snapshot().text != base)
            throw std::runtime_error{"edit repetition diverged"};
    }
}

void measureCommandDelta(std::vector<Operation> const& operations,
                           Timings& timings) {
    std::string const initial(16U * 1024U, 'a');
    for (std::size_t repetition = 0; repetition < kRepetitions; ++repetition) {
        ssg::Document document{initial};
        auto catalog = std::make_shared<ssg::CommandCatalog>();
        catalog->add(ssg::CommandSpecBuilder{"benchmark.edit"}
                         .owner("benchmark")
                         .summary("Apply one benchmark edit")
                         .mutates()
                         .inProcessHandler<Operation>(
                             [&document](ssg::CommandContext&,
                                         Operation const& operation) {
                                 auto result = apply(document, operation);
                                 return result.accepted()
                                            ? ssg::CommandHandlerResult::success()
                                            : ssg::CommandHandlerResult::failure(
                                                  result.message);
                             }));
        ssg::CommandExecutor session{catalog};
        ssg::InvocationPrincipal const principal{
            ssg::ClientId{1}, ssg::InvocationOrigin::InProcess};
        if (!session.attach(principal, ssg::ViewId{1}).accepted())
            throw std::runtime_error{"benchmark client attach failed"};
        ssg::DocumentViewState view{
            document.revision(), initial, ssg::ByteOffset{0}};

        for (std::size_t i = 0; i < operations.size(); ++i) {
            Operation normalized = operations[i];
            normalized.offset = 0;
            auto const start = Clock::now();
            auto result = session.dispatch(
                principal.clientId(),
                {"benchmark.edit", session.revision(), normalized});
            auto snapshot = document.snapshot();
            ssg::DocumentViewState after{
                snapshot.revision, std::move(snapshot.text),
                ssg::ByteOffset{normalized.insert ? 1U : 0U}};
            auto delta = ssg::DocumentSnapshotCodec{}.deriveDelta(view, after);
            auto const elapsed = Clock::now() - start;
            if (!result.accepted() || !delta.has_value())
                throw std::runtime_error{"command-to-delta cycle failed"};
            view = std::move(after);
            if (i >= kWarmupCount)
                timings.commandDeltaMicroseconds.push_back(
                    std::chrono::duration<double, std::micro>(elapsed).count());
        }
        if (view.text != initial)
            throw std::runtime_error{"command repetition diverged"};
    }
}

void measureOpenViewport(std::string const& base, Timings& timings) {
    for (std::size_t repetition = 0; repetition < kRepetitions; ++repetition) {
        auto const start = Clock::now();
        ssg::Document document{base};
        auto snapshot = document.snapshot();
        auto runs = firstViewportRuns(snapshot.text);
        auto view =
            ssg::Viewport{}.compute(runs, ssg::ViewportDimensions{120, 40});
        if (view.visibleRows.empty())
            throw std::runtime_error{"first viewport is empty"};
        timings.openViewportMilliseconds.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start)
                .count());
    }
}

[[nodiscard]] double percentile(std::vector<double> values, double p) {
    if (values.empty()) throw std::runtime_error{"empty timing sample"};
    std::sort(values.begin(), values.end());
    auto const index = static_cast<std::size_t>(
        std::ceil(p * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

void printReport(Timings const& timings) {
    std::cout << std::fixed << std::setprecision(3)
              << "provenance platform="
#ifdef _WIN32
              << "windows"
#else
              << "linux"
#endif
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " compiler=\"" << SSG_BENCHMARK_COMPILER << "\""
              << " build_type=\"" << SSG_BENCHMARK_BUILD_TYPE << "\""
              << " flags=\"" << SSG_BENCHMARK_BUILD_FLAGS << "\"\n"
              << "protocol operations=10000 warmup=1000 repetitions=5"
                 " aggregate_samples="
              << timings.editMicroseconds.size() << '\n'
              << "edit_us p50=" << percentile(timings.editMicroseconds, 0.50)
              << " p99=" << percentile(timings.editMicroseconds, 0.99) << '\n'
              << "command_delta_us p50="
              << percentile(timings.commandDeltaMicroseconds, 0.50)
              << " p99="
              << percentile(timings.commandDeltaMicroseconds, 0.99) << '\n'
              << "open_viewport_ms max="
              << *std::max_element(timings.openViewportMilliseconds.begin(),
                                   timings.openViewportMilliseconds.end())
              << '\n'
              << "idle_cpu_ms=" << timings.idleCpuMilliseconds << '\n';
}

void enforce(Timings const& timings) {
    if (percentile(timings.editMicroseconds, 0.50) >= 1'000.0 ||
        percentile(timings.editMicroseconds, 0.99) >= 4'000.0)
        throw std::runtime_error{"edit latency budget exceeded"};
    if (percentile(timings.commandDeltaMicroseconds, 0.50) >= 2'000.0 ||
        percentile(timings.commandDeltaMicroseconds, 0.99) >= 8'000.0)
        throw std::runtime_error{"command-to-delta latency budget exceeded"};
    if (*std::max_element(timings.openViewportMilliseconds.begin(),
                          timings.openViewportMilliseconds.end()) >= 250.0)
        throw std::runtime_error{"10 MiB first-viewport budget exceeded"};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        bool const verifyOnly =
            argc == 2 && std::string_view{argv[1]} == "--verify-only";
        bool const enforceLimits =
            argc == 2 && std::string_view{argv[1]} == "--enforce";
        if (argc > 2 || (argc == 2 && !verifyOnly && !enforceLimits))
            throw std::runtime_error{"usage: editor_benchmark [--verify-only|--enforce]"};

        auto const seed = readFile(SSG_PERFORMANCE_CORPUS);
        auto const script = readFile(SSG_PERFORMANCE_OPERATIONS);
        verifyInputs(seed, script);
        auto const operations = loadOperations(SSG_PERFORMANCE_OPERATIONS);
        auto const base = expandCorpus(seed);
        Timings timings;
        verifyCorrectness(base, operations, timings);
        if (verifyOnly) {
            std::cout << "performance correctness verified\n";
            return 0;
        }
        measureEdits(base, operations, timings);
        measureCommandDelta(operations, timings);
        measureOpenViewport(base, timings);
        printReport(timings);
        if (enforceLimits) enforce(timings);
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "editor_benchmark: " << error.what() << '\n';
        return 1;
    }
}
