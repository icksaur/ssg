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

constexpr std::size_t repetitions = 12;
constexpr std::size_t discard = 2;  // Discard the first launches (page-in, JIT of caches).

// The ordered cold-start phases, and the human-readable spans between them.
constexpr std::array<char const*, 5> phase_marks{
    "main_entry", "post_create", "post_attach", "post_open",
    "first_content_frame"};

[[nodiscard]] long long monotonic_ns() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
}

// One launch's marks, keyed by phase name, in monotonic nanoseconds.
struct Trace {
    long long t0 = 0;  // Parent's pre-fork timestamp (the exec start proxy).
    std::map<std::string, long long> marks;

    [[nodiscard]] bool complete() const {
        for (auto const* phase : phase_marks) {
            if (!marks.contains(phase)) return false;
        }
        return true;
    }
};

[[nodiscard]] Trace read_trace(fs::path const& path, long long t0) {
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
void drain_nonblocking(int fd) {
    char buffer[4096];
    for (;;) {
        ssize_t const n = ::read(fd, buffer, sizeof buffer);
        if (n <= 0) break;  // EAGAIN / EWOULDBLOCK / EOF: nothing more right now.
    }
}

// True once the child has exited (reaps it if so).  Lets the poll loops notice a
// child that died (e.g. execl failure -> _exit(127)) instead of waiting out the
// full deadline.
[[nodiscard]] bool child_exited(pid_t pid, int& status) {
    return ::waitpid(pid, &status, WNOHANG) == pid;
}

// Run the probe once under a pty with the file argument; return its trace, or
// nullopt on failure.  Kills the child once the first content frame is recorded.
[[nodiscard]] std::optional<Trace> run_once(std::string const& probe,
                                            std::string const& file_arg,
                                            std::string const& cwd) {
    auto trace_path =
        fs::temp_directory_path() /
        ("ssg-startup-" + std::to_string(::getpid()) + "-" +
         std::to_string(monotonic_ns()) + ".trace");
    std::error_code ec;
    fs::remove(trace_path, ec);

    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    long long const t0 = monotonic_ns();
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return std::nullopt;
    if (pid == 0) {
        // Child: the pty slave is stdin/stdout.  Point the probe at the trace
        // file and run it against the fixture.
        setenv("SSG_STARTUP_TRACE", trace_path.c_str(), 1);
        setenv("TERM", "xterm-256color", 1);
        if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) _exit(127); }
        execl(probe.c_str(), probe.c_str(), file_arg.c_str(),
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
    bool child_dead = false;
    int child_status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{master, POLLIN, 0};
        ::poll(&pfd, 1, 5);
        drain_nonblocking(master);
        auto trace = read_trace(trace_path, t0);
        if (trace.complete()) { done = true; break; }
        if (child_exited(pid, child_status)) { child_dead = true; break; }
    }

    if (!child_dead) {
        ::kill(pid, SIGTERM);
        ::waitpid(pid, &child_status, 0);
    }
    ::close(master);

    if (!done) { fs::remove(trace_path, ec); return std::nullopt; }
    auto trace = read_trace(trace_path, t0);
    fs::remove(trace_path, ec);
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
    std::vector<double> samples_ms;
};

// A phase span's start/end mark, and the special total from t0.
void accumulate(std::vector<Trace> const& traces,
                std::vector<SpanStats>& spans) {
    auto ms = [](long long ns) { return static_cast<double>(ns) / 1'000'000.0; };
    for (auto const& trace : traces) {
        // spawn+exec+link: t0 -> main_entry.
        spans[0].samples_ms.push_back(ms(trace.marks.at("main_entry") - trace.t0));
        // Between consecutive marks.
        for (std::size_t i = 1; i < phase_marks.size(); ++i) {
            spans[i].samples_ms.push_back(
                ms(trace.marks.at(phase_marks[i]) - trace.marks.at(phase_marks[i - 1])));
        }
        // Total: t0 -> first_content_frame.
        spans[phase_marks.size()].samples_ms.push_back(
            ms(trace.marks.at("first_content_frame") - trace.t0));
    }
}

struct Fixture {
    std::string name;
    std::string cwd;
    std::string file_arg;
};

// Build the fixtures under `root`: a small file, a 10 MiB file, and a deep tree.
[[nodiscard]] std::vector<Fixture> make_fixtures(fs::path const& root) {
    fs::create_directories(root);

    auto small_dir = root / "small";
    fs::create_directories(small_dir);
    {
        std::ofstream out{small_dir / "note.txt", std::ios::binary};
        for (int i = 0; i < 40; ++i) out << "the quick brown fox jumps over the lazy dog\n";
    }

    auto big_dir = root / "big";
    fs::create_directories(big_dir);
    {
        std::ofstream out{big_dir / "big.txt", std::ios::binary};
        std::string line(80, 'a');
        line.push_back('\n');
        std::size_t written = 0;
        constexpr std::size_t target = 10U * 1024U * 1024U;
        while (written < target) { out << line; written += line.size(); }
    }

    auto tree_dir = root / "tree";
    for (int d = 0; d < 40; ++d) {
        auto sub = tree_dir / ("dir_" + std::to_string(d));
        fs::create_directories(sub);
        for (int f = 0; f < 40; ++f) {
            std::ofstream out{sub / ("file_" + std::to_string(f) + ".txt"),
                              std::ios::binary};
            out << "content\n";
        }
    }
    {
        std::ofstream out{tree_dir / "root.txt", std::ios::binary};
        out << "root\n";
    }

    return {
        {"small_file", small_dir.string(), "note.txt"},
        {"big_10MiB", big_dir.string(), "big.txt"},
        {"deep_tree", tree_dir.string(), "root.txt"},
    };
}

