#include "ssg/Settings.h"

#include "ssg/platform_files.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <system_error>

namespace ssg {
namespace {

constexpr std::size_t index(SettingKey key) noexcept {
    return static_cast<std::size_t>(key);
}

constexpr std::size_t index(SettingScope scope) noexcept {
    return static_cast<std::size_t>(scope);
}

constexpr bool valid(SettingKey key) noexcept {
    return index(key) < kSettingKeyCount;
}

constexpr bool valid(SettingScope scope) noexcept {
    return index(scope) <= index(SettingScope::Document);
}

constexpr std::array kAllKeys{
    SettingKey::IndentWidth,
    SettingKey::IndentStyle,
    SettingKey::IndentDetection,
    SettingKey::AutoIndent,
    SettingKey::LineEnding,
    SettingKey::FinalNewline,
    SettingKey::Encoding,
    SettingKey::WordWrap,
    SettingKey::Theme,
    SettingKey::Keymap,
    SettingKey::SearchCaseSensitive,
    SettingKey::SearchWholeWord,
    SettingKey::SearchRegularExpression,
    SettingKey::UndoByteBudget,
    SettingKey::RecoveryByteBudget,
    SettingKey::TypingCoalescingMs,
    SettingKey::FileFinderRespectGitignore,
    SettingKey::AutosaveDebounceMs,
};

constexpr std::array<std::string_view, kSettingKeyCount> kEyNames{
    "indent_width",
    "indent_style",
    "indent_detection",
    "auto_indent",
    "line_ending",
    "final_newline",
    "encoding",
    "word_wrap",
    "theme",
    "keymap",
    "search_case_sensitive",
    "search_whole_word",
    "search_regular_expression",
    "undo_byte_budget",
    "recovery_byte_budget",
    "typing_coalescing_ms",
    "file_finder_respect_gitignore",
    "autosave_debounce_ms",
};

SettingValue defaultValue(SettingKey key) {
    switch (key) {
    case SettingKey::IndentWidth: return std::uint32_t{4};
    case SettingKey::IndentStyle: return IndentStyle::Spaces;
    case SettingKey::IndentDetection: return true;
    case SettingKey::AutoIndent: return true;
    case SettingKey::LineEnding: return LineEnding::Lf;
    case SettingKey::FinalNewline: return true;
    case SettingKey::Encoding: return TextEncoding::Utf8;
    case SettingKey::WordWrap: return false;
    case SettingKey::Theme: return std::string{"default"};
    case SettingKey::Keymap: return std::string{"default"};
    case SettingKey::SearchCaseSensitive: return false;
    case SettingKey::SearchWholeWord: return false;
    case SettingKey::SearchRegularExpression: return false;
    case SettingKey::UndoByteBudget: return std::uint64_t{16u * 1024u * 1024u};
    case SettingKey::RecoveryByteBudget: return std::uint64_t{256u * 1024u * 1024u};
    case SettingKey::TypingCoalescingMs: return std::uint32_t{750};
    case SettingKey::FileFinderRespectGitignore: return true;
    case SettingKey::AutosaveDebounceMs: return std::uint32_t{10000};
    }
    throw std::logic_error("unknown setting key");
}

std::optional<SettingError> validate(SettingKey key, const SettingValue& value) {
    const auto wrongType = [key] {
        return SettingError{SettingErrorCode::WrongValueType, key,
                            "setting value has the wrong type for its key"};
    };
    switch (key) {
    case SettingKey::IndentWidth: {
        const auto* width = std::get_if<std::uint32_t>(&value);
        if (width == nullptr) return wrongType();
        if (*width < 1 || *width > 16) {
            return SettingError{SettingErrorCode::OutOfRange, key,
                                "indent width must be in [1, 16]"};
        }
        return std::nullopt;
    }
    case SettingKey::IndentStyle:
        if (!std::holds_alternative<IndentStyle>(value)) return wrongType();
        if (const auto style = std::get<IndentStyle>(value);
            style != IndentStyle::Spaces && style != IndentStyle::Tabs) {
            return SettingError{SettingErrorCode::OutOfRange, key,
                                "indent style is not recognized"};
        }
        return std::nullopt;
    case SettingKey::LineEnding: {
        const auto* ending = std::get_if<LineEnding>(&value);
        if (ending == nullptr) return wrongType();
        if (*ending != LineEnding::Lf && *ending != LineEnding::Crlf &&
            *ending != LineEnding::Cr) {
            return SettingError{SettingErrorCode::OutOfRange, key,
                                "line ending must be lf, crlf, or cr"};
        }
        return std::nullopt;
    }
    case SettingKey::Encoding:
        if (!std::holds_alternative<TextEncoding>(value)) return wrongType();
        if (static_cast<std::uint8_t>(std::get<TextEncoding>(value)) >
            static_cast<std::uint8_t>(TextEncoding::Iso88591)) {
            return SettingError{SettingErrorCode::OutOfRange, key,
                                "text encoding is not recognized"};
        }
        return std::nullopt;
    case SettingKey::Theme:
    case SettingKey::Keymap: {
        const auto* identity = std::get_if<std::string>(&value);
        if (identity == nullptr) return wrongType();
        if (identity->empty()) {
            return SettingError{SettingErrorCode::EmptyIdentity, key,
                                "setting identity must not be empty"};
        }
        return std::nullopt;
    }
    case SettingKey::UndoByteBudget:
    case SettingKey::RecoveryByteBudget:
        if (!std::holds_alternative<std::uint64_t>(value)) return wrongType();
        return std::nullopt;
    case SettingKey::TypingCoalescingMs:
        if (!std::holds_alternative<std::uint32_t>(value)) return wrongType();
        return std::nullopt;
    case SettingKey::AutosaveDebounceMs:
        if (!std::holds_alternative<std::uint32_t>(value)) return wrongType();
        if (std::get<std::uint32_t>(value) < 1) {
            return SettingError{SettingErrorCode::OutOfRange, key,
                                "autosave debounce must be at least 1 ms"};
        }
        return std::nullopt;
    case SettingKey::IndentDetection:
    case SettingKey::AutoIndent:
    case SettingKey::FinalNewline:
    case SettingKey::WordWrap:
    case SettingKey::SearchCaseSensitive:
    case SettingKey::SearchWholeWord:
    case SettingKey::SearchRegularExpression:
    case SettingKey::FileFinderRespectGitignore:
        if (!std::holds_alternative<bool>(value)) return wrongType();
        return std::nullopt;
    }
    throw std::logic_error("unknown setting key");
}

std::string encodeString(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char byte : value) {
        const bool safe = (byte >= 'a' && byte <= 'z') ||
                          (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '-' ||
                          byte == '_' || byte == '.';
        if (safe) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[byte >> 4]);
            encoded.push_back(hex[byte & 0x0f]);
        }
    }
    return encoded;
}

