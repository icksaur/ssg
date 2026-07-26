#include "ssg/ScratchJournal.h"

#include "ssg/platform_files.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ssg {
namespace {

constexpr std::array<std::byte, 4> kMagic{
    std::byte{'S'}, std::byte{'S'}, std::byte{'G'}, std::byte{'J'}};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 14;
constexpr std::uint32_t kMaximumPayloadSize = 64U * 1024U * 1024U;

enum class RecordKind : std::uint8_t {
    Checkpoint = 1,
    Document = 2,
    Remove = 3,
};

bool validUtf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t continuation = 0;
        std::uint32_t scalar = 0;
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1;
            scalar = first & 0x1fU;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2;
            scalar = first & 0x0fU;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3;
            scalar = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= value.size()) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next =
                static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) return false;
            scalar = (scalar << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2 && scalar < 0x800U) ||
            (continuation == 3 && scalar < 0x10000U) ||
            scalar > 0x10ffffU ||
            (scalar >= 0xd800U && scalar <= 0xdfffU)) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

void requireValidText(std::string_view value, std::string_view field) {
    if (!validUtf8(value)) {
        throw std::invalid_argument(std::string{field} +
                                    " must contain valid UTF-8");
    }
}

class Writer {
public:
    void u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8U));
    }

    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void raw(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void string32(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("journal string exceeds 32-bit limit");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        raw({reinterpret_cast<const std::byte*>(value.data()), value.size()});
    }

    void string64(std::string_view value) {
        u64(value.size());
        raw({reinterpret_cast<const std::byte*>(value.data()), value.size()});
    }

    std::vector<std::byte> take() && { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t& value) {
        if (remaining() < 1) return false;
        value = std::to_integer<std::uint8_t>(bytes_[position_++]);
        return true;
    }

    bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t part = 0;
            if (!u8(part)) return false;
            value |= static_cast<std::uint32_t>(part) << shift;
        }
        return true;
    }

    bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::uint8_t part = 0;
            if (!u8(part)) return false;
            value |= static_cast<std::uint64_t>(part) << shift;
        }
        return true;
    }

    bool raw(std::size_t size, std::span<const std::byte>& value) {
        if (size > remaining()) return false;
        value = bytes_.subspan(position_, size);
        position_ += size;
        return true;
    }

    bool string32(std::string& value) {
        std::uint32_t size = 0;
        if (!u32(size)) return false;
        return string(size, value);
    }

    bool string64(std::string& value) {
        std::uint64_t size = 0;
        if (!u64(size) || size > remaining()) return false;
        return string(static_cast<std::size_t>(size), value);
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - position_;
    }

