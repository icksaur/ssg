#include <ssg/capabilities_report.h>

#include <ssg/fd_readiness.h>
#include <ssg/ssg_terminal.h>

#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace ssg::app {

int reportCapabilities() {
    TerminalCapabilities capabilities{
        [](std::string_view name) { return std::getenv(std::string{name}.c_str()); }};

    termios original{};
    bool raw = false;
    if (tcgetattr(STDIN_FILENO, &original) == 0) {
        termios probe = original;
        probe.c_lflag &= ~(ICANON | ECHO);
        probe.c_cc[VMIN] = 0;
        probe.c_cc[VTIME] = 0;
        raw = tcsetattr(STDIN_FILENO, TCSAFLUSH, &probe) == 0;
    }
    if (raw) {
        writeAll(capabilities.beginProbe());
        auto const deadline = std::chrono::steady_clock::now() +
                              TerminalCapabilities::kProbeWindow;
        std::string buffer;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!waitReadiness(10, -1, -1).input) continue;
            char bytes[256];
            auto const count = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (count <= 0) continue;
            buffer.append(bytes, static_cast<std::size_t>(count));
            while (!buffer.empty()) {
                std::size_t consumed = 0;
                auto const decoded = decode_input(buffer, true, consumed);
                if (decoded.status == DecodeStatus::incomplete || consumed == 0) {
                    break;
                }
                if (decoded.status == DecodeStatus::reply) {
                    capabilities.observeReply(decoded.reply);
                }
                buffer.erase(0, consumed);
            }
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    }

    char const* const term = std::getenv("TERM");
    std::printf("TERM=%s\n", term == nullptr ? "(unset)" : term);
    std::printf("%-24s %s\n", "color_depth",
                color_depth_name(capabilities.colorDepth()).data());
    for (auto const capability : kAllCapabilities) {
        std::printf("%-24s %s\n", std::string{capability_name(capability)}.c_str(),
                    capabilities.has(capability) ? "yes" : "no");
    }
    if (!raw) {
        std::printf(
            "\nstdin is not a terminal, so nothing was asked: every queried\n"
            "capability above reports its default.\n");
    }

    auto const visible = [](std::string_view bytes) {
        std::string shown;
        for (unsigned char const byte : bytes) {
            if (byte == 0x1b) {
                shown += "<ESC>";
            } else if (byte < 0x20 || byte == 0x7f) {
                shown += '.';
            } else {
                shown += static_cast<char>(byte);
            }
        }
        return shown;
    };
    auto const& log = capabilities.probeLog();
    if (raw) {
        std::printf("\nreplies (%zu):\n", log.believed.size());
        for (auto const& reply : log.believed) {
            std::printf("  %s\n", visible(reply).c_str());
        }
        if (log.believed.empty()) {
            std::printf("  (none -- the terminal answered nothing at all)\n");
        }
        if (!log.ignored.empty()) {
            std::printf(
                "\nreplies that arrived AFTER the fence closed the window, and\n"
                "were therefore not believed (%zu):\n",
                log.ignored.size());
            for (auto const& reply : log.ignored) {
                std::printf("  %s\n", visible(reply).c_str());
            }
        }
    }
    std::printf(
        "\nOverride any answer with SSG_TERM_<NAME>=on|off (for example\n"
        "SSG_TERM_SYNCHRONIZED_OUTPUT=off), or the color depth with\n"
        "SSG_COLOR_DEPTH.  An override always beats what the terminal reports.\n");
    return 0;
}

}  // namespace ssg::app
