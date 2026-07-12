#include "ssg/settings.h"

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
    return index(key) < setting_key_count;
}

constexpr bool valid(SettingScope scope) noexcept {
    return index(scope) <= index(SettingScope::document);
}

constexpr std::array all_keys{
    SettingKey::indent_width,
    SettingKey::indent_style,
    SettingKey::indent_detection,
    SettingKey::auto_indent,
    SettingKey::line_ending,
    SettingKey::final_newline,
    SettingKey::encoding,
    SettingKey::word_wrap,
    SettingKey::theme,
    SettingKey::keymap,
    SettingKey::search_case_sensitive,
    SettingKey::search_whole_word,
    SettingKey::search_regular_expression,
    SettingKey::undo_byte_budget,
    SettingKey::recovery_byte_budget,
    SettingKey::typing_coalescing_ms,
};

constexpr std::array<std::string_view, setting_key_count> key_names{
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
};

SettingValue default_value(SettingKey key) {
    switch (key) {
    case SettingKey::indent_width: return std::uint32_t{4};
    case SettingKey::indent_style: return IndentStyle::spaces;
    case SettingKey::indent_detection: return true;
    case SettingKey::auto_indent: return true;
    case SettingKey::line_ending: return LineEnding::lf;
    case SettingKey::final_newline: return true;
    case SettingKey::encoding: return TextEncoding::utf8;
    case SettingKey::word_wrap: return false;
    case SettingKey::theme: return std::string{"default"};
    case SettingKey::keymap: return std::string{"default"};
    case SettingKey::search_case_sensitive: return false;
    case SettingKey::search_whole_word: return false;
    case SettingKey::search_regular_expression: return false;
    case SettingKey::undo_byte_budget: return std::uint64_t{16u * 1024u * 1024u};
    case SettingKey::recovery_byte_budget: return std::uint64_t{256u * 1024u * 1024u};
    case SettingKey::typing_coalescing_ms: return std::uint32_t{750};
    }
    throw std::logic_error("unknown setting key");
}

std::optional<SettingError> validate(SettingKey key, const SettingValue& value) {
    const auto wrong_type = [key] {
        return SettingError{SettingErrorCode::wrong_value_type, key,
                            "setting value has the wrong type for its key"};
    };
    switch (key) {
    case SettingKey::indent_width: {
        const auto* width = std::get_if<std::uint32_t>(&value);
        if (width == nullptr) return wrong_type();
        if (*width < 1 || *width > 16) {
            return SettingError{SettingErrorCode::out_of_range, key,
                                "indent width must be in [1, 16]"};
        }
        return std::nullopt;
    }
    case SettingKey::indent_style:
        if (!std::holds_alternative<IndentStyle>(value)) return wrong_type();
        if (const auto style = std::get<IndentStyle>(value);
            style != IndentStyle::spaces && style != IndentStyle::tabs) {
            return SettingError{SettingErrorCode::out_of_range, key,
                                "indent style is not recognized"};
        }
        return std::nullopt;
    case SettingKey::line_ending: {
        const auto* ending = std::get_if<LineEnding>(&value);
        if (ending == nullptr) return wrong_type();
        if (*ending != LineEnding::lf && *ending != LineEnding::crlf &&
            *ending != LineEnding::cr) {
            return SettingError{SettingErrorCode::out_of_range, key,
                                "line ending must be lf, crlf, or cr"};
        }
        return std::nullopt;
    }
    case SettingKey::encoding:
        if (!std::holds_alternative<TextEncoding>(value)) return wrong_type();
        if (static_cast<std::uint8_t>(std::get<TextEncoding>(value)) >
            static_cast<std::uint8_t>(TextEncoding::iso88591)) {
            return SettingError{SettingErrorCode::out_of_range, key,
                                "text encoding is not recognized"};
        }
        return std::nullopt;
    case SettingKey::theme:
    case SettingKey::keymap: {
        const auto* identity = std::get_if<std::string>(&value);
        if (identity == nullptr) return wrong_type();
        if (identity->empty()) {
            return SettingError{SettingErrorCode::empty_identity, key,
                                "setting identity must not be empty"};
        }
        return std::nullopt;
    }
    case SettingKey::undo_byte_budget:
    case SettingKey::recovery_byte_budget:
        if (!std::holds_alternative<std::uint64_t>(value)) return wrong_type();
        return std::nullopt;
    case SettingKey::typing_coalescing_ms:
        if (!std::holds_alternative<std::uint32_t>(value)) return wrong_type();
        return std::nullopt;
    case SettingKey::indent_detection:
    case SettingKey::auto_indent:
    case SettingKey::final_newline:
    case SettingKey::word_wrap:
    case SettingKey::search_case_sensitive:
    case SettingKey::search_whole_word:
    case SettingKey::search_regular_expression:
        if (!std::holds_alternative<bool>(value)) return wrong_type();
        return std::nullopt;
    }
    throw std::logic_error("unknown setting key");
}