void report_fixture(std::ostream& out, Fixture const& fixture,
                    std::vector<Trace> const& traces) {
    std::vector<SpanStats> spans{
        {"spawn_exec_link", {}}, {"construct", {}}, {"attach", {}},
        {"file_open", {}},       {"first_frame", {}}, {"total_exec_to_frame", {}}};
    accumulate(traces, spans);
    out << "fixture " << fixture.name << " samples=" << traces.size() << '\n';
    for (auto const& span : spans) {
        out << std::fixed << std::setprecision(3) << "  " << std::left
            << std::setw(22) << span.name
            << " p50=" << percentile(span.samples_ms, 0.50) << "ms"
            << " p99=" << percentile(span.samples_ms, 0.99) << "ms\n";
    }
}

// Run `binary file_arg` under a pty with the trace env set; report whether a
// trace file appeared and whether the child failed to exec (exit 127).  Shared
// by --verify-clean's negative case and its positive control.
struct TraceProbeResult {
    bool trace_written = false;
    bool exec_failed = false;
};

[[nodiscard]] TraceProbeResult run_trace_probe(std::string const& binary,
                                               fs::path const& cwd,
                                               std::string const& file_arg) {
    auto trace_path = cwd / "startup-probe.trace";
    std::error_code ec;
    fs::remove(trace_path, ec);

    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) { std::cerr << "forkpty failed\n"; return {false, true}; }
    if (pid == 0) {
        setenv("SSG_STARTUP_TRACE", trace_path.c_str(), 1);
        setenv("TERM", "xterm-256color", 1);
        if (chdir(cwd.c_str()) != 0) _exit(127);
        execl(binary.c_str(), binary.c_str(), file_arg.c_str(),
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
        drain_nonblocking(master);
        if (fs::exists(trace_path)) break;
        if (child_exited(pid, status)) { dead = true; break; }
    }
    bool const trace_written = fs::exists(trace_path);
    bool const exec_failed = dead && WIFEXITED(status) && WEXITSTATUS(status) == 127;
    if (!dead) { ::kill(pid, SIGTERM); ::waitpid(pid, &status, 0); }
    ::close(master);
    fs::remove(trace_path, ec);
    return {trace_written, exec_failed};
}

// --verify-clean: prove the SHIPPED (uninstrumented) ssg has the instrumentation
// compiled out.  A positive control (the instrumented probe) must write a trace
// under the identical env/pty setup, so a missing trace from the clean binary is
// meaningful (the setup works) rather than a false pass; both binaries must
// actually exec.
[[nodiscard]] int verify_clean(std::string const& clean_binary,
                               std::string const& probe_binary) {
    auto root = fs::temp_directory_path() /
                ("ssg-verify-clean-" + std::to_string(::getpid()));
    fs::create_directories(root);
    { std::ofstream out{root / "note.txt"}; out << "hi\n"; }

    auto const control = run_trace_probe(probe_binary, root, "note.txt");
    if (control.exec_failed) {
        std::cerr << "FAIL: could not exec the instrumented probe\n";
        std::error_code ec; fs::remove_all(root, ec);
        return 1;
    }
    if (!control.trace_written) {
        std::cerr << "FAIL: positive control did not write a trace; the harness "
                     "setup cannot capture one, so the clean check is meaningless\n";
        std::error_code ec; fs::remove_all(root, ec);
        return 1;
    }

    auto const clean = run_trace_probe(clean_binary, root, "note.txt");
    std::error_code ec;
    fs::remove_all(root, ec);
    if (clean.exec_failed) {
        std::cerr << "FAIL: could not exec the shipped ssg\n";
        return 1;
    }
    if (clean.trace_written) {
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

[[nodiscard]] double total_exec_p99(std::vector<Trace> const& traces) {
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
[[nodiscard]] std::map<std::string, std::vector<Trace>> measure_fixtures(
    std::string const& probe, std::vector<Fixture> const& fixtures) {
    std::map<std::string, std::vector<Trace>> result;
    for (auto const& fixture : fixtures) {
        std::vector<Trace> traces;
        for (std::size_t rep = 0; rep < repetitions; ++rep) {
            auto trace = run_once(probe, fixture.file_arg, fixture.cwd);
            if (trace && rep >= discard) traces.push_back(*trace);
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
        return verify_clean(clean, probe);
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
    auto fixtures = make_fixtures(root);
    auto measured = measure_fixtures(probe, fixtures);

    std::ostringstream report;
    report << "provenance platform=linux compiler=\"" << SSG_BENCHMARK_COMPILER
           << "\" build_type=\"" << SSG_BENCHMARK_BUILD_TYPE << "\" flags=\""
           << SSG_BENCHMARK_BUILD_FLAGS << "\"\n"
           << "protocol repetitions=" << repetitions << " discard=" << discard
           << " cache=warm winsize=80x24 clock=CLOCK_MONOTONIC\n";

    for (auto const& fixture : fixtures) {
        auto const& traces = measured.at(fixture.name);
        if (traces.empty()) {
            report << "fixture " << fixture.name << " FAILED to produce traces\n";
            continue;
        }
        report_fixture(report, fixture, traces);
    }

    fs::remove_all(root, ec);

    std::cout << report.str();
    if (char const* out_path = std::getenv("SSG_STARTUP_REPORT")) {
        std::ofstream out{out_path};
        out << report.str();
        std::cout << "baseline report written to " << out_path << '\n';
    }

    if (enforce) {
        auto const it = measured.find("small_file");
        if (it == measured.end() || it->second.empty()) {
            std::cerr << "enforce: small_file produced no traces\n";
            return 1;
        }
        double const p99 = total_exec_p99(it->second);
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
