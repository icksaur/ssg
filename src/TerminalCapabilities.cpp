#include <ssg/TerminalCapabilities.h>

#include <ssg/TerminalOutput.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace ssg {
namespace {

std::string lowercase(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

bool matchesAny(std::string_view value,
                std::initializer_list<std::string_view> options) {
    return std::ranges::any_of(options, [&](std::string_view option) {
        return value == option;
    });
}

} // namespace

std::string_view capabilityName(Capability capability) {
    switch (capability) {
    case Capability::SynchronizedOutput: return "synchronized_output";
    case Capability::KeyboardProtocol: return "keyboard_protocol";
    case Capability::ClipboardWrite: return "clipboard_write";
    }
    return "unknown";
}

namespace {

// A report's parts: the private-prefix flag, its numeric parameters, its
// intermediate byte if any, and its final byte.  Parsing is separated from
// interpretation so each answer below reads as the protocol rule it encodes.
struct ReplyParts {
    bool privatePrefix = false;
    std::vector<std::int64_t> params;
    char intermediate = '\0';
    char final = '\0';
};

std::optional<ReplyParts> parseReply(std::string_view reply) {
    if (reply.size() < 3 || reply[0] != 0x1b || reply[1] != '[') return std::nullopt;
    ReplyParts parts;
    std::size_t index = 2;
    if (reply[index] == '?') {
        parts.privatePrefix = true;
        ++index;
    }
    // Parameters are decimal runs separated by ';'.  An empty run is a defaulted
    // parameter and is reported as such rather than skipped, so positions hold.
    std::int64_t current = 0;
    bool anyDigit = false;
    for (; index < reply.size(); ++index) {
        auto const byte = static_cast<unsigned char>(reply[index]);
        if (byte >= '0' && byte <= '9') {
            if (current < 1'000'000) current = current * 10 + (byte - '0');
            anyDigit = true;
            continue;
        }
        if (byte == ';') {
            parts.params.push_back(anyDigit ? current : 0);
            current = 0;
            anyDigit = false;
            continue;
        }
        break;
    }
    if (anyDigit) parts.params.push_back(current);
    if (index < reply.size()) {
        auto const byte = static_cast<unsigned char>(reply[index]);
        if (byte >= 0x20 && byte <= 0x2f) {
            parts.intermediate = reply[index];
            ++index;
        }
    }
    if (index >= reply.size()) return std::nullopt;
    parts.final = reply[index];
    return parts;
}

std::string overrideVariable(Capability capability) {
    std::string name = "SSG_TERM_";
    for (char const ch : capabilityName(capability)) {
        name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return name;
}

}  // namespace

TerminalCapabilities::TerminalCapabilities(EnvironmentLookup lookup, Clock clock)
    : lookup_{std::move(lookup)}, clock_{std::move(clock)} {
    if (!clock_) {
        clock_ = [] { return std::chrono::steady_clock::now(); };
    }
    answers_.fill(Answer::Unknown);
    auto const read = [&](char const* name) {
        return lookup_ ? lookup_(name) : nullptr;
    };
    colorDepth_ = detectColorDepth(read("SSG_COLOR_DEPTH"), read("COLORTERM"),
                                     read("TERM"), read("TERM_PROGRAM"));
}

std::string TerminalCapabilities::beginProbe() {
    // Start from Unknown rather than carrying answers forward.  A second probe
    // asks a terminal that may not be the one that answered the first -- a
    // resumed session, a reattached multiplexer -- and a stale Present would
    // survive the fence that is supposed to be able to retire it.  Resetting can
    // only ever turn features off until their answers land, which is the safe
    // direction.
    answers_.fill(Answer::Unknown);
    probing_ = true;
    deadline_ = clock_() + kProbeWindow;
    // DA1 is written last: every terminal answers it, so its reply is the fence
    // that tells us the speculative questions above have had their chance.
    return std::string{"\x1b[?2026$p"}  // Synchronized output (DECRQM).
           + "\x1b[?u"                  // Keyboard protocol flags.
           + "\x1b[c";                  // Primary device attributes: the fence.
}

void TerminalCapabilities::observeReply(std::string_view reply) {
    if (expired()) endProbe();
    if (!probing_) return;
    auto const parts = parseReply(reply);
    if (!parts) return;
    auto const record = [&](Capability capability, bool present) {
        answers_[static_cast<std::size_t>(capability)] =
            present ? Answer::Present : Answer::Absent;
    };

    if (parts->final == 'c' && parts->privatePrefix) {
        // DA1: the leading parameter is the terminal class; the rest are
        // extensions, of which 52 advertises clipboard access.
        bool clipboard = false;
        for (std::size_t i = 1; i < parts->params.size(); ++i) {
            if (parts->params[i] == 52) clipboard = true;
        }
        record(Capability::ClipboardWrite, clipboard);
        endProbe();
        return;
    }
    if (parts->final == 'y' && parts->intermediate == '$' && parts->privatePrefix) {
        // DECRPM: mode, then state.  0 means the terminal does not recognize the
        // mode; 4 ("permanently reset") means it recognizes it but can never
        // enable it, which is indistinguishable from absent for our purposes.
        // 1 (set), 2 (reset) and 3 (permanently set) all mean usable.
        if (parts->params.size() >= 2 && parts->params[0] == 2026) {
            auto const state = parts->params[1];
            record(Capability::SynchronizedOutput,
                   state == 1 || state == 2 || state == 3);
        }
        return;
    }
    if (parts->final == 'u' && parts->privatePrefix) {
        // Only a terminal implementing the keyboard protocol answers at all.
        record(Capability::KeyboardProtocol, true);
    }
}

void TerminalCapabilities::endProbe() { probing_ = false; }

bool TerminalCapabilities::expired() const {
    return probing_ && clock_ && clock_() > deadline_;
}

bool TerminalCapabilities::probing() const { return probing_ && !expired(); }

std::optional<bool> TerminalCapabilities::overrideFor(Capability capability) const {
    if (!lookup_) return std::nullopt;
    char const* const value = lookup_(overrideVariable(capability));
    if (value == nullptr) return std::nullopt;
    auto const normalized = lowercase(value);
    if (matchesAny(normalized, {"1", "on", "yes", "true"})) return true;
    if (matchesAny(normalized, {"0", "off", "no", "false"})) return false;
    return std::nullopt;  // Unparseable: fall through to what the terminal said.
}

bool TerminalCapabilities::has(Capability capability) const {
    // An explicit override always wins: terminals lie, and a user hitting a
    // rendering bug needs a way to switch a feature off without rebuilding.
    if (auto const forced = overrideFor(capability)) return *forced;
    return answers_[static_cast<std::size_t>(capability)] == Answer::Present;
}

ssg::ColorDepth TerminalCapabilities::colorDepth() const { return colorDepth_; }

} // namespace ssg
