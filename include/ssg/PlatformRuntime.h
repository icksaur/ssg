#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ssg {

class PlatformWake {
public:
    PlatformWake();
    ~PlatformWake();
    PlatformWake(PlatformWake&&) noexcept;
    PlatformWake& operator=(PlatformWake&&) noexcept;
    PlatformWake(const PlatformWake&) = delete;
    PlatformWake& operator=(const PlatformWake&) = delete;

    // Producers queue authoritative data before notify. Consumers consume
    // before draining it. Notifications coalesce but remain ready until consume.
    void notify() noexcept;
    void consume() noexcept;

private:
    friend class PlatformEventLoop;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class TerminationKind : std::uint8_t {
    Hangup,
    Terminate,
    Interrupt,
    Break,
    Close,
    Logoff,
    Shutdown,
};

struct TerminationRequest {
    TerminationKind kind;
};

struct PlatformReadiness {
    bool input = false;
    bool resize = false;
    std::optional<TerminationRequest> termination;
    std::vector<std::size_t> wakes;
};

struct PlatformEventLoopOptions {
    bool monitorInput = true;
    bool monitorProcessControl = true;
};

class PlatformEventLoop {
public:
    explicit PlatformEventLoop(PlatformEventLoopOptions options = {});
    ~PlatformEventLoop();
    PlatformEventLoop(const PlatformEventLoop&) = delete;
    PlatformEventLoop& operator=(const PlatformEventLoop&) = delete;

    // Registered wakes are borrowed only for this call and must outlive it.
    [[nodiscard]] PlatformReadiness wait(
        std::optional<std::chrono::milliseconds> timeout,
        std::span<const PlatformWake* const> wakes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[noreturn]] void terminateProcess(TerminationRequest request);

} // namespace ssg