private:
    bool string(std::size_t size, std::string& value) {
        std::span<const std::byte> bytes;
        if (!raw(size, bytes)) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^
                  (0x82f63b78U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void encodeKey(Writer& writer, const JournalDocumentKey& key) {
    if (key.kind() == JournalDocumentKeyKind::Saved) {
        writer.u8(0);
        writer.string32(key.savedPath());
        return;
    }
    writer.u8(1);
    writer.raw(key.untitledId().bytes());
}

void encodeDocument(Writer& writer, const JournalDocument& document) {
    requireValidText(document.utf8Content, "journal document content");
    encodeKey(writer, document.key);
    writer.u8(static_cast<std::uint8_t>(document.mode));
    writer.u8(document.dirty ? 1 : 0);
    writer.string64(document.utf8Content);
}

std::vector<std::byte> frame(RecordKind kind, Writer body) {
    Writer payloadWriter;
    payloadWriter.u8(static_cast<std::uint8_t>(kind));
    const auto bodyBytes = std::move(body).take();
    payloadWriter.raw(bodyBytes);
    const auto payload = std::move(payloadWriter).take();
    if (payload.size() > kMaximumPayloadSize) {
        throw std::length_error("journal record exceeds maximum payload size");
    }

    Writer result;
    result.raw(kMagic);
    result.u16(kFormatVersion);
    result.u32(static_cast<std::uint32_t>(payload.size()));
    result.u32(crc32c(payload));
    result.raw(payload);
    return std::move(result).take();
}

bool decodeKey(Reader& reader, JournalDocumentKey& key) {
    std::uint8_t kind = 0;
    if (!reader.u8(kind)) return false;
    try {
        if (kind == 0) {
            std::string path;
            if (!reader.string32(path)) return false;
            key = JournalDocumentKey::saved(path);
            return true;
        }
        if (kind == 1) {
            std::span<const std::byte> bytes;
            if (!reader.raw(16, bytes)) return false;
            std::array<std::byte, 16> id{};
            std::copy(bytes.begin(), bytes.end(), id.begin());
            key = JournalDocumentKey::untitled(UntitledDocumentId{id});
            return true;
        }
    } catch (const std::invalid_argument&) {
        return false;
    }
    return false;
}

bool decodeDocument(Reader& reader, JournalDocument& document) {
    JournalDocumentKey key =
        JournalDocumentKey::untitled(UntitledDocumentId{{}});
    std::uint8_t mode = 0;
    std::uint8_t dirty = 0;
    std::string content;
    if (!decodeKey(reader, key) || !reader.u8(mode) || mode > 2 ||
        !reader.u8(dirty) || dirty > 1 || !reader.string64(content) ||
        !validUtf8(content)) {
        return false;
    }
    document = {std::move(key), static_cast<DocumentMode>(mode), dirty != 0,
                std::move(content)};
    return true;
}

void upsert(std::vector<JournalDocument>& documents,
            JournalDocument document) {
    const auto found =
        std::find_if(documents.begin(), documents.end(), [&](const auto& item) {
            return item.key == document.key;
        });
    if (found == documents.end()) {
        documents.push_back(std::move(document));
    } else {
        *found = std::move(document);
    }
}

bool applyPayload(std::span<const std::byte> payload,
                   JournalRecoverySet& recovery) {
    Reader reader{payload};
    std::uint8_t rawKind = 0;
    if (!reader.u8(rawKind)) return false;
    const auto kind = static_cast<RecordKind>(rawKind);

    if (kind == RecordKind::Checkpoint) {
        std::uint32_t count = 0;
        if (!reader.u32(count)) return false;
        std::vector<JournalDocument> documents;
        documents.reserve(std::min<std::uint32_t>(count, 1024U));
        for (std::uint32_t index = 0; index < count; ++index) {
            JournalDocument document{
                JournalDocumentKey::untitled(UntitledDocumentId{{}})};
            if (!decodeDocument(reader, document) ||
                std::any_of(documents.begin(), documents.end(),
                            [&](const auto& existing) {
                                return existing.key == document.key;
                            })) {
                return false;
            }
            documents.push_back(std::move(document));
        }
        if (reader.remaining() != 0) return false;
        recovery.documents = std::move(documents);
        return true;
    }

    if (kind == RecordKind::Document) {
        JournalDocument document{
            JournalDocumentKey::untitled(UntitledDocumentId{{}})};
        if (!decodeDocument(reader, document) || reader.remaining() != 0) {
            return false;
        }
        upsert(recovery.documents, std::move(document));
        return true;
    }

    if (kind == RecordKind::Remove) {
        JournalDocumentKey key =
            JournalDocumentKey::untitled(UntitledDocumentId{{}});
        if (!decodeKey(reader, key) || reader.remaining() != 0) return false;
        std::erase_if(recovery.documents,
                      [&](const auto& document) { return document.key == key; });
        return true;
    }
    return false;
}

std::vector<std::byte> readJournalBytes(const std::filesystem::path& path) {
    auto result = readFile(path);
    // A journal that has never been written is not an error: replay of nothing
    // is the correct start state. Any OTHER failure must surface, because
    // treating an unreadable journal as empty would silently discard recovery
    // data.
    if (result.status == FileIoStatus::NotFound) return {};
    if (!result.ok()) {
        throw std::runtime_error("open scratch journal for replay: " +
                                 result.message);
    }
    return {reinterpret_cast<const std::byte*>(result.bytes.data()),
            reinterpret_cast<const std::byte*>(result.bytes.data() +
                                               result.bytes.size())};
}

#ifndef _WIN32
void throwErrno(std::string_view operation) {
    throw std::system_error(errno, std::generic_category(),
                            std::string{operation});
}

void syncParentDirectory(const std::filesystem::path& path) {
    const auto parent = path.parent_path().empty()
                            ? std::filesystem::path{"."}
                            : path.parent_path();
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) throwErrno("open scratch journal parent directory");
    if (::fsync(descriptor) != 0) {
        const int failure = errno;
        ::close(descriptor);
        errno = failure;
        throwErrno("flush scratch journal parent directory");
    }
    if (::close(descriptor) != 0) {
        throwErrno("close scratch journal parent directory");
    }
}
#endif

} // namespace

