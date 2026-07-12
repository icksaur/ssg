#pragma once

#include <ssg/session.h>
#include <ssg/snapshot.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

struct ProtocolLimits {
    std::size_t max_message_bytes{64 * 1024};
    std::size_t max_insert_bytes{32 * 1024};
};

enum class ProtocolError : std::uint8_t {
    none,
    message_too_large,
    malformed_message,
    unsupported_version,
    unsupported_command,
    insert_too_large,
};

struct InsertRequest {
    Revision base_revision;
    std::string text;

    bool operator==(InsertRequest const&) const = default;
};

struct DecodeInsertResult {
    ProtocolError error;
    std::optional<InsertRequest> request;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

struct SliceResponse {
    ProtocolError protocol_error;
    CommandError command_error;
    DocumentViewState snapshot;
    std::optional<DocumentDelta> delta;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return protocol_error == ProtocolError::none &&
               command_error == CommandError::none;
    }
    bool operator==(SliceResponse const&) const = default;
};

[[nodiscard]] std::string encode_insert_request(InsertRequest const& request);
[[nodiscard]] DecodeInsertResult decode_insert_request(
    std::string_view message, ProtocolLimits limits = {});
[[nodiscard]] std::string encode_slice_response(
    SliceResponse const& response);
[[nodiscard]] SliceResponse decode_slice_response(
    std::string_view message, ProtocolLimits limits = {});

class CoreEditorSlice {
public:
    CoreEditorSlice();
    ~CoreEditorSlice();

    CoreEditorSlice(CoreEditorSlice const&) = delete;
    CoreEditorSlice& operator=(CoreEditorSlice const&) = delete;

    [[nodiscard]] bool attach(InvocationPrincipal principal);
    [[nodiscard]] bool detach(ClientId client_id);
    [[nodiscard]] SliceResponse execute(ClientId client_id,
                                        InsertRequest const& request);
    [[nodiscard]] DocumentViewState snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
