#include "fake_lsp_server.h"

#include <algorithm>

namespace ssg::test {

LspIoResult FakeLspServer::write(std::string_view bytes,
                                 std::chrono::milliseconds) {
    if (write_timeout_) {
        write_timeout_ = false;
        return {LspIoStatus::Timeout, "scripted write timeout"};
    }
    auto decoded = client_decoder_.feed(bytes);
    if (!decoded.accepted()) {
        return {LspIoStatus::Error, decoded.message};
    }
    for (auto& payload : decoded.messages) {
        received_payloads_.push_back(std::move(payload));
    }
    return {LspIoStatus::Ok, {}};
}

LspIoResult FakeLspServer::read(std::string& bytes, std::size_t maximum_bytes,
                                std::chrono::milliseconds) {
    bytes.clear();
    if (read_timeout_) {
        read_timeout_ = false;
        return {LspIoStatus::Timeout, "scripted read timeout"};
    }
    if (reads_.empty()) {
        return {LspIoStatus::Timeout, "no scripted server bytes"};
    }
    auto& front = reads_.front();
    const auto count = std::min(maximum_bytes, front.size());
    bytes.assign(front.data(), count);
    front.erase(0, count);
    if (front.empty()) {
        reads_.pop_front();
    }
    return {LspIoStatus::Ok, {}};
}

void FakeLspServer::queue_payload(std::string payload, std::size_t chunk_bytes) {
    auto frame = encode_lsp_frame(payload);
    if (chunk_bytes == 0) {
        reads_.push_back(std::move(frame));
        return;
    }
    while (!frame.empty()) {
        const auto count = std::min(chunk_bytes, frame.size());
        reads_.push_back(frame.substr(0, count));
        frame.erase(0, count);
    }
}

void FakeLspServer::queue_raw(std::string bytes) {
    reads_.push_back(std::move(bytes));
}

void FakeLspServer::timeout_next_read() noexcept { read_timeout_ = true; }
void FakeLspServer::timeout_next_write() noexcept { write_timeout_ = true; }

std::string response(std::uint64_t id, std::string_view result_json) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
           ",\"result\":" + std::string(result_json) + "}";
}

std::string diagnostics(std::string_view uri,
                        std::optional<std::int64_t> version,
                        std::string_view diagnostics_json) {
    std::string value =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
        "\"params\":{\"uri\":\"" +
        std::string(uri) + "\",";
    if (version) {
        value += "\"version\":" + std::to_string(*version) + ",";
    }
    value += "\"diagnostics\":" + std::string(diagnostics_json) + "}}";
    return value;
}

} // namespace ssg::test