int hexValue(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

std::optional<std::string> decodeString(std::string_view value) {
    std::string decoded;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            decoded.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) return std::nullopt;
        const int high = hexValue(value[i + 1]);
        const int low = hexValue(value[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return decoded;
}

template <class Integer>
std::optional<Integer> parseInteger(std::string_view value) {
    Integer result{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

std::string encodeValue(const SettingValue& value) {
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "b:1" : "b:0";
    }
    if (const auto* number = std::get_if<std::uint32_t>(&value)) {
        return "u32:" + std::to_string(*number);
    }
    if (const auto* number = std::get_if<std::uint64_t>(&value)) {
        return "u64:" + std::to_string(*number);
    }
    if (const auto* style = std::get_if<IndentStyle>(&value)) {
        return *style == IndentStyle::Spaces ? "indent:spaces" : "indent:tabs";
    }
    if (const auto* ending = std::get_if<LineEnding>(&value)) {
        switch (*ending) {
        case LineEnding::Lf: return "eol:lf";
        case LineEnding::Crlf: return "eol:crlf";
        case LineEnding::Cr: return "eol:cr";
        case LineEnding::Mixed: break;
        }
    }
    if (const auto* encoding = std::get_if<TextEncoding>(&value)) {
        constexpr std::array names{
            "utf8", "utf8_bom", "utf16le", "utf16be", "windows1252", "iso88591"};
        return "encoding:" + std::string{names[static_cast<std::size_t>(*encoding)]};
    }
    return "s:" + encodeString(std::get<std::string>(value));
}

std::optional<SettingValue> decodeValue(std::string_view encoded) {
    const auto separator = encoded.find(':');
    if (separator == std::string_view::npos) return std::nullopt;
    const auto type = encoded.substr(0, separator);
    const auto value = encoded.substr(separator + 1);
    if (type == "b") {
        if (value == "0") return SettingValue{false};
        if (value == "1") return SettingValue{true};
        return std::nullopt;
    }
    if (type == "u32") {
        const auto parsed = parseInteger<std::uint32_t>(value);
        if (parsed) return SettingValue{*parsed};
        return std::nullopt;
    }
    if (type == "u64") {
        const auto parsed = parseInteger<std::uint64_t>(value);
        if (parsed) return SettingValue{*parsed};
        return std::nullopt;
    }
    if (type == "indent") {
        if (value == "spaces") return SettingValue{IndentStyle::Spaces};
        if (value == "tabs") return SettingValue{IndentStyle::Tabs};
        return std::nullopt;
    }
    if (type == "eol") {
        if (value == "lf") return SettingValue{LineEnding::Lf};
        if (value == "crlf") return SettingValue{LineEnding::Crlf};
        if (value == "cr") return SettingValue{LineEnding::Cr};
        return std::nullopt;
    }
    if (type == "encoding") {
        if (value == "utf8") return SettingValue{TextEncoding::Utf8};
        if (value == "utf8_bom") return SettingValue{TextEncoding::Utf8Bom};
        if (value == "utf16le") return SettingValue{TextEncoding::Utf16le};
        if (value == "utf16be") return SettingValue{TextEncoding::Utf16be};
        if (value == "windows1252") return SettingValue{TextEncoding::Windows1252};
        if (value == "iso88591") return SettingValue{TextEncoding::Iso88591};
        return std::nullopt;
    }
    if (type == "s") {
        const auto decoded = decodeString(value);
        if (decoded) return SettingValue{*decoded};
    }
    return std::nullopt;
}

std::optional<SettingKey> keyFromName(std::string_view name) {
    for (const auto key : kAllKeys) {
        if (kEyNames[index(key)] == name) return key;
    }
    return std::nullopt;
}

std::optional<std::string> readIfPresent(const std::filesystem::path& path,
                                           std::string& error) {
    // One call decides present-vs-absent-vs-unreadable. The previous
    // exists()-then-open pair could report "absent" for a file that appeared
    // between the two calls, and reported an open failure for a file that had
    // been removed in between.
    auto result = readFile(path);
    if (result.status == FileIoStatus::NotFound) return std::nullopt;
    if (!result.ok()) {
        error = "failed to open settings file: " + path.string();
        return std::nullopt;
    }
    return std::string{reinterpret_cast<const char*>(result.bytes.data()),
                       result.bytes.size()};
}

void writeDocument(const std::filesystem::path& path, std::string_view document) {
    std::filesystem::create_directories(path.parent_path());
    const auto bytes = std::as_bytes(std::span{document.data(), document.size()});
    replaceFileAtomically(path, bytes);
}

} // namespace

const SettingViewEntry* SettingsViewState::find(SettingKey key) const noexcept {
    if (!valid(key)) return nullptr;
    const auto position = index(key);
    return &entries[position];
}

SettingsModel::SettingsModel() {
    for (const auto key : kAllKeys) {
        scopes_[index(SettingScope::Defaults)].values[index(key)] = defaultValue(key);
    }
}

EffectiveSetting SettingsModel::resolve(SettingKey key) const {
    if (!valid(key)) throw std::invalid_argument("setting key is not recognized");
    for (std::size_t scope = index(SettingScope::Document);; --scope) {
        const auto& value = scopes_[scope].values[index(key)];
        if (value) return {*value, static_cast<SettingScope>(scope)};
        if (scope == 0) break;
    }
    throw std::logic_error("setting defaults are incomplete");
}

std::optional<SettingValue> SettingsModel::scopedValue(
    SettingScope scope, SettingKey key) const {
    if (!valid(scope)) throw std::invalid_argument("setting scope is not recognized");
    if (!valid(key)) throw std::invalid_argument("setting key is not recognized");
    return scopes_[index(scope)].values[index(key)];
}

SettingsViewState SettingsModel::viewState() const {
    SettingsViewState state;
    for (const auto key : kAllKeys) {
        state.entries[index(key)] = {key, resolve(key)};
    }
    return state;
}

SettingMutation SettingsModel::set(
    SettingScope scope, SettingKey key, SettingValue value) {
    if (!valid(key)) {
        return {{SettingError{SettingErrorCode::UnknownKey, key,
                              "setting key is not recognized"}},
                std::nullopt, {}};
    }
    if (!valid(scope)) {
        return {{SettingError{SettingErrorCode::ImmutableScope, key,
                              "setting scope is not recognized"}},
                std::nullopt, {}};
    }
    if (scope == SettingScope::Defaults) {
        return {{SettingError{SettingErrorCode::ImmutableScope, key,
                              "default settings are immutable"}},
                std::nullopt, {}};
    }
    if (auto error = validate(key, value)) return {{std::move(*error)}, std::nullopt, {}};

    auto& data = scopes_[index(scope)];
    auto& slot = data.values[index(key)];
    const auto before = resolve(key);
    const auto restore = slot;
    slot = std::move(value);
    const auto generation = ++data.generations[index(key)];
    const auto after = resolve(key);
    return {std::nullopt, SettingsDelta{key, before, after},
            SettingCompensation{scope, key, slot, restore, generation}};
}

SettingMutation SettingsModel::reset(SettingScope scope, SettingKey key) {
    if (!valid(key)) {
        return {{SettingError{SettingErrorCode::UnknownKey, key,
                              "setting key is not recognized"}},
                std::nullopt, {}};
    }
    if (!valid(scope)) {
        return {{SettingError{SettingErrorCode::ImmutableScope, key,
                              "setting scope is not recognized"}},
                std::nullopt, {}};
    }
    if (scope == SettingScope::Defaults) {
        return {{SettingError{SettingErrorCode::ImmutableScope, key,
                              "default settings are immutable"}},
                std::nullopt, {}};
    }
    auto& data = scopes_[index(scope)];
    auto& slot = data.values[index(key)];
    const auto before = resolve(key);
    const auto restore = slot;
    slot.reset();
    const auto generation = ++data.generations[index(key)];
    const auto after = resolve(key);
    return {std::nullopt, SettingsDelta{key, before, after},
            SettingCompensation{scope, key, std::nullopt, restore, generation}};
}

SettingMutation SettingsModel::apply(const SettingCompensation& compensation) {
    if (!valid(compensation.key)) {
        return {{SettingError{SettingErrorCode::UnknownKey, compensation.key,
                              "setting key is not recognized"}},
                std::nullopt, {}};
    }
    if (!valid(compensation.scope)) {
        return {{SettingError{SettingErrorCode::ImmutableScope, compensation.key,
                              "setting scope is not recognized"}},
                std::nullopt, {}};
    }
    if (compensation.scope == SettingScope::Defaults) {
        return {{SettingError{SettingErrorCode::ImmutableScope, compensation.key,
                              "default settings are immutable"}},
                std::nullopt, {}};
    }
    auto& data = scopes_[index(compensation.scope)];
    auto& slot = data.values[index(compensation.key)];
    if (slot != compensation.expected ||
        data.generations[index(compensation.key)] !=
            compensation.expectedGeneration) {
        return {{SettingError{SettingErrorCode::StaleCompensation, compensation.key,
                              "setting changed after the compensating action was created"}},
                std::nullopt, {}};
    }
    const auto before = resolve(compensation.key);
    slot = compensation.restore;
    const auto generation = ++data.generations[index(compensation.key)];
    const auto after = resolve(compensation.key);
    return {std::nullopt, SettingsDelta{compensation.key, before, after},
            SettingCompensation{compensation.scope, compensation.key,
                                compensation.restore, compensation.expected,
                                generation}};
}

std::string SettingsModel::exportScope(SettingScope scope) const {
    if (!valid(scope)) throw std::invalid_argument("setting scope is not recognized");
    std::string document{"schema=1\n"};
    const auto& data = scopes_[index(scope)];
    for (const auto key : kAllKeys) {
        const auto& value = data.values[index(key)];
        if (value) {
            document += kEyNames[index(key)];
            document += '=';
            document += encodeValue(*value);
            document += '\n';
        }
    }
    for (const auto& field : data.unknownFields) {
        document += field;
        document += '\n';
    }
    return document;
}

SettingsIoResult SettingsModel::importScope(
    SettingScope scope, std::string_view document) {
    if (!valid(scope)) return {false, "setting scope is not recognized"};
    if (scope == SettingScope::Defaults) {
        return {false, "default settings are immutable"};
    }
    ScopeData parsed;
    bool sawSchema = false;
    std::size_t start = 0;
    while (start < document.size()) {
        const auto end = document.find('\n', start);
        auto line = document.substr(
            start, end == std::string_view::npos ? document.size() - start : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = end == std::string_view::npos ? document.size() : end + 1;
        if (line.empty()) continue;
        if (!sawSchema) {
            if (line != "schema=1") return {false, "unsupported settings schema"};
            sawSchema = true;
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            return {false, "settings record is missing '='"};
        }
        const auto name = line.substr(0, separator);
        const auto key = keyFromName(name);
        if (!key) {
            parsed.unknownFields.emplace_back(line);
            continue;
        }
        auto& slot = parsed.values[index(*key)];
        if (slot) return {false, "duplicate setting key: " + std::string{name}};
        const auto value = decodeValue(line.substr(separator + 1));
        if (!value) return {false, "invalid encoded value for setting: " + std::string{name}};
        if (const auto error = validate(*key, *value)) return {false, error->message};
        slot = *value;
    }
    if (!sawSchema) return {false, "settings schema header is missing"};
    const auto& current = scopes_[index(scope)];
    for (std::size_t key = 0; key < kSettingKeyCount; ++key) {
        parsed.generations[key] = current.generations[key] + 1;
    }
    scopes_[index(scope)] = std::move(parsed);
    return {};
}

SettingsPersistence::SettingsPersistence(SettingsPaths paths)
    : paths_(std::move(paths)) {
    if (paths_.userFile.empty() || paths_.workspaceFile.empty()) {
        throw std::invalid_argument("settings persistence paths must not be empty");
    }
}

SettingsIoResult SettingsPersistence::load(SettingsModel& settings) const {
    SettingsModel candidate = settings;
    std::string error;
    const auto user = readIfPresent(paths_.userFile, error);
    if (!error.empty()) return {false, error};
    if (user) {
        const auto result = candidate.importScope(SettingScope::User, *user);
        if (!result.ok) return result;
    }
    const auto workspace = readIfPresent(paths_.workspaceFile, error);
    if (!error.empty()) return {false, error};
    if (workspace) {
        const auto result = candidate.importScope(SettingScope::Workspace, *workspace);
        if (!result.ok) return result;
    }
    settings = std::move(candidate);
    return {};
}

SettingsIoResult SettingsPersistence::save(const SettingsModel& settings) const {
    try {
        writeDocument(paths_.userFile, settings.exportScope(SettingScope::User));
        writeDocument(paths_.workspaceFile,
                       settings.exportScope(SettingScope::Workspace));
        return {};
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}

} // namespace ssg
