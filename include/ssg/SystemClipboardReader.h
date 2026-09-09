#pragma once

#include <ssg/ClientInput.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

inline constexpr auto kSystemClipboardReadDeadline =
    std::chrono::milliseconds{500};
inline constexpr std::size_t kSystemClipboardReadLimit = 8 * 1024 * 1024;

struct SystemClipboardProgram {
    std::filesystem::path executable;
    std::vector<std::string> arguments;

    friend bool operator==(const SystemClipboardProgram&,
                           const SystemClipboardProgram&) = default;
};

using ClipboardEnvironmentLookup =
    std::function<std::optional<std::string>(std::string_view)>;
using ClipboardExecutableLookup =
    std::function<std::optional<std::filesystem::path>(std::string_view)>;

[[nodiscard]] std::vector<SystemClipboardProgram>
discoverSystemClipboardPrograms(const ClipboardEnvironmentLookup& environment,
                                const ClipboardExecutableLookup& executable);

enum class SystemClipboardReadStatus {
    Success,
    Unavailable,
    Failed,
    InvalidUtf8,
    TooLarge,
    TimedOut,
};

struct SystemClipboardRead {
    SystemClipboardReadStatus status = SystemClipboardReadStatus::Unavailable;
    std::string text;

    [[nodiscard]] bool accepted() const noexcept {
        return status == SystemClipboardReadStatus::Success;
    }
};

enum class SystemClipboardPasteKind {
    None,
    CommittedText,
    InternalRegister,
};

struct SystemClipboardPaste {
    SystemClipboardPasteKind kind = SystemClipboardPasteKind::None;
    std::string text;
};

[[nodiscard]] SystemClipboardPaste planSystemClipboardPaste(
    const ClientOwnedInput& request, SystemClipboardRead read);

// Runs optional desktop clipboard helpers with bounded time and output.
// Every started helper is terminated if necessary and reaped before return.
class SystemClipboardReader {
public:
    SystemClipboardReader();
    explicit SystemClipboardReader(
        std::vector<SystemClipboardProgram> programs,
        std::chrono::milliseconds deadline = kSystemClipboardReadDeadline,
        std::size_t byteLimit = kSystemClipboardReadLimit);

    [[nodiscard]] SystemClipboardRead read() const;

private:
    std::vector<SystemClipboardProgram> programs_;
    std::chrono::milliseconds deadline_;
    std::size_t byteLimit_;
};

} // namespace ssg
