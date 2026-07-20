// M10-1 startup measurement harness (doc/spec-fast-startup.md).
//
// Launches the instrumented `ssg` probe under a pseudo-terminal, once per
// repetition, and attributes exec->first-content-frame wall-clock time to the
// cold-start phases the probe records to a trace file (main_entry, post_create,
// post_attach, post_open, first_content_frame).  The parent's pre-fork
// CLOCK_MONOTONIC timestamp and the child's marks share the system-wide
// monotonic clock, so exec+spawn+link time is (main_entry - t0).
//
// This produces a BASELINE report (no pass/fail threshold): the wall-clock
// budget is pinned later (M10-5) from these numbers.  The measured `ssg` binary
// ships without the instrumentation (compiled out unless
// SSG_STARTUP_TRACE_ENABLED); the harness proves that with --verify-clean.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pty.h>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kRepetitions = 12;
constexpr std::size_t kDiscard = 2;  // Discard the first launches (page-in, JIT of caches).

// The ordered cold-start phases, and the human-readable spans between them.
constexpr std::array<char const*, 5> kPhaseMarks{
    "main_entry", "post_create", "post_attach", "post_open",
    "first_content_frame"};

[[nodiscard]] long long monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
}

// One launch's marks, keyed by phase name, in monotonic nanoseconds.
struct Trace {
    long long t0 = 0;  // Parent's pre-fork timestamp (the exec start proxy).
    std::map<std::string, long long> marks;

    [[nodiscard]] bool complete() const {
        for (auto const* phase : kPhaseMarks) {
            if (!marks.contains(phase)) return false;
        }
        return true;
    }
};

[[nodiscard]] Trace readTrace(fs::path const& path, long long t0) {
    Trace trace;
    trace.t0 = t0;
    std::ifstream input{path};
    std::string phase;
    long long ns = 0;
    while (input >> phase >> ns) trace.marks[phase] = ns;
    return trace;
}

// Drain whatever is currently readable from a NON-BLOCKING master fd, discarding
// the bytes (timing comes from the trace file).  Never blocks, so a child that
// hangs before writing anything cannot stall the harness past its deadline.
void drainNonblocking(int fd) {
    char buffer[4096];
    for (;;) {
        ssize_t const n = ::read(fd, buffer, sizeof buffer);
        if (n <= 0) break;  // EAGAIN / EWOULDBLOCK / EOF: nothing more right now.
    }
}

// True once the child has exited (reaps it if so).  Lets the poll loops notice a
// child that died (e.g. execl failure -> _exit(127)) instead of waiting out the
// full deadline.
[[nodiscard]] bool childExited(pid_t pid, int& status) {
    return ::waitpid(pid, &status, WNOHANG) == pid;
}

// Run the probe once under a pty with the file argument; return its trace, or
// nullopt on failure.  Kills the child once the first content frame is recorded.
[[nodiscard]] std::optional<Trace> runOnce(std::string const& probe,
                                            std::string const& fileArg,
                                            std::string const& cwd) {
    auto tracePath =
        fs::temp_directory_path() /
        ("ssg-startup-" + std::to_string(::getpid()) + "-" +
         std::to_string(monotonicNs()) + ".trace");
    std::error_code ec;
    fs::remove(tracePath, ec);

    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    long long const t0 = monotonicNs();
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return std::nullopt;
    if (pid == 0) {
        // Child: the pty slave is stdin/stdout.  Point the probe at the trace
        // file and run it against the fixture.
        setenv("SSG_STARTUP_TRACE", tracePath.c_str(), 1);
        setenv("TERM", "xterm-256color", 1);
        if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) _exit(127); }
        execl(probe.c_str(), probe.c_str(), fileArg.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    // Parent: the master fd is non-blocking so a child that hangs before writing
    // anything cannot stall us; drain readable bytes (so the child never blocks
    // on a full pty buffer) while polling the trace file for the sentinel, and
    // bail early if the child dies (e.g. execl failure).
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    bool done = false;
    bool childDead = false;
    int childStatus = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{master, POLLIN, 0};
        ::poll(&pfd, 1, 5);
        drainNonblocking(master);
        auto trace = readTrace(tracePath, t0);
        if (trace.complete()) { done = true; break; }
        if (childExited(pid, childStatus)) { childDead = true; break; }
    }

    if (!childDead) {
        ::kill(pid, SIGTERM);
        ::waitpid(pid, &childStatus, 0);
    }
    ::close(master);

    if (!done) { fs::remove(tracePath, ec); return std::nullopt; }
    auto trace = readTrace(tracePath, t0);
    fs::remove(tracePath, ec);
    if (!trace.complete()) return std::nullopt;
    return trace;
}