std::string encode_string(std::string_view value) {
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

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

std::optional<std::string> decode_string(std::string_view value) {
    std::string decoded;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            decoded.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) return std::nullopt;
        const int high = hex_value(value[i + 1]);
        const int low = hex_value(value[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return decoded;
}

template <class Integer>
std::optional<Integer> parse_integer(std::string_view value) {
    Integer result{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

std::string encode_value(const SettingValue& value) {
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
        return *style == IndentStyle::spaces ? "indent:spaces" : "indent:tabs";
    }
    if (const auto* ending = std::get_if<LineEnding>(&value)) {
        switch (*ending) {
        case LineEnding::lf: return "eol:lf";
        case LineEnding::crlf: return "eol:crlf";
        case LineEnding::cr: return "eol:cr";
        case LineEnding::mixed: break;
        }
    }
    if (const auto* encoding = std::get_if<TextEncoding>(&value)) {
        constexpr std::array names{
            "utf8", "utf8_bom", "utf16le", "utf16be", "windows1252", "iso88591"};
        return "encoding:" + std::string{names[static_cast<std::size_t>(*encoding)]};
    }
    return "s:" + encode_string(std::get<std::string>(value));
}

std::optional<SettingValue> decode_value(std::string_view encoded) {
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
        const auto parsed = parse_integer<std::uint32_t>(value);
        if (parsed) return SettingValue{*parsed};
        return std::nullopt;
    }
    if (type == "u64") {
        const auto parsed = parse_integer<std::uint64_t>(value);
        if (parsed) return SettingValue{*parsed};
        return std::nullopt;
    }
    if (type == "indent") {
        if (value == "spaces") return SettingValue{IndentStyle::spaces};
        if (value == "tabs") return SettingValue{IndentStyle::tabs};
        return std::nullopt;
    }
    if (type == "eol") {
        if (value == "lf") return SettingValue{LineEnding::lf};
        if (value == "crlf") return SettingValue{LineEnding::crlf};
        if (value == "cr") return SettingValue{LineEnding::cr};
        return std::nullopt;
    }
    if (type == "encoding") {
        if (value == "utf8") return SettingValue{TextEncoding::utf8};
        if (value == "utf8_bom") return SettingValue{TextEncoding::utf8_bom};
        if (value == "utf16le") return SettingValue{TextEncoding::utf16le};
        if (value == "utf16be") return SettingValue{TextEncoding::utf16be};
        if (value == "windows1252") return SettingValue{TextEncoding::windows1252};
        if (value == "iso88591") return SettingValue{TextEncoding::iso88591};
        return std::nullopt;
    }
    if (type == "s") {
        const auto decoded = decode_string(value);
        if (decoded) return SettingValue{*decoded};
    }
    return std::nullopt;
}

std::optional<SettingKey> key_from_name(std::string_view name) {
    for (const auto key : all_keys) {
        if (key_names[index(key)] == name) return key;
    }
    return std::nullopt;
}

std::optional<std::string> read_if_present(const std::filesystem::path& path,
                                           std::string& error) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        if (exists_error) error = exists_error.message();
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open settings file: " + path.string();
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>()};
}

void write_document(const std::filesystem::path& path, std::string_view document) {
    std::filesystem::create_directories(path.parent_path());
    const auto bytes = std::as_bytes(std::span{document.data(), document.size()});
    replace_file_atomically(path, bytes);
}

} // namespace

const SettingViewEntry* SettingsViewState::find(SettingKey key) const noexcept {
    if (!valid(key)) return nullptr;
    const auto position = index(key);
    return &entries[position];
}

SettingsModel::SettingsModel() {
    for (const auto key : all_keys) {
        scopes_[index(SettingScope::defaults)].values[index(key)] = default_value(key);
    }
}

EffectiveSetting SettingsModel::resolve(SettingKey key) const {
    if (!valid(key)) throw std::invalid_argument("setting key is not recognized");
    for (std::size_t scope = index(SettingScope::document);; --scope) {
        const auto& value = scopes_[scope].values[index(key)];
        if (value) return {*value, static_cast<SettingScope>(scope)};
        if (scope == 0) break;
    }
    throw std::logic_error("setting defaults are incomplete");
}

std::optional<SettingValue> SettingsModel::scoped_value(
    SettingScope scope, SettingKey key) const {
    if (!valid(scope)) throw std::invalid_argument("setting scope is not recognized");
    if (!valid(key)) throw std::invalid_argument("setting key is not recognized");
    return scopes_[index(scope)].values[index(key)];
}

SettingsViewState SettingsModel::view_state() const {
    SettingsViewState state;
    for (const auto key : all_keys) {
        state.entries[index(key)] = {key, resolve(key)};
    }
    return state;
}

