#include <ssg/ScratchStore.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace ssg {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::array<std::byte, 32> sha256(std::span<const std::byte> input) {
    std::array<std::uint32_t, 8> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::vector<std::byte> padded(input.begin(), input.end());
    padded.push_back(std::byte{0x80});
    while (padded.size() % 64 != 56) {
        padded.push_back(std::byte{0});
    }
    const auto bitCount = static_cast<std::uint64_t>(input.size()) * 8U;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(
            static_cast<std::byte>((bitCount >> shift) & 0xffU));
    }

    for (std::size_t block = 0; block < padded.size(); block += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = block + index * 4;
            words[index] =
                (std::to_integer<std::uint32_t>(padded[offset]) << 24U) |
                (std::to_integer<std::uint32_t>(padded[offset + 1]) << 16U) |
                (std::to_integer<std::uint32_t>(padded[offset + 2]) << 8U) |
                std::to_integer<std::uint32_t>(padded[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15], 7) ^
                            std::rotr(words[index - 15], 18) ^
                            (words[index - 15] >> 3U);
            const auto s1 = std::rotr(words[index - 2], 17) ^
                            std::rotr(words[index - 2], 19) ^
                            (words[index - 2] >> 10U);
            words[index] =
                words[index - 16] + s0 + words[index - 7] + s1;
        }

        auto a = state[0];
        auto b = state[1];
        auto c = state[2];
        auto d = state[3];
        auto e = state[4];
        auto f = state[5];
        auto g = state[6];
        auto h = state[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                              std::rotr(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temporary1 =
                h + sum1 + choice + kSha256Constants[index] + words[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                              std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::byte, 32> result{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            result[index * 4 + byte] = static_cast<std::byte>(
                (state[index] >> (24U - byte * 8U)) & 0xffU);
        }
    }
    return result;
}

std::string lowercaseHex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = std::to_integer<unsigned int>(bytes[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 0x0fU];
    }
    return result;
}

void requireCanonicalAbsolute(
    const std::filesystem::path& canonicalWorkspace) {
    if (!canonicalWorkspace.is_absolute() ||
        canonicalWorkspace.lexically_normal() != canonicalWorkspace) {
        throw std::invalid_argument(
            "scratch workspace path must be canonical and absolute");
    }
}

void makePrivateDirectory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directory(path, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "create scratch directory", path, error);
    }
    setOwnerOnlyPermissions(path);
}

std::string generateSessionId() {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (now < 0) {
        throw std::runtime_error("system clock predates the Unix epoch");
    }
    std::array<std::byte, 16> randomBytes{};
    std::random_device source;
    for (auto& byte : randomBytes) {
        byte = static_cast<std::byte>(source() & 0xffU);
    }
    std::ostringstream result;
    result << std::setw(20) << std::setfill('0')
           << static_cast<std::uint64_t>(now) << '-'
           << lowercaseHex(randomBytes);
    return result.str();
}

bool validSessionId(std::string_view id) {
    if (id.size() != 53 || id[20] != '-') {
        return false;
    }
    return std::all_of(id.begin(), id.begin() + 20,
                       [](char value) { return value >= '0' && value <= '9'; }) &&
           std::all_of(id.begin() + 21, id.end(), [](char value) {
               return (value >= '0' && value <= '9') ||
                      (value >= 'a' && value <= 'f');
           });
}

} // namespace

std::string scratchWorkspaceKey(
    const std::filesystem::path& canonicalWorkspace) {
    requireCanonicalAbsolute(canonicalWorkspace);
    const auto pathBytes = canonicalWorkspace.u8string();
    const auto bytes = std::as_bytes(std::span{pathBytes});
    return lowercaseHex(sha256(bytes));
}

JournalReplayResult ScratchRemnantClaim::replay() const {
    return ScratchJournal{journalPath()}.replay();
}

void ScratchRemnantClaim::markRestored() {
    constexpr std::array marker{std::byte{'r'}, std::byte{'e'}, std::byte{'s'},
                                std::byte{'t'}, std::byte{'o'}, std::byte{'r'},
                                std::byte{'e'}, std::byte{'d'}, std::byte{'\n'}};
    replaceFileAtomically(path_ / "restored", marker);
    setOwnerOnlyPermissions(path_ / "restored");
}

ScratchSession ScratchSession::create(
    const std::filesystem::path& scratchRoot,
    const std::filesystem::path& canonicalWorkspace) {
    requireCanonicalAbsolute(canonicalWorkspace);
    if (scratchRoot.empty()) {
        throw std::invalid_argument("scratch root must not be empty");
    }

    makePrivateDirectory(scratchRoot);
    const auto workspaces = scratchRoot / "workspaces";
    makePrivateDirectory(workspaces);
    const auto workspacePath =
        workspaces / scratchWorkspaceKey(canonicalWorkspace);
    makePrivateDirectory(workspacePath);
    const auto sessionsPath = workspacePath / "sessions";
    makePrivateDirectory(sessionsPath);

    for (int attempt = 0; attempt < 100; ++attempt) {
        auto id = generateSessionId();
        const auto path = sessionsPath / id;
        std::error_code error;
        const bool created = std::filesystem::create_directory(path, error);
        if (error) {
            throw std::filesystem::filesystem_error(
                "create scratch session directory", path, error);
        }
        if (!created) {
            continue;
        }
        setOwnerOnlyPermissions(path);
        auto lock = tryLockFile(path / "session.lock");
        if (!lock) {
            throw std::runtime_error(
                "new scratch session lock unexpectedly contended");
        }
        return ScratchSession{ScratchSessionId{std::move(id)}, path,
                              sessionsPath, std::move(*lock)};
    }
    throw std::runtime_error("cannot allocate a unique scratch session ID");
}

std::optional<ScratchRemnantClaim>
ScratchSession::claimNewestRestorable() const {
    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(sessionsPath_)) {
        std::error_code statusError;
        const bool isDirectory = entry.is_directory(statusError);
        if (statusError == std::errc::no_such_file_or_directory) {
            continue;
        }
        if (statusError) {
            throw std::filesystem::filesystem_error(
                "inspect scratch session directory", entry.path(), statusError);
        }
        if (isDirectory &&
            validSessionId(entry.path().filename().string()) &&
            entry.path().filename().string() != id_.value()) {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.filename().string() > right.filename().string();
              });

    for (const auto& candidate : candidates) {
        std::optional<ExclusiveFileLock> lock;
        try {
            lock = tryLockFile(candidate / "session.lock");
        } catch (const std::filesystem::filesystem_error& error) {
            if (error.code() == std::errc::no_such_file_or_directory) {
                continue;
            }
            throw;
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::no_such_file_or_directory) {
                continue;
            }
            throw;
        }
        if (!lock) {
            continue;
        }
        if (std::filesystem::exists(candidate / "restored") ||
            !std::filesystem::is_regular_file(candidate / "journal.bin")) {
            continue;
        }
        const auto replayed = ScratchJournal{candidate / "journal.bin"}.replay();
        if (replayed.recovery.documents.empty()) {
            continue;
        }
        return ScratchRemnantClaim{
            ScratchSessionId{candidate.filename().string()}, candidate,
            std::move(*lock)};
    }
    return std::nullopt;
}

} // namespace ssg
