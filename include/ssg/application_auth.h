#pragma once

#include <ssg/http_server.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ssg {

class SecureRandomSource {
public:
    virtual ~SecureRandomSource() = default;
    virtual void fill(std::span<std::byte> bytes) = 0;
};

class BearerCredential {
public:
    BearerCredential(BearerCredential const&) = delete;
    BearerCredential& operator=(BearerCredential const&) = delete;
    BearerCredential(BearerCredential&&) noexcept = default;
    BearerCredential& operator=(BearerCredential&&) noexcept = default;

    [[nodiscard]] std::string_view value() const noexcept { return value_; }

private:
    friend BearerCredential generate_bearer_credential(SecureRandomSource&);
    explicit BearerCredential(std::string value) : value_{std::move(value)} {}

    std::string value_;
};

[[nodiscard]] BearerCredential generate_bearer_credential(
    SecureRandomSource& source);
[[nodiscard]] BearerCredential generate_bearer_credential();

class ApplicationAuthentication {
public:
    ApplicationAuthentication(BearerCredential credential, SessionId session_id,
                              ClientId client_id, ViewId view_id);

    [[nodiscard]] std::optional<AuthenticatedSession> authenticate(
        std::string_view presented_credential) const;

private:
    BearerCredential credential_;
    SessionId session_id_;
    ClientId client_id_;
    ViewId view_id_;
};

}  // namespace ssg
