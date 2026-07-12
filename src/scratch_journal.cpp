#include "ssg/scratch_journal.h"

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

constexpr std::array<std::byte, 4> magic{
    std::byte{'S'}, std::byte{'S'}, std::byte{'G'}, std::byte{'J'}};
constexpr std::uint16_t format_version = 1;
constexpr std::size_t header_size = 14;
constexpr std::uint32_t maximum_payload_size = 64U * 1024U * 1024U;

enum class RecordKind : std::uint8_t {
    checkpoint = 1,
    document = 2,
    remove = 3,
};

bool valid_utf8(std::string_view value) noexcept {
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

void require_valid_text(std::string_view value, std::string_view field) {
    if (!valid_utf8(value)) {
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

void encode_key(Writer& writer, const JournalDocumentKey& key) {
    if (key.kind() == JournalDocumentKeyKind::saved) {
        writer.u8(0);
        writer.string32(key.saved_path());
        return;
    }
    writer.u8(1);
    writer.raw(key.untitled_id().bytes());
}

void encode_document(Writer& writer, const JournalDocument& document) {
    require_valid_text(document.utf8_content, "journal document content");
    encode_key(writer, document.key);
    writer.u8(static_cast<std::uint8_t>(document.mode));
    writer.u8(document.dirty ? 1 : 0);
    writer.string64(document.utf8_content);
}

std::vector<std::byte> frame(RecordKind kind, Writer body) {
    Writer payload_writer;
    payload_writer.u8(static_cast<std::uint8_t>(kind));
    const auto body_bytes = std::move(body).take();
    payload_writer.raw(body_bytes);
    const auto payload = std::move(payload_writer).take();
    if (payload.size() > maximum_payload_size) {
        throw std::length_error("journal record exceeds maximum payload size");
    }

    Writer result;
    result.raw(magic);
    result.u16(format_version);
    result.u32(static_cast<std::uint32_t>(payload.size()));
    result.u32(crc32c(payload));
    result.raw(payload);
    return std::move(result).take();
}

bool decode_key(Reader& reader, JournalDocumentKey& key) {
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

bool decode_document(Reader& reader, JournalDocument& document) {
    JournalDocumentKey key =
        JournalDocumentKey::untitled(UntitledDocumentId{{}});
    std::uint8_t mode = 0;
    std::uint8_t dirty = 0;
    std::string content;
    if (!decode_key(reader, key) || !reader.u8(mode) || mode > 2 ||
        !reader.u8(dirty) || dirty > 1 || !reader.string64(content) ||
        !valid_utf8(content)) {
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

bool apply_payload(std::span<const std::byte> payload,
                   JournalRecoverySet& recovery) {
    Reader reader{payload};
    std::uint8_t raw_kind = 0;
    if (!reader.u8(raw_kind)) return false;
    const auto kind = static_cast<RecordKind>(raw_kind);

    if (kind == RecordKind::checkpoint) {
        std::uint32_t count = 0;
        if (!reader.u32(count)) return false;
        std::vector<JournalDocument> documents;
        documents.reserve(std::min<std::uint32_t>(count, 1024U));
        for (std::uint32_t index = 0; index < count; ++index) {
            JournalDocument document{
                JournalDocumentKey::untitled(UntitledDocumentId{{}})};
            if (!decode_document(reader, document) ||
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

    if (kind == RecordKind::document) {
        JournalDocument document{
            JournalDocumentKey::untitled(UntitledDocumentId{{}})};
        if (!decode_document(reader, document) || reader.remaining() != 0) {
            return false;
        }
        upsert(recovery.documents, std::move(document));
        return true;
    }

    if (kind == RecordKind::remove) {
        JournalDocumentKey key =
            JournalDocumentKey::untitled(UntitledDocumentId{{}});
        if (!decode_key(reader, key) || reader.remaining() != 0) return false;
        std::erase_if(recovery.documents,
                      [&](const auto& document) { return document.key == key; });
        return true;
    }
    return false;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (!std::filesystem::exists(path)) return {};
        throw std::system_error(errno, std::generic_category(),
                                "open scratch journal for replay");
    }
    const std::string contents{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    return {reinterpret_cast<const std::byte*>(contents.data()),
            reinterpret_cast<const std::byte*>(contents.data() +
                                               contents.size())};
}

#ifndef _WIN32
void throw_errno(std::string_view operation) {
    throw std::system_error(errno, std::generic_category(),
                            std::string{operation});
}

void sync_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path().empty()
                            ? std::filesystem::path{"."}
                            : path.parent_path();
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) throw_errno("open scratch journal parent directory");
    if (::fsync(descriptor) != 0) {
        const int failure = errno;
        ::close(descriptor);
        errno = failure;
        throw_errno("flush scratch journal parent directory");
    }
    if (::close(descriptor) != 0) {
        throw_errno("close scratch journal parent directory");
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
    std::string_view workspace_relative_path) {
    if (workspace_relative_path.empty() ||
        workspace_relative_path.front() == '/' ||
        workspace_relative_path.front() == '\\' ||
        workspace_relative_path.find('\0') != std::string_view::npos ||
        !valid_utf8(workspace_relative_path)) {
        throw std::invalid_argument(
            "saved journal identity must be a valid workspace-relative path");
    }
    std::size_t start = 0;
    while (start <= workspace_relative_path.size()) {
        const auto end = workspace_relative_path.find_first_of("/\\", start);
        const auto component = workspace_relative_path.substr(
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
    return {JournalDocumentKeyKind::saved,
            std::string{workspace_relative_path}, UntitledDocumentId{{}}};
}

JournalDocumentKey JournalDocumentKey::untitled(UntitledDocumentId id) {
    return {JournalDocumentKeyKind::untitled, {}, id};
}

const std::string& JournalDocumentKey::saved_path() const {
    if (kind_ != JournalDocumentKeyKind::saved) {
        throw std::logic_error("untitled journal key has no saved path");
    }
    return path_;
}

UntitledDocumentId JournalDocumentKey::untitled_id() const {
    if (kind_ != JournalDocumentKeyKind::untitled) {
        throw std::logic_error("saved journal key has no untitled ID");
    }
    return id_;
}

std::vector<std::byte> encode_checkpoint_record(
    const JournalRecoverySet& recovery) {
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
        encode_document(body, document);
    }
    return frame(RecordKind::checkpoint, std::move(body));
}

std::vector<std::byte> encode_document_record(
    const JournalDocument& document) {
    Writer body;
    encode_document(body, document);
    return frame(RecordKind::document, std::move(body));
}

std::vector<std::byte> encode_remove_record(const JournalDocumentKey& key) {
    Writer body;
    encode_key(body, key);
    return frame(RecordKind::remove, std::move(body));
}

JournalReplayResult replay_journal(std::span<const std::byte> bytes) {
    JournalReplayResult result;
    std::size_t position = 0;
    while (position < bytes.size()) {
        if (bytes.size() - position < header_size ||
            !std::equal(magic.begin(), magic.end(), bytes.begin() + position)) {
            result.discarded_tail = true;
            break;
        }
        Reader header{bytes.subspan(position + magic.size(),
                                    header_size - magic.size())};
        std::uint8_t version_low = 0;
        std::uint8_t version_high = 0;
        std::uint32_t payload_size = 0;
        std::uint32_t expected_crc = 0;
        if (!header.u8(version_low) || !header.u8(version_high) ||
            (static_cast<std::uint16_t>(version_low) |
             (static_cast<std::uint16_t>(version_high) << 8U)) !=
                format_version ||
            !header.u32(payload_size) ||
            payload_size > maximum_payload_size ||
            !header.u32(expected_crc) ||
            payload_size > bytes.size() - position - header_size) {
            result.discarded_tail = true;
            break;
        }
        const auto payload =
            bytes.subspan(position + header_size, payload_size);
        if (crc32c(payload) != expected_crc ||
            !apply_payload(payload, result.recovery)) {
            result.discarded_tail = true;
            break;
        }
        position += header_size + payload_size;
        result.valid_bytes = position;
    }
    return result;
}

ScratchJournal::ScratchJournal(std::filesystem::path path)
    : path_(std::move(path)) {
    if (path_.empty()) {
        throw std::invalid_argument("scratch journal path must not be empty");
    }
}

void ScratchJournal::append_checkpoint(
    const JournalRecoverySet& recovery) const {
    const auto record = encode_checkpoint_record(recovery);
    append(record);
}

void ScratchJournal::append_document(const JournalDocument& document) const {
    const auto record = encode_document_record(document);
    append(record);
}

void ScratchJournal::append_remove(const JournalDocumentKey& key) const {
    const auto record = encode_remove_record(key);
    append(record);
}

JournalReplayResult ScratchJournal::replay() const {
    return replay_journal(read_file(path_));
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
    if (descriptor < 0) throw_errno("open scratch journal for append");

    try {
        std::size_t written_total = 0;
        while (written_total < record.size()) {
            const auto written =
                ::write(descriptor, record.data() + written_total,
                        record.size() - written_total);
            if (written < 0) {
                if (errno == EINTR) continue;
                throw_errno("write scratch journal record");
            }
            if (written == 0) {
                throw std::system_error(EIO, std::generic_category(),
                                        "write scratch journal record");
            }
            written_total += static_cast<std::size_t>(written);
        }
        if (::fsync(descriptor) != 0) {
            throw_errno("flush scratch journal record");
        }
        if (::close(descriptor) != 0) {
            descriptor = -1;
            throw_errno("close scratch journal");
        }
        descriptor = -1;
        if (created) sync_parent_directory(path_);
    } catch (...) {
        if (descriptor >= 0) ::close(descriptor);
        throw;
    }
#endif
}

} // namespace ssg
