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
    Defaults,
    User,
    Workspace,
    Language,
    Document,
};

enum class SettingKey : std::uint8_t {
    IndentWidth,
    IndentStyle,
    IndentDetection,
    AutoIndent,
    LineEnding,
    FinalNewline,
    Encoding,
    WordWrap,
    Theme,
    Keymap,
    SearchCaseSensitive,
    SearchWholeWord,
    SearchRegularExpression,
    UndoByteBudget,
    RecoveryByteBudget,
    TypingCoalescingMs,
    FileFinderRespectGitignore,
};

inline constexpr std::size_t kSettingKeyCount = 17;

enum class TextEncoding : std::uint8_t {
    Utf8,
    Utf8Bom,
    Utf16le,
    Utf16be,
    Windows1252,
    Iso88591,
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

struct SettingsCommandDescriptor {
    std::string_view id;
};

struct SettingsCommandSet {
    std::array<SettingsCommandDescriptor, 6> descriptors{{
        {"settings.open"},
        {"settings.set"},
        {"settings.reset"},
        {"settings.reset_scope"},
        {"settings.export_workspace"},
        {"settings.import_workspace"},
    }};
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