SettingMutation SettingsModel::set(
    SettingScope scope, SettingKey key, SettingValue value) {
    if (!valid(key)) {
        return {{SettingError{SettingErrorCode::unknown_key, key,
                              "setting key is not recognized"}},
                std::nullopt, {}};
    }
    if (!valid(scope)) {
        return {{SettingError{SettingErrorCode::immutable_scope, key,
                              "setting scope is not recognized"}},
                std::nullopt, {}};
    }
    if (scope == SettingScope::defaults) {
        return {{SettingError{SettingErrorCode::immutable_scope, key,
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
        return {{SettingError{SettingErrorCode::unknown_key, key,
                              "setting key is not recognized"}},
                std::nullopt, {}};
    }
    if (!valid(scope)) {
        return {{SettingError{SettingErrorCode::immutable_scope, key,
                              "setting scope is not recognized"}},
                std::nullopt, {}};
    }
    if (scope == SettingScope::defaults) {
        return {{SettingError{SettingErrorCode::immutable_scope, key,
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
        return {{SettingError{SettingErrorCode::unknown_key, compensation.key,
                              "setting key is not recognized"}},
                std::nullopt, {}};
    }
    if (!valid(compensation.scope)) {
        return {{SettingError{SettingErrorCode::immutable_scope, compensation.key,
                              "setting scope is not recognized"}},
                std::nullopt, {}};
    }
    if (compensation.scope == SettingScope::defaults) {
        return {{SettingError{SettingErrorCode::immutable_scope, compensation.key,
                              "default settings are immutable"}},
                std::nullopt, {}};
    }
    auto& data = scopes_[index(compensation.scope)];
    auto& slot = data.values[index(compensation.key)];
    if (slot != compensation.expected ||
        data.generations[index(compensation.key)] !=
            compensation.expected_generation) {
        return {{SettingError{SettingErrorCode::stale_compensation, compensation.key,
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

std::string SettingsModel::export_scope(SettingScope scope) const {
    if (!valid(scope)) throw std::invalid_argument("setting scope is not recognized");
    std::string document{"schema=1\n"};
    const auto& data = scopes_[index(scope)];
    for (const auto key : all_keys) {
        const auto& value = data.values[index(key)];
        if (value) {
            document += key_names[index(key)];
            document += '=';
            document += encode_value(*value);
            document += '\n';
        }
    }
    for (const auto& field : data.unknown_fields) {
        document += field;
        document += '\n';
    }
    return document;
}

SettingsIoResult SettingsModel::import_scope(
    SettingScope scope, std::string_view document) {
    if (!valid(scope)) return {false, "setting scope is not recognized"};
    if (scope == SettingScope::defaults) {
        return {false, "default settings are immutable"};
    }
    ScopeData parsed;
    bool saw_schema = false;
    std::size_t start = 0;
    while (start < document.size()) {
        const auto end = document.find('\n', start);
        auto line = document.substr(
            start, end == std::string_view::npos ? document.size() - start : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = end == std::string_view::npos ? document.size() : end + 1;
        if (line.empty()) continue;
        if (!saw_schema) {
            if (line != "schema=1") return {false, "unsupported settings schema"};
            saw_schema = true;
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            return {false, "settings record is missing '='"};
        }
        const auto name = line.substr(0, separator);
        const auto key = key_from_name(name);
        if (!key) {
            parsed.unknown_fields.emplace_back(line);
            continue;
        }
        auto& slot = parsed.values[index(*key)];
        if (slot) return {false, "duplicate setting key: " + std::string{name}};
        const auto value = decode_value(line.substr(separator + 1));
        if (!value) return {false, "invalid encoded value for setting: " + std::string{name}};
        if (const auto error = validate(*key, *value)) return {false, error->message};
        slot = *value;
    }
    if (!saw_schema) return {false, "settings schema header is missing"};
    const auto& current = scopes_[index(scope)];
    for (std::size_t key = 0; key < setting_key_count; ++key) {
        parsed.generations[key] = current.generations[key] + 1;
    }
    scopes_[index(scope)] = std::move(parsed);
    return {};
}

SettingsPersistence::SettingsPersistence(SettingsPaths paths)
    : paths_(std::move(paths)) {
    if (paths_.user_file.empty() || paths_.workspace_file.empty()) {
        throw std::invalid_argument("settings persistence paths must not be empty");
    }
}

SettingsIoResult SettingsPersistence::load(SettingsModel& settings) const {
    SettingsModel candidate = settings;
    std::string error;
    const auto user = read_if_present(paths_.user_file, error);
    if (!error.empty()) return {false, error};
    if (user) {
        const auto result = candidate.import_scope(SettingScope::user, *user);
        if (!result.ok) return result;
    }
    const auto workspace = read_if_present(paths_.workspace_file, error);
    if (!error.empty()) return {false, error};
    if (workspace) {
        const auto result = candidate.import_scope(SettingScope::workspace, *workspace);
        if (!result.ok) return result;
    }
    settings = std::move(candidate);
    return {};
}

SettingsIoResult SettingsPersistence::save(const SettingsModel& settings) const {
    try {
        write_document(paths_.user_file, settings.export_scope(SettingScope::user));
        write_document(paths_.workspace_file,
                       settings.export_scope(SettingScope::workspace));
        return {};
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}

} // namespace ssg
