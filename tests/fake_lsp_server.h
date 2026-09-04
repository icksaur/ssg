#pragma once

#include <ssg/LspSyncClient.h>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ssg::test {

class FakeLspServer final : public LspByteStream {
public:
    LspIoResult write(std::string_view bytes,
                      std::chrono::milliseconds timeout) override;
    LspIoResult read(std::string& bytes, std::size_t maximum_bytes,
                     std::chrono::milliseconds timeout) override;

    void queue_payload(std::string payload, std::size_t chunk_bytes = 0);
    void queue_raw(std::string bytes);
    void timeout_next_read() noexcept;
    void timeout_next_write() noexcept;

    [[nodiscard]] const std::vector<std::string>& received_payloads() const {
        return received_payloads_;
    }

private:
    LspFrameDecoder client_decoder_;
    std::deque<std::string> reads_;
    std::vector<std::string> received_payloads_;
    bool read_timeout_ = false;
    bool write_timeout_ = false;
};

[[nodiscard]] std::string response(std::uint64_t id,
                                   std::string_view result_json = "null");
[[nodiscard]] std::string diagnostics(
    std::string_view uri, std::optional<std::int64_t> version,
    std::string_view diagnostics_json);

} // namespace ssg::test
