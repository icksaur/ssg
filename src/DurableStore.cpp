#include <ssg/DurableStore.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace ssg {
namespace {

constexpr std::size_t kTimestampLength = 15;
constexpr std::size_t kFractionLength = 9;
constexpr std::size_t kLegacyTimestampLength = 20;
constexpr std::size_t kClaimAttempts = 10000;

bool decimal(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return character >= '0' && character <= '9';
           });
}

std::string randomSuffix() {
    std::array<std::byte, 16> bytes{};
    std::random_device source;
    for (auto& byte : bytes) {
        byte = static_cast<std::byte>(source() & 0xffU);
    }

    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = std::to_integer<unsigned int>(bytes[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 0x0fU];
    }
    return result;
}

bool older(const DurableStoreEntry& left, const DurableStoreEntry& right) {
    if (left.created != right.created) {
        if (!left.created) return false;
        if (!right.created) return true;
        return *left.created < *right.created;
    }
    return left.path.filename().string() < right.path.filename().string();
}

} // namespace

DurableStore::DurableStore(std::filesystem::path root)
    : root_(std::move(root)) {}

DurableStoreClaimResult DurableStore::claimEntry(
    std::chrono::system_clock::time_point created) {
    const auto rootCreated = ensureDirectory(root_);
    if (!rootCreated.ok()) {
        return {rootCreated.status, {}, rootCreated.message};
    }

    for (std::size_t attempt = 0; attempt < kClaimAttempts; ++attempt) {
        const auto path = root_ / entryName(created, randomSuffix());
        const auto claimed = createDirectoriesDurably(path);
        if (claimed.ok()) return {FileIoStatus::Ok, path, {}};
        if (claimed.status != FileIoStatus::AlreadyExists) {
            return {claimed.status, {}, claimed.message};
        }
    }
    return {FileIoStatus::IoError, {}, "could not claim a unique store entry"};
}

DurableStoreListResult DurableStore::entries() const {
    const auto listed = listDirectory(root_);
    if (!listed.ok()) {
        return {listed.status, {}, listed.complete, listed.message};
    }

    DurableStoreListResult result{
        FileIoStatus::Ok, {}, listed.complete, listed.message};
    try {
        for (const auto& listedEntry : listed.entries) {
            const auto status = statFile(listedEntry.path());
            if (!status || status->kind != FileKind::Directory) continue;
            const auto name = listedEntry.path().filename().string();
            result.entries.push_back(
                {listedEntry.path(), entryTimestamp(name)});
        }
    } catch (const std::exception& error) {
        return {FileIoStatus::IoError, {}, false, error.what()};
    }
    std::sort(result.entries.begin(), result.entries.end(), older);
    return result;
}

std::string DurableStore::entryName(
    std::chrono::system_clock::time_point created,
    std::string_view suffix) {
    const auto seconds =
        std::chrono::floor<std::chrono::seconds>(created);
    const auto fraction =
        std::chrono::duration_cast<std::chrono::nanoseconds>(created - seconds);
    const auto epochSeconds =
        std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::time_point{seconds});
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &epochSeconds);
#else
    gmtime_r(&epochSeconds, &utc);
#endif
    std::ostringstream name;
    name << std::put_time(&utc, "%Y%m%dT%H%M%S") << '-' << std::setfill('0')
         << std::setw(static_cast<int>(kFractionLength)) << fraction.count()
         << '-' << suffix;
    return name.str();
}

std::optional<std::chrono::system_clock::time_point>
DurableStore::entryTimestamp(std::string_view name) {
    if (name.size() >= kLegacyTimestampLength &&
        decimal(name.substr(0, kLegacyTimestampLength)) &&
        (name.size() == kLegacyTimestampLength ||
         name[kLegacyTimestampLength] == '-')) {
        std::uint64_t nanoseconds = 0;
        const auto parsed = std::from_chars(
            name.data(), name.data() + kLegacyTimestampLength, nanoseconds);
        if (parsed.ec == std::errc{}) {
            return std::chrono::system_clock::time_point{
                std::chrono::duration_cast<
                    std::chrono::system_clock::duration>(
                    std::chrono::nanoseconds{nanoseconds})};
        }
    }

    if (name.size() < kTimestampLength ||
        (name.size() > kTimestampLength &&
         name[kTimestampLength] != '-')) {
        return std::nullopt;
    }
    std::tm utc{};
    std::istringstream stamp{std::string{name.substr(0, kTimestampLength)}};
    stamp >> std::get_time(&utc, "%Y%m%dT%H%M%S");
    if (stamp.fail()) return std::nullopt;
#ifdef _WIN32
    const auto epochSeconds = _mkgmtime(&utc);
#else
    const auto epochSeconds = timegm(&utc);
#endif
    if (epochSeconds == static_cast<std::time_t>(-1)) return std::nullopt;

    auto created = std::chrono::system_clock::from_time_t(epochSeconds);
    const auto fractionStart = kTimestampLength + 1;
    if (name.size() >= fractionStart + kFractionLength &&
        decimal(name.substr(fractionStart, kFractionLength)) &&
        (name.size() == fractionStart + kFractionLength ||
         name[fractionStart + kFractionLength] == '-')) {
        std::uint64_t nanoseconds = 0;
        const auto parsed = std::from_chars(
            name.data() + fractionStart,
            name.data() + fractionStart + kFractionLength, nanoseconds);
        if (parsed.ec != std::errc{}) return std::nullopt;
        created +=
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::nanoseconds{nanoseconds});
    }
    return created;
}

DurableStoreEvictionResult DurableStore::evictOldestWhile(
    std::vector<DurableStoreEntry> candidates,
    const std::function<bool(std::span<const DurableStoreEntry>)>&
        policySatisfied) {
    std::sort(candidates.begin(), candidates.end(), older);
    DurableStoreEvictionResult result;
    result.remaining = std::move(candidates);
    result.policySatisfied = policySatisfied(result.remaining);
    while (!result.policySatisfied && !result.remaining.empty()) {
        const auto removed = removeTreeIfPresent(result.remaining.front().path);
        if (!removed.ok()) {
            result.message = removed.message;
            return result;
        }
        result.removed.push_back(std::move(result.remaining.front()));
        result.remaining.erase(result.remaining.begin());
        result.policySatisfied = policySatisfied(result.remaining);
    }
    return result;
}

} // namespace ssg
