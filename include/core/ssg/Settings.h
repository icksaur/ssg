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
    Defaults = 0,
    User = 1,
    Workspace = 2,
    Language = 3,
    Document = 4,
};

enum class SettingKey : std::uint8_t {
    IndentWidth = 0,
    IndentStyle = 1,
    IndentDetection = 2,
    AutoIndent = 3,
    LineEnding = 4,
    FinalNewline = 5,
    Encoding = 6,
    WordWrap = 7,
    Theme = 8,
    Keymap = 9,
    SearchCaseSensitive = 10,
    SearchWholeWord = 11,
    SearchRegularExpression = 12,
    UndoByteBudget = 13,
    RecoveryByteBudget = 14,
    TypingCoalescingMs = 15,
    FileFinderRespectGitignore = 16,
    AutosaveDebounceMs = 17,
    LineNumbers = 18,
};

inline constexpr std::size_t kSettingKeyCount =
    static_cast<std::size_t>(SettingKey::LineNumbers) + 1;

enum class TextEncoding : std::uint8_t {
    Utf8 = 0,
    Utf8Bom = 1,
    Utf16le = 2,
    Utf16be = 3,
    Windows1252 = 4,
    Iso88591 = 5,
};

using SettingValue =
    std::variant<bool, std::uint32_t, std::uint64_t, IndentStyle, LineEnding,
                 TextEncoding, std::string>;

struct EffectiveSetting {
    SettingValue value;
    SettingScope source = SettingScope::Defaults;

    friend bool operator==(const EffectiveSetting&, const EffectiveSetting&) = default;
};

struct SettingViewEntry {
    SettingKey key = SettingKey::IndentWidth;
    EffectiveSetting effective{std::uint32_t{4}, SettingScope::Defaults};

    friend bool operator==(const SettingViewEntry&, const SettingViewEntry&) = default;
};

struct SettingsViewState {
    std::array<SettingViewEntry, kSettingKeyCount> entries{};

    [[nodiscard]] const SettingViewEntry* find(SettingKey key) const noexcept;
    friend bool operator==(const SettingsViewState&, const SettingsViewState&) = default;
};

struct SettingsDelta {
    SettingKey key = SettingKey::IndentWidth;
    EffectiveSetting before{std::uint32_t{4}, SettingScope::Defaults};
    EffectiveSetting after{std::uint32_t{4}, SettingScope::Defaults};

    friend bool operator==(const SettingsDelta&, const SettingsDelta&) = default;
};

enum class SettingErrorCode : std::uint8_t {
    UnknownKey,
    WrongValueType,
    OutOfRange,
    EmptyIdentity,
    ImmutableScope,
    StaleCompensation,
    InvalidSchema,
    InvalidRecord,
    IoFailure,
};

struct SettingError {
    SettingErrorCode code = SettingErrorCode::InvalidRecord;
    SettingKey key = SettingKey::IndentWidth;
    std::string message;

    friend bool operator==(const SettingError&, const SettingError&) = default;
};

struct SettingCompensation {
    SettingScope scope = SettingScope::User;
    SettingKey key = SettingKey::IndentWidth;
    std::optional<SettingValue> expected;
    std::optional<SettingValue> restore;
    std::uint64_t expectedGeneration = 0;

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

struct SettingSetArguments {
    SettingScope scope = SettingScope::User;
    SettingKey key = SettingKey::IndentWidth;
    SettingValue value = std::uint32_t{4};

    bool operator==(const SettingSetArguments&) const = default;
};

struct SettingResetArguments {
    SettingScope scope = SettingScope::User;
    SettingKey key = SettingKey::IndentWidth;

    bool operator==(const SettingResetArguments&) const = default;
};

struct SettingResetScopeArguments {
    SettingScope scope = SettingScope::User;

    bool operator==(const SettingResetScopeArguments&) const = default;
};

class SettingsModel {
public:
    SettingsModel();

    [[nodiscard]] EffectiveSetting resolve(SettingKey key) const;
    [[nodiscard]] std::optional<SettingValue> scopedValue(
        SettingScope scope, SettingKey key) const;
    [[nodiscard]] SettingsViewState viewState() const;

    [[nodiscard]] SettingMutation set(
        SettingScope scope, SettingKey key, SettingValue value);
    [[nodiscard]] SettingMutation reset(SettingScope scope, SettingKey key);
    [[nodiscard]] SettingMutation apply(const SettingCompensation& compensation);

    [[nodiscard]] std::string exportScope(SettingScope scope) const;
    [[nodiscard]] SettingsIoResult importScope(
        SettingScope scope, std::string_view document);

private:
    struct ScopeData {
        std::array<std::optional<SettingValue>, kSettingKeyCount> values;
        std::array<std::uint64_t, kSettingKeyCount> generations{};
        std::vector<std::string> unknownFields;
    };

    std::array<ScopeData, 5> scopes_;
};

struct SettingsPaths {
    std::filesystem::path userFile;
    std::filesystem::path workspaceFile;
};

[[nodiscard]] SettingsPaths linuxSettingsPaths(
    const std::filesystem::path& userConfigurationRoot,
    const std::filesystem::path& workspaceStorageRoot,
    std::string_view canonicalWorkspace);

[[nodiscard]] SettingsPaths windowsSettingsPaths(
    const std::filesystem::path& userConfigurationRoot,
    const std::filesystem::path& workspaceStorageRoot,
    std::string_view canonicalWorkspace);

class SettingsPersistence {
public:
    explicit SettingsPersistence(SettingsPaths paths);

    [[nodiscard]] SettingsIoResult load(SettingsModel& settings) const;
    [[nodiscard]] SettingsIoResult save(const SettingsModel& settings) const;

private:
    SettingsPaths paths_;
};

} // namespace ssg
