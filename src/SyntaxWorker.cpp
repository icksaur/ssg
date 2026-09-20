#include <ssg/SyntaxWorker.h>

#include <utility>

namespace ssg {

SyntaxWorker::SyntaxWorker(std::shared_ptr<SyntaxParser> parser)
    : parser_(std::move(parser)) {
    if (parser_) thread_ = std::thread{&SyntaxWorker::run, this};
}

SyntaxWorker::~SyntaxWorker() {
    {
        std::lock_guard lock{mutex_};
        stop_ = true;
        if (running_) running_->cancel();
        for (auto& [id, work] : queued_) {
            (void)id;
            work.request->cancel();
        }
    }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void SyntaxWorker::submit(SyntaxWork work) {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock{mutex_};
        const auto key = work.document.value();
        if (auto existing = queued_.find(key); existing != queued_.end()) {
            existing->second.request->cancel();
            existing->second = std::move(work);
        } else {
            queued_.emplace(key, std::move(work));
        }
        if (runningDocument_ &&
            runningDocument_->value() == key && running_) {
            running_->cancel();
        }
    }
    ready_.notify_one();
}

void SyntaxWorker::cancel(FileDocumentId document) noexcept {
    std::lock_guard lock{mutex_};
    if (auto queued = queued_.find(document.value()); queued != queued_.end()) {
        queued->second.request->cancel();
        queued_.erase(queued);
    }
    if (runningDocument_ && *runningDocument_ == document && running_) {
        running_->cancel();
    }
}

std::deque<SyntaxCompletion> SyntaxWorker::drain() {
    readiness_.consume();
    std::lock_guard lock{mutex_};
    return std::exchange(completed_, {});
}

void SyntaxWorker::run() {
    while (true) {
        SyntaxWork work;
        {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, [&] { return stop_ || !queued_.empty(); });
            if (stop_) return;
            auto next = queued_.begin();
            work = std::move(next->second);
            queued_.erase(next);
            running_ = work.request;
            runningDocument_ = work.document;
        }

        auto output = runSyntaxParse(parser_, *work.request);

        {
            std::lock_guard lock{mutex_};
            completed_.push_back(
                {work.document, std::move(work.request), std::move(output)});
            running_.reset();
            runningDocument_.reset();
        }
        readiness_.notify();
    }
}

}  // namespace ssg