[[nodiscard]] double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    auto const index = static_cast<std::size_t>(
        std::ceil(p * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

struct SpanStats {
    std::string name;
    std::vector<double> samplesMs;
};

// A phase span's start/end mark, and the special total from t0.
void accumulate(std::vector<Trace> const& traces,
                std::vector<SpanStats>& spans) {
    auto ms = [](long long ns) { return static_cast<double>(ns) / 1'000'000.0; };
    for (auto const& trace : traces) {
        // spawn+exec+link: t0 -> main_entry.
        spans[0].samplesMs.push_back(ms(trace.marks.at("main_entry") - trace.t0));
        // Between consecutive marks.
        for (std::size_t i = 1; i < kPhaseMarks.size(); ++i) {
            spans[i].samplesMs.push_back(
                ms(trace.marks.at(kPhaseMarks[i]) - trace.marks.at(kPhaseMarks[i - 1])));
        }
        // Total: t0 -> first_content_frame.
        spans[kPhaseMarks.size()].samplesMs.push_back(
            ms(trace.marks.at("first_content_frame") - trace.t0));
    }
}

struct Fixture {
    std::string name;
    std::string cwd;
    std::string fileArg;
};

// Build the fixtures under `root`: a small file, a 10 MiB file, and a deep tree.
[[nodiscard]] std::vector<Fixture> makeFixtures(fs::path const& root) {
    fs::create_directories(root);

    auto smallDir = root / "small";
    fs::create_directories(smallDir);
    {
        std::ofstream out{smallDir / "note.txt", std::ios::binary};
        for (int i = 0; i < 40; ++i) out << "the quick brown fox jumps over the lazy dog\n";
    }

    auto bigDir = root / "big";
    fs::create_directories(bigDir);
    {
        std::ofstream out{bigDir / "big.txt", std::ios::binary};
        std::string line(80, 'a');
        line.push_back('\n');
        std::size_t written = 0;
        constexpr std::size_t target = 10U * 1024U * 1024U;
        while (written < target) { out << line; written += line.size(); }
    }

    auto treeDir = root / "tree";
    for (int d = 0; d < 40; ++d) {
        auto sub = treeDir / ("dir_" + std::to_string(d));
        fs::create_directories(sub);
        for (int f = 0; f < 40; ++f) {
            std::ofstream out{sub / ("file_" + std::to_string(f) + ".txt"),
                              std::ios::binary};
            out << "content\n";
        }
    }
    {
        std::ofstream out{treeDir / "root.txt", std::ios::binary};
        out << "root\n";
    }

    return {
        {"small_file", smallDir.string(), "note.txt"},
        {"big_10MiB", bigDir.string(), "big.txt"},
        {"deep_tree", treeDir.string(), "root.txt"},
    };
}

void reportFixture(std::ostream& out, Fixture const& fixture,
                    std::vector<Trace> const& traces) {
    std::vector<SpanStats> spans{
        {"spawn_exec_link", {}}, {"construct", {}}, {"attach", {}},
        {"file_open", {}},       {"first_frame", {}}, {"total_exec_to_frame", {}}};
    accumulate(traces, spans);
    out << "fixture " << fixture.name << " samples=" << traces.size() << '\n';
    for (auto const& span : spans) {
        out << std::fixed << std::setprecision(3) << "  " << std::left
            << std::setw(22) << span.name
            << " p50=" << percentile(span.samplesMs, 0.50) << "ms"
            << " p99=" << percentile(span.samplesMs, 0.99) << "ms\n";
    }
}

// Run `binary file_arg` under a pty with the trace env set; report whether a
// trace file appeared and whether the child failed to exec (exit 127).  Shared
// by --verify-clean's negative case and its positive control.
struct TraceProbeResult {
    bool traceWritten = false;
    bool execFailed = false;
};

[[nodiscard]] TraceProbeResult runTraceProbe(std::string const& binary,
                                               fs::path const& cwd,
                                               std::string const& fileArg) {
    auto tracePath = cwd / "startup-probe.trace";
    std::error_code ec;
    fs::remove(tracePath, ec);

    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) { std::cerr << "forkpty failed\n"; return {false, true}; }
    if (pid == 0) {
        setenv("SSG_STARTUP_TRACE", tracePath.c_str(), 1);
        setenv("TERM", "xterm-256color", 1);
        if (chdir(cwd.c_str()) != 0) _exit(127);
        execl(binary.c_str(), binary.c_str(), fileArg.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    // Give the child time to reach (or pass) the first frame, draining output.
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    int status = 0;
    bool dead = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{master, POLLIN, 0};
        ::poll(&pfd, 1, 20);
        drainNonblocking(master);
        if (fs::exists(tracePath)) break;
        if (childExited(pid, status)) { dead = true; break; }
    }
    bool const traceWritten = fs::exists(tracePath);
    bool const execFailed = dead && WIFEXITED(status) && WEXITSTATUS(status) == 127;
    if (!dead) { ::kill(pid, SIGTERM); ::waitpid(pid, &status, 0); }
    ::close(master);
    fs::remove(tracePath, ec);
    return {traceWritten, execFailed};
}

// --verify-clean: prove the SHIPPED (uninstrumented) ssg has the instrumentation
// compiled out.  A positive control (the instrumented probe) must write a trace
// under the identical env/pty setup, so a missing trace from the clean binary is
// meaningful (the setup works) rather than a false pass; both binaries must
// actually exec.
[[nodiscard]] int verifyClean(std::string const& cleanBinary,
                               std::string const& probeBinary) {
    auto root = fs::temp_directory_path() /
                ("ssg-verify-clean-" + std::to_string(::getpid()));
    fs::create_directories(root);
    { std::ofstream out{root / "note.txt"}; out << "hi\n"; }

    auto const control = runTraceProbe(probeBinary, root, "note.txt");
    if (control.execFailed) {
        std::cerr << "FAIL: could not exec the instrumented probe\n";
        std::error_code ec; fs::remove_all(root, ec);
        return 1;
    }
    if (!control.traceWritten) {
        std::cerr << "FAIL: positive control did not write a trace; the harness "
                     "setup cannot capture one, so the clean check is meaningless\n";
        std::error_code ec; fs::remove_all(root, ec);
        return 1;
    }

    auto const clean = runTraceProbe(cleanBinary, root, "note.txt");
    std::error_code ec;
    fs::remove_all(root, ec);
    if (clean.execFailed) {
        std::cerr << "FAIL: could not exec the shipped ssg\n";
        return 1;
    }
    if (clean.traceWritten) {
        std::cerr << "FAIL: shipped ssg wrote a startup trace; instrumentation "
                     "is NOT compiled out\n";
        return 1;
    }
    std::cout << "verify-clean: instrumented probe traces, shipped ssg does not "
                 "(instrumentation compiled out) OK\n";
    return 0;
}

// The exec->first-frame ceiling for the small cache-warm fixture, enforced only
// under --enforce on the designated benchmark host (per doc/spec-fast-startup.md
// M10-5).  Generous relative to the measured baseline (single-digit ms) so it is
// a gross-regression tripwire, not a host-tight gate; the real number is tuned on
// the bench host.  The 10 MiB fixture is intentionally NOT gated here — its cost
// is the O(document) work deferred to Milestone 12.
constexpr double kSmallFileBudgetMs = 250.0;

[[nodiscard]] double totalExecP99(std::vector<Trace> const& traces) {
    std::vector<double> samples;
    samples.reserve(traces.size());
    for (auto const& trace : traces) {
        samples.push_back(
            static_cast<double>(trace.marks.at("first_content_frame") - trace.t0) /
            1'000'000.0);
    }
    return percentile(std::move(samples), 0.99);
}

// Measure every fixture; returns per-fixture kept traces (post-discard).
[[nodiscard]] std::map<std::string, std::vector<Trace>> measureFixtures(
    std::string const& probe, std::vector<Fixture> const& fixtures) {
    std::map<std::string, std::vector<Trace>> result;
    for (auto const& fixture : fixtures) {
        std::vector<Trace> traces;
        for (std::size_t rep = 0; rep < kRepetitions; ++rep) {
            auto trace = runOnce(probe, fixture.fileArg, fixture.cwd);
            if (trace && rep >= kDiscard) traces.push_back(*trace);
        }
        result.emplace(fixture.name, std::move(traces));
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    std::string const probe = SSG_STARTUP_PROBE_BINARY;
    std::string const clean = SSG_STARTUP_CLEAN_BINARY;

    if (argc == 2 && std::string_view{argv[1]} == "--verify-clean") {
        return verifyClean(clean, probe);
    }
    bool const enforce =
        argc == 2 && std::string_view{argv[1]} == "--enforce";
    if (argc > 1 && !enforce) {
        std::cerr << "usage: startup_benchmark [--verify-clean|--enforce]\n";
        return 1;
    }

    auto root = fs::temp_directory_path() /
                ("ssg-startup-fixtures-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    auto fixtures = makeFixtures(root);
    auto measured = measureFixtures(probe, fixtures);

    std::ostringstream report;
    report << "provenance platform=linux compiler=\"" << SSG_BENCHMARK_COMPILER
           << "\" build_type=\"" << SSG_BENCHMARK_BUILD_TYPE << "\" flags=\""
           << SSG_BENCHMARK_BUILD_FLAGS << "\"\n"
           << "protocol repetitions=" << kRepetitions << " discard=" << kDiscard
           << " cache=warm winsize=80x24 clock=CLOCK_MONOTONIC\n";

    for (auto const& fixture : fixtures) {
        auto const& traces = measured.at(fixture.name);
        if (traces.empty()) {
            report << "fixture " << fixture.name << " FAILED to produce traces\n";
            continue;
        }
        reportFixture(report, fixture, traces);
    }

    fs::remove_all(root, ec);

    std::cout << report.str();
    if (char const* outPath = std::getenv("SSG_STARTUP_REPORT")) {
        std::ofstream out{outPath};
        out << report.str();
        std::cout << "baseline report written to " << outPath << '\n';
    }

    if (enforce) {
        auto const it = measured.find("small_file");
        if (it == measured.end() || it->second.empty()) {
            std::cerr << "enforce: small_file produced no traces\n";
            return 1;
        }
        double const p99 = totalExecP99(it->second);
        if (p99 >= kSmallFileBudgetMs) {
            std::cerr << "enforce: small_file exec->first-frame p99 " << p99
                      << "ms exceeds budget " << kSmallFileBudgetMs << "ms\n";
            return 1;
        }
        std::cout << "enforce: small_file exec->first-frame p99 " << p99
                  << "ms within budget " << kSmallFileBudgetMs << "ms OK\n";
    }
    return 0;
}
