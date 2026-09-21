#pragma once

#include <ssg/color.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

enum class Capability : std::uint8_t {
    SynchronizedOutput,
    KeyboardProtocol,
    ClipboardWrite,
};

inline constexpr std::array<Capability, 3> kAllCapabilities{
    Capability::SynchronizedOutput,
    Capability::KeyboardProtocol,
    Capability::ClipboardWrite,
};

[[nodiscard]] std::string_view capabilityName(Capability capability);

// Owns terminal probing, reply interpretation, and normalized capability state.
class TerminalCapabilities {
  public:
    using EnvironmentLookup = std::function<const char*(std::string_view)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    static constexpr std::chrono::milliseconds kProbeWindow{250};

    explicit TerminalCapabilities(EnvironmentLookup lookup, Clock clock = {});

    [[nodiscard]] std::string beginProbe();
    void observeReply(std::string_view reply);
    void endProbe();
    [[nodiscard]] bool has(Capability capability) const;
    [[nodiscard]] ColorDepth colorDepth() const;

  private:
    enum class Answer : std::uint8_t { Unknown, Absent, Present };

    [[nodiscard]] std::optional<bool>
    overrideFor(Capability capability) const;
    [[nodiscard]] bool expired() const;

    EnvironmentLookup lookup_;
    Clock clock_;
    std::array<Answer, kAllCapabilities.size()> answers_{};
    bool probing_ = false;
    std::chrono::steady_clock::time_point deadline_{};
    ColorDepth colorDepth_ = ColorDepth::Ansi16;
};

} // namespace ssg
