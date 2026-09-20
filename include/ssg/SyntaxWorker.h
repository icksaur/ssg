#pragma once

#include <ssg/PlatformRuntime.h>
#include <ssg/SyntaxModel.h>
#include <ssg/Workspace.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ssg {

struct SyntaxWork {
    FileDocumentId document;
    std::shared_ptr<const SyntaxParseRequest> request;
};

struct SyntaxCompletion {
    FileDocumentId document;
    std::shared_ptr<const SyntaxParseRequest> request;
    SyntaxParseOutput output;
};

// Runs immutable syntax requests and returns immutable completions. Editor
// state remains owned by the runtime thread and is never reachable from here.
class SyntaxWorker {
public:
    explicit SyntaxWorker(std::shared_ptr<SyntaxParser> parser);
    ~SyntaxWorker();

    SyntaxWorker(const SyntaxWorker&) = delete;
    SyntaxWorker& operator=(const SyntaxWorker&) = delete;

    void submit(SyntaxWork work);
    void cancel(FileDocumentId document) noexcept;
    [[nodiscard]] std::deque<SyntaxCompletion> drain();
    [[nodiscard]] const PlatformWake* wake() const noexcept {
        return &readiness_;
    }

private:
    void run();

    std::shared_ptr<SyntaxParser> parser_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stop_ = false;
    std::map<std::uint64_t, SyntaxWork> queued_;
    std::shared_ptr<const SyntaxParseRequest> running_;
    std::optional<FileDocumentId> runningDocument_;
    std::deque<SyntaxCompletion> completed_;
    PlatformWake readiness_;
    std::thread thread_;
};

}  // namespace ssg
