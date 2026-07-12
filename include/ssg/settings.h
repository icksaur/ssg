#pragma once

#include "ssg/config.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ssg {

enum class SettingScope : std::uint8_t {
    defaults,
    user,
    workspace,
    language,
    document,
};

enum class SettingKey : std::uint8_t {
    indent_width,
    indent_style,
    indent_detection,
    auto_indent,
    line_ending,
    final_newline,
    encoding,
    word_wrap,
    theme,
    keymap,
    search_case_sensitive,
    search_whole_word,
    search_regular_expression,
    undo_byte_budget,
    recovery_byte_budget,
    typing_coalescing_ms,
};

inline constexpr std::size_t setting_key_count = 16;

enum class TextEncoding : std::uint8_t {
    utf8,
    utf8_bom,
    utf16le,
    utf16be,
    windows1252,
    iso88591,
};

using SettingValue =
    std::variant<bool, std::uint32_t, std::uint64_t, IndentStyle, LineEnding,
                 TextEncoding, std::string>;

struct EffectiveSetting {
    SettingValue value;
    SettingScope source = SettingScope::defaults;

    friend bool operator==(const EffectiveSetting&, const EffectiveSetting&) = default;
};

struct SettingViewEntry {
    SettingKey key = SettingKey::indent_width;
    EffectiveSetting effective{std::uint32_t{4}, SettingScope::defaults};

    friend bool operator==(const SettingViewEntry&, const SettingViewEntry&) = default;
};

struct SettingsViewState {
    std::array<SettingViewEntry, setting_key_count> entries{};

    [[nodiscard]] const SettingViewEntry* find(SettingKey key) const noexcept;
    friend bool operator==(const SettingsViewState&, const SettingsViewState&) = default;
};

struct SettingsDelta {
    SettingKey key = SettingKey::indent_width;
    EffectiveSetting before{std::uint32_t{4}, SettingScope::defaults};
    EffectiveSetting after{std::uint32_t{4}, SettingScope::defaults};

    friend bool operator==(const SettingsDelta&, const SettingsDelta&) = default;
};

enum class SettingErrorCode : std::uint8_t {
    unknown_key,
    wrong_value_type,
    out_of_range,
    empty_identity,
    immutable_scope,
    stale_compensation,
    invalid_schema,
    invalid_record,
    io_failure,
};

struct SettingError {
    SettingErrorCode code = SettingErrorCode::invalid_record;
    SettingKey key = SettingKey::indent_width;
    std::string message;

    friend bool operator==(const SettingError&, const SettingError&) = default;
};

struct SettingCompensation {
    SettingScope scope = SettingScope::user;
    SettingKey key = SettingKey::indent_width;
    std::optional<SettingValue> expected;
    std::optional<SettingValue> restore;
    std::uint64_t expected_generation = 0;

    friend bool operator==(const SettingCompensation&, const SettingCompensation&) = default;
};

struct SettingMutation {
    std::optional<SettingError> error;
    std::optional<SettingsDelta> delta;
    SettingCompensation compensation;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

struct SettingsIoResult {
    bool ok = true;
    std::string message;
};

struct SettingsCommandDescriptor {
    std::string_view id;
};

struct SettingsCommandSet {
    std::array<SettingsCommandDescriptor, 2> descriptors{{
        {"settings.set"},
        {"settings.reset"},
    }};
};

class SettingsModel {
public:
    SettingsModel();

    [[nodiscard]] EffectiveSetting resolve(SettingKey key) const;
    [[nodiscard]] std::optional<SettingValue> scoped_value(
        SettingScope scope, SettingKey key) const;
    [[nodiscard]] SettingsViewState view_state() const;

    [[nodiscard]] SettingMutation set(
        SettingScope scope, SettingKey key, SettingValue value);
    [[nodiscard]] SettingMutation reset(SettingScope scope, SettingKey key);
    [[nodiscard]] SettingMutation apply(const SettingCompensation& compensation);

    [[nodiscard]] std::string export_scope(SettingScope scope) const;
    [[nodiscard]] SettingsIoResult import_scope(
        SettingScope scope, std::string_view document);

private:
    struct ScopeData {
        std::array<std::optional<SettingValue>, setting_key_count> values;
        std::array<std::uint64_t, setting_key_count> generations{};
        std::vector<std::string> unknown_fields;
    };

    std::array<ScopeData, 5> scopes_;
};

struct SettingsPaths {
    std::filesystem::path user_file;
    std::filesystem::path workspace_file;
};

[[nodiscard]] SettingsPaths linux_settings_paths(
    const std::filesystem::path& user_configuration_root,
    const std::filesystem::path& workspace_storage_root,
    std::string_view canonical_workspace);

[[nodiscard]] SettingsPaths windows_settings_paths(
    const std::filesystem::path& user_configuration_root,
    const std::filesystem::path& workspace_storage_root,
    std::string_view canonical_workspace);

class SettingsPersistence {
public:
    explicit SettingsPersistence(SettingsPaths paths);

    [[nodiscard]] SettingsIoResult load(SettingsModel& settings) const;
    [[nodiscard]] SettingsIoResult save(const SettingsModel& settings) const;

private:
    SettingsPaths paths_;
};

} // namespace ssg
