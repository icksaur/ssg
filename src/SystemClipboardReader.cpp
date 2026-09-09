#include <ssg/SystemClipboardReader.h>

#include <ssg/TextCodec.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <span>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ssg {
namespace {

std::optional<std::string> environmentValue(std::string_view name) {
    const auto value = std::getenv(std::string{name}.c_str());
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string{value};
}

std::optional<std::filesystem::path> findExecutable(std::string_view name) {
    const auto path = environmentValue("PATH");
    if (!path) return std::nullopt;
    std::size_t begin = 0;
    while (begin <= path->size()) {
        const auto end = path->find(':', begin);
        const auto directory =
            path->substr(begin, end == std::string::npos ? end : end - begin);
        auto candidate =
            std::filesystem::path{directory.empty() ? "." : directory} / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return std::filesystem::absolute(candidate);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return std::nullopt;
}

void terminateAndReap(pid_t child) {
    if (::waitpid(child, nullptr, WNOHANG) == child) return;
    (void)::kill(-child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
}

SystemClipboardRead runProgram(const SystemClipboardProgram& program,
                               std::chrono::milliseconds deadline,
                               std::size_t byteLimit) {
    std::vector<char*> arguments;
    arguments.reserve(program.arguments.size() + 2);
    arguments.push_back(const_cast<char*>(program.executable.c_str()));
    for (const auto& argument : program.arguments) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);

    int output[2] = {-1, -1};
    if (::pipe(output) != 0) {
        return {SystemClipboardReadStatus::Failed, {}};
    }

    const auto child = ::fork();
    if (child < 0) {
        ::close(output[0]);
        ::close(output[1]);
        return {SystemClipboardReadStatus::Failed, {}};
    }
    if (child == 0) {
        (void)::setpgid(0, 0);
        ::close(output[0]);
        const int null = ::open("/dev/null", O_RDWR);
        if (null < 0 || ::dup2(null, STDIN_FILENO) < 0 ||
            ::dup2(output[1], STDOUT_FILENO) < 0 ||
            ::dup2(null, STDERR_FILENO) < 0) {
            _exit(126);
        }
        ::close(null);
        ::close(output[1]);

        ::execv(program.executable.c_str(), arguments.data());
        _exit(127);
    }

    (void)::setpgid(child, child);
    ::close(output[1]);
    const int flags = ::fcntl(output[0], F_GETFL, 0);
    if (flags < 0 || ::fcntl(output[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(output[0]);
        terminateAndReap(child);
        return {SystemClipboardReadStatus::Failed, {}};
    }

    std::string text;
    const auto expires = std::chrono::steady_clock::now() + deadline;
    bool outputComplete = false;
    bool childComplete = false;
    int status = 0;
    while (!outputComplete || !childComplete) {
        if (!outputComplete) {
            char buffer[4096];
            for (;;) {
                const auto count = ::read(output[0], buffer, sizeof buffer);
                if (count > 0) {
                    const auto size = static_cast<std::size_t>(count);
                    if (size > byteLimit - std::min(byteLimit, text.size())) {
                        ::close(output[0]);
                        terminateAndReap(child);
                        return {SystemClipboardReadStatus::TooLarge, {}};
                    }
                    text.append(buffer, size);
                    continue;
                }
                if (count == 0) {
                    outputComplete = true;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR) {
                    ::close(output[0]);
                    terminateAndReap(child);
                    return {SystemClipboardReadStatus::Failed, {}};
                }
                break;
            }
        }

        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            childComplete = true;
        } else if (waited < 0 && errno != EINTR) {
            ::close(output[0]);
            terminateAndReap(child);
            return {SystemClipboardReadStatus::Failed, {}};
        }
        if (outputComplete && childComplete) break;

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            expires - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            ::close(output[0]);
            terminateAndReap(child);
            return {SystemClipboardReadStatus::TimedOut, {}};
        }
        if (!outputComplete) {
            pollfd readiness{output[0], POLLIN | POLLHUP, 0};
            const auto ready =
                ::poll(&readiness, 1, static_cast<int>(remaining.count()));
            if (ready < 0 && errno != EINTR) {
                ::close(output[0]);
                terminateAndReap(child);
                return {SystemClipboardReadStatus::Failed, {}};
            }
        } else {
            (void)::poll(nullptr, 0, 1);
        }
    }
    ::close(output[0]);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return {SystemClipboardReadStatus::Failed, {}};
    }
    if (text.find('\0') != std::string::npos) {
        return {SystemClipboardReadStatus::InvalidUtf8, {}};
    }
    const auto bytes = std::span{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    if (!decodeText(bytes, TextEncoding::Utf8).accepted()) {
        return {SystemClipboardReadStatus::InvalidUtf8, {}};
    }
    return {SystemClipboardReadStatus::Success, std::move(text)};
}

} // namespace

std::vector<SystemClipboardProgram> discoverSystemClipboardPrograms(
    const ClipboardEnvironmentLookup& environment,
    const ClipboardExecutableLookup& executable) {
    std::vector<SystemClipboardProgram> programs;
    if (environment("WAYLAND_DISPLAY")) {
        if (auto path = executable("wl-paste")) {
            programs.push_back({std::move(*path), {"--no-newline"}});
        }
    }
    if (environment("DISPLAY")) {
        if (auto path = executable("xclip")) {
            programs.push_back(
                {std::move(*path),
                 {"-selection", "clipboard", "-out", "-target", "UTF8_STRING"}});
        }
    }
    return programs;
}

SystemClipboardReader::SystemClipboardReader()
    : SystemClipboardReader{
          discoverSystemClipboardPrograms(environmentValue, findExecutable)} {}

SystemClipboardReader::SystemClipboardReader(
    std::vector<SystemClipboardProgram> programs,
    std::chrono::milliseconds deadline, std::size_t byteLimit)
    : programs_{std::move(programs)}, deadline_{deadline}, byteLimit_{byteLimit} {}

SystemClipboardRead SystemClipboardReader::read() const {
    if (programs_.empty()) {
        return {SystemClipboardReadStatus::Unavailable, {}};
    }
    auto failure = SystemClipboardRead{SystemClipboardReadStatus::Failed, {}};
    for (const auto& program : programs_) {
        auto result = runProgram(program, deadline_, byteLimit_);
        if (result.accepted()) return result;
        failure = std::move(result);
    }
    return failure;
}

SystemClipboardPaste planSystemClipboardPaste(const ClientOwnedInput& request,
                                              SystemClipboardRead read) {
    const bool editor =
        request.kind == ClientOwnedInputKind::SystemClipboardPasteIntoEditor;
    const bool text =
        request.kind == ClientOwnedInputKind::SystemClipboardPasteIntoText;
    if (!editor && !text) return {};
    if (read.accepted()) {
        return read.text.empty()
                   ? SystemClipboardPaste{}
                   : SystemClipboardPaste{
                         SystemClipboardPasteKind::CommittedText,
                         std::move(read.text)};
    }
    if (editor) {
        return {SystemClipboardPasteKind::InternalRegister, {}};
    }
    return request.text.empty()
               ? SystemClipboardPaste{}
               : SystemClipboardPaste{SystemClipboardPasteKind::CommittedText,
                                      request.text};
}

} // namespace ssg