UntitledDocumentId UntitledDocumentId::generate() {
    std::array<std::byte, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<std::byte>(random() & 0xffU);
    }
    bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
    bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
    return UntitledDocumentId{bytes};
}

JournalDocumentKey JournalDocumentKey::saved(
    std::string_view workspaceRelativePath) {
    if (workspaceRelativePath.empty() ||
        workspaceRelativePath.front() == '/' ||
        workspaceRelativePath.front() == '\\' ||
        workspaceRelativePath.find('\0') != std::string_view::npos ||
        !validUtf8(workspaceRelativePath)) {
        throw std::invalid_argument(
            "saved journal identity must be a valid workspace-relative path");
    }
    std::size_t start = 0;
    while (start <= workspaceRelativePath.size()) {
        const auto end = workspaceRelativePath.find_first_of("/\\", start);
        const auto component = workspaceRelativePath.substr(
            start, end == std::string_view::npos
                       ? std::string_view::npos
                       : end - start);
        if (component.empty() || component == "." || component == "..") {
            throw std::invalid_argument(
                "saved journal identity contains an invalid path component");
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {JournalDocumentKeyKind::Saved,
            std::string{workspaceRelativePath}, UntitledDocumentId{{}}};
}

JournalDocumentKey JournalDocumentKey::untitled(UntitledDocumentId id) {
    return {JournalDocumentKeyKind::Untitled, {}, id};
}

const std::string& JournalDocumentKey::savedPath() const {
    if (kind_ != JournalDocumentKeyKind::Saved) {
        throw std::logic_error("untitled journal key has no saved path");
    }
    return path_;
}

UntitledDocumentId JournalDocumentKey::untitledId() const {
    if (kind_ != JournalDocumentKeyKind::Untitled) {
        throw std::logic_error("saved journal key has no untitled ID");
    }
    return id_;
}

std::vector<std::byte> JournalCodec::encodeCheckpoint(
    const JournalRecoverySet& recovery) const {
    if (recovery.documents.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("too many documents in journal checkpoint");
    }
    Writer body;
    body.u32(static_cast<std::uint32_t>(recovery.documents.size()));
    std::vector<JournalDocumentKey> keys;
    keys.reserve(recovery.documents.size());
    for (const auto& document : recovery.documents) {
        if (std::find(keys.begin(), keys.end(), document.key) != keys.end()) {
            throw std::invalid_argument(
                "journal checkpoint contains duplicate document identity");
        }
        keys.push_back(document.key);
        ::ssg::encodeDocument(body, document);
    }
    return frame(RecordKind::Checkpoint, std::move(body));
}

std::vector<std::byte> JournalCodec::encodeDocument(
    const JournalDocument& document) const {
    Writer body;
    ::ssg::encodeDocument(body, document);
    return frame(RecordKind::Document, std::move(body));
}

std::vector<std::byte> JournalCodec::encodeRemove(
    const JournalDocumentKey& key) const {
    Writer body;
    encodeKey(body, key);
    return frame(RecordKind::Remove, std::move(body));
}

JournalReplayResult JournalCodec::replay(std::span<const std::byte> bytes) const {
    JournalReplayResult result;
    std::size_t position = 0;
    while (position < bytes.size()) {
        if (bytes.size() - position < kHeaderSize ||
            !std::equal(kMagic.begin(), kMagic.end(), bytes.begin() + position)) {
            result.discardedTail = true;
            break;
        }
        Reader header{bytes.subspan(position + kMagic.size(),
                                    kHeaderSize - kMagic.size())};
        std::uint8_t versionLow = 0;
        std::uint8_t versionHigh = 0;
        std::uint32_t payloadSize = 0;
        std::uint32_t expectedCrc = 0;
        if (!header.u8(versionLow) || !header.u8(versionHigh) ||
            (static_cast<std::uint16_t>(versionLow) |
             (static_cast<std::uint16_t>(versionHigh) << 8U)) !=
                kFormatVersion ||
            !header.u32(payloadSize) ||
            payloadSize > kMaximumPayloadSize ||
            !header.u32(expectedCrc) ||
            payloadSize > bytes.size() - position - kHeaderSize) {
            result.discardedTail = true;
            break;
        }
        const auto payload =
            bytes.subspan(position + kHeaderSize, payloadSize);
        if (crc32c(payload) != expectedCrc ||
            !applyPayload(payload, result.recovery)) {
            result.discardedTail = true;
            break;
        }
        position += kHeaderSize + payloadSize;
        result.validBytes = position;
    }
    return result;
}

ScratchJournal::ScratchJournal(std::filesystem::path path)
    : path_(std::move(path)) {
    if (path_.empty()) {
        throw std::invalid_argument("scratch journal path must not be empty");
    }
}

void ScratchJournal::appendCheckpoint(
    const JournalRecoverySet& recovery) const {
    const auto record = JournalCodec{}.encodeCheckpoint(recovery);
    append(record);
}

void ScratchJournal::appendDocument(const JournalDocument& document) const {
    const auto record = JournalCodec{}.encodeDocument(document);
    append(record);
}

void ScratchJournal::appendRemove(const JournalDocumentKey& key) const {
    const auto record = JournalCodec{}.encodeRemove(key);
    append(record);
}

JournalReplayResult ScratchJournal::replay() const {
    return JournalCodec{}.replay(readJournalBytes(path_));
}

void ScratchJournal::append(std::span<const std::byte> record) const {
    const auto parent = path_.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent)) {
        throw std::invalid_argument(
            "scratch journal parent directory must already exist");
    }

#ifdef _WIN32
    const HANDLE handle =
        CreateFileW(path_.c_str(), FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "open scratch journal for append");
    }
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    try {
        if (created) set_owner_only_permissions(path_);
        std::size_t written_total = 0;
        while (written_total < record.size()) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                record.size() - written_total,
                std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(handle, record.data() + written_total, chunk,
                           &written, nullptr) ||
                written == 0) {
                throw std::system_error(static_cast<int>(GetLastError()),
                                        std::system_category(),
                                        "write scratch journal record");
            }
            written_total += written;
        }
        if (!FlushFileBuffers(handle)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "flush scratch journal record");
        }
        CloseHandle(handle);
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
#else
    bool created = false;
    int descriptor =
        ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL, 0600);
    if (descriptor >= 0) {
        created = true;
    } else if (errno == EEXIST) {
        descriptor = ::open(path_.c_str(), O_WRONLY | O_APPEND);
    }
    if (descriptor < 0) throwErrno("open scratch journal for append");

    try {
        std::size_t writtenTotal = 0;
        while (writtenTotal < record.size()) {
            const auto written =
                ::write(descriptor, record.data() + writtenTotal,
                        record.size() - writtenTotal);
            if (written < 0) {
                if (errno == EINTR) continue;
                throwErrno("write scratch journal record");
            }
            if (written == 0) {
                throw std::system_error(EIO, std::generic_category(),
                                        "write scratch journal record");
            }
            writtenTotal += static_cast<std::size_t>(written);
        }
        if (::fsync(descriptor) != 0) {
            throwErrno("flush scratch journal record");
        }
        if (::close(descriptor) != 0) {
            descriptor = -1;
            throwErrno("close scratch journal");
        }
        descriptor = -1;
        if (created) syncParentDirectory(path_);
    } catch (...) {
        if (descriptor >= 0) ::close(descriptor);
        throw;
    }
#endif
}

} // namespace ssg
