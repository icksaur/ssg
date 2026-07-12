#pragma once

#include <ssg/document.h>
#include <ssg/history.h>
#include <ssg/selection.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class ClipboardCommand : std::uint8_t {
    copy,
    cut,
    paste,
};

struct ClipboardCommandDescriptor {
    std::string_view id;
    ClipboardCommand command;

    bool operator==(const ClipboardCommandDescriptor&) const noexcept = default;
};

class ClipboardCommandSet {
public:
    ClipboardCommandSet(const ClipboardCommandSet&) = default;
    ClipboardCommandSet& operator=(const ClipboardCommandSet&) = delete;

    [[nodiscard]] const std::array<ClipboardCommandDescriptor, 3>&
    descriptors() const noexcept;

private:
    friend ClipboardCommandSet clipboard_command_set();
    ClipboardCommandSet();

    const std::array<ClipboardCommandDescriptor, 3> descriptors_;
};

[[nodiscard]] ClipboardCommandSet clipboard_command_set();

enum class ClipboardRequestKind : std::uint8_t {
    write,
    read,
};

struct ClipboardRequest {
    std::uint64_t id;
    ClipboardRequestKind kind;
    Revision request_revision;
    std::string text;

    bool operator==(const ClipboardRequest&) const = default;
};

enum class ClipboardResponseStatus : std::uint8_t {
    success,
    denied,
    unavailable,
    disconnected,
};

struct ClipboardResponse {
    std::uint64_t id;
    Revision request_revision;
    Revision observed_document_revision;
    ClipboardResponseStatus status;
    std::string text;

    bool operator==(const ClipboardResponse&) const = default;
};

enum class ClipboardPasteMode : std::uint8_t {
    internal_only,
    system_first,
};

enum class ClipboardSystemStatus : std::uint8_t {
    not_requested,
    pending,
    succeeded,
    denied,
    unavailable,
    disconnected,
    stale,
};

enum class ClipboardError : std::uint8_t {
    none,
    read_only,
    diff,
    invalid_selection,
    invalid_utf8,
    stale_response,
    no_request,
    request_exhausted,
    document_rejected,
};

struct ClipboardResult {
    ClipboardError error;
    ClipboardSystemStatus system_status;
    Revision revision;
    std::optional<SelectionSet> selections;
    std::optional<ClipboardRequest> request;
    bool document_changed;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ClipboardError::none;
    }
};

struct ClipboardViewState {
    std::vector<std::string> fragments;
    std::string plain_text;
    std::optional<ClipboardRequest> pending_read;
    std::optional<ClipboardRequest> pending_write;

    bool operator==(const ClipboardViewState&) const = default;
};

struct ClipboardDelta {
    bool changed;
    std::optional<ClipboardViewState> replacement;

    bool operator==(const ClipboardDelta&) const = default;
};

[[nodiscard]] ClipboardDelta derive_clipboard_delta(
    const ClipboardViewState& before, const ClipboardViewState& after);

class ClipboardRegister {
public:
    explicit ClipboardRegister(int tab_width = 4);
    ~ClipboardRegister();

    ClipboardRegister(const ClipboardRegister&) = delete;
    ClipboardRegister& operator=(const ClipboardRegister&) = delete;
    ClipboardRegister(ClipboardRegister&&) noexcept;
    ClipboardRegister& operator=(ClipboardRegister&&) noexcept;

    [[nodiscard]] ClipboardResult copy(
        const DocumentSnapshot& document, const SelectionSet& selections);
    [[nodiscard]] ClipboardResult cut(
        Document& document, DocumentHistory& history,
        const SelectionSet& selections, std::uint64_t timestamp_ms);
    [[nodiscard]] ClipboardResult paste(
        Document& document, DocumentHistory& history,
        const SelectionSet& selections, ClipboardPasteMode mode,
        std::uint64_t timestamp_ms);
    [[nodiscard]] ClipboardResult handle_response(
        Document& document, DocumentHistory& history,
        const SelectionSet& current_selections,
        const ClipboardResponse& response, std::uint64_t timestamp_ms);

    [[nodiscard]] ClipboardViewState view_state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
