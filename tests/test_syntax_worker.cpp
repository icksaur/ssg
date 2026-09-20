#include <ssg/SyntaxWorker.h>

#include "test_helpers.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace {

using namespace ssg;

class WorkerParse final : public OpaqueSyntaxParse {};

class WorkerParser final : public SyntaxParser {
public:
    bool grammarAvailable = true;
    bool fail = false;
    bool throwOnParse = false;
    bool blockFirst = false;

    bool hasGrammar(const LanguageId&) const override {
        return grammarAvailable;
    }

    SyntaxParseOutput parse(const SyntaxParseRequest& request) override {
        const auto call = calls_.fetch_add(1);
        entered_.release();
        if (blockFirst && call == 0) release_.acquire();
        if (throwOnParse) throw std::runtime_error{"parser failure"};

        SyntaxParseOutput output{
            .revision = request.revision(),
            .status = fail ? SyntaxParseStatus::Failed
                           : SyntaxParseStatus::Parsed,
        };
        if (request.cancelled()) {
            output.status = SyntaxParseStatus::Cancelled;
        } else if (!fail) {
            output.parse = std::make_shared<WorkerParse>();
        }
        return output;
    }

    void waitUntilEntered() { entered_.acquire(); }
    void releaseFirst() { release_.release(); }

private:
    std::atomic<std::size_t> calls_{0};
    std::counting_semaphore<> entered_{0};
    std::binary_semaphore release_{0};
};

std::shared_ptr<const SyntaxParseRequest> request(
    SyntaxModel& model, std::uint64_t revision, std::string text = "text") {
    auto prepared =
        model.request(revision, LanguageId{"toy"}, std::move(text));
    ASSERT_TRUE(prepared.accepted());
    return prepared.request;
}

std::vector<SyntaxCompletion> waitForCompletions(
    SyntaxWorker& worker, std::size_t count) {
    std::vector<SyntaxCompletion> completions;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (completions.size() < count &&
           std::chrono::steady_clock::now() < deadline) {
        auto drained = worker.drain();
        for (auto& completion : drained) {
            completions.push_back(std::move(completion));
        }
        if (completions.size() < count) std::this_thread::yield();
    }
    return completions;
}

TEST(latestQueuedRequestWinsPerDocumentAndPreservesIdentity) {
    auto parser = std::make_shared<WorkerParser>();
    parser->blockFirst = true;
    SyntaxWorker worker{parser};

    SyntaxModel firstModel{parser};
    SyntaxModel secondModel{parser};
    auto first = request(firstModel, 1, "first");
    auto superseded = request(secondModel, 1, "old");
    worker.submit({FileDocumentId{1}, first});
    parser->waitUntilEntered();
    worker.submit({FileDocumentId{2}, superseded});
    auto latest = request(secondModel, 2, "new");
    worker.submit({FileDocumentId{2}, latest});
    ASSERT_TRUE(superseded->cancelled());

    parser->releaseFirst();
    auto completions = waitForCompletions(worker, 2);
    ASSERT_EQ(completions.size(), std::size_t{2});
    if (completions.size() != 2) return;
    ASSERT_TRUE(completions[0].request == first);
    ASSERT_TRUE(completions[1].request == latest);
    ASSERT_EQ(completions[1].document, FileDocumentId{2});
}

TEST(cancelRemovesQueuedWorkAndCancelsRunningWork) {
    auto parser = std::make_shared<WorkerParser>();
    parser->blockFirst = true;
    SyntaxWorker worker{parser};
    SyntaxModel runningModel{parser};
    SyntaxModel queuedModel{parser};
    auto running = request(runningModel, 1);
    auto queued = request(queuedModel, 1);

    worker.submit({FileDocumentId{1}, running});
    parser->waitUntilEntered();
    worker.submit({FileDocumentId{2}, queued});
    worker.cancel(FileDocumentId{1});
    worker.cancel(FileDocumentId{2});
    ASSERT_TRUE(running->cancelled());
    ASSERT_TRUE(queued->cancelled());

    parser->releaseFirst();
    auto completions = waitForCompletions(worker, 1);
    ASSERT_EQ(completions.size(), std::size_t{1});
    if (completions.empty()) return;
    ASSERT_EQ(completions.front().output.status,
              SyntaxParseStatus::Cancelled);
}

TEST(workerAndDirectExecutionShareStatusMapping) {
    for (auto scenario = 0; scenario < 3; ++scenario) {
        auto parser = std::make_shared<WorkerParser>();
        parser->grammarAvailable = scenario != 1;
        parser->throwOnParse = scenario == 2;

        SyntaxModel directModel{parser};
        auto directRequest = request(directModel, 1);
        const auto direct = runSyntaxParse(parser, *directRequest);

        SyntaxModel workerModel{parser};
        auto workerRequest = request(workerModel, 1);
        SyntaxWorker worker{parser};
        worker.submit({FileDocumentId{1}, workerRequest});
        auto completions = waitForCompletions(worker, 1);
        ASSERT_EQ(completions.size(), std::size_t{1});
        if (completions.empty()) continue;
        ASSERT_EQ(completions.front().output.status, direct.status);
        ASSERT_EQ(completions.front().output.revision, direct.revision);
    }
}

TEST(destructionCancelsRunningRequestBeforeJoining) {
    auto parser = std::make_shared<WorkerParser>();
    parser->blockFirst = true;
    auto worker = std::make_unique<SyntaxWorker>(parser);
    SyntaxModel model{parser};
    auto running = request(model, 1);
    worker->submit({FileDocumentId{1}, running});
    parser->waitUntilEntered();

    std::thread destroyer{[&] { worker.reset(); }};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!running->cancelled() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(running->cancelled());
    parser->releaseFirst();
    destroyer.join();
}

}  // namespace

SSG_TEST_SUITE(test_syntax_worker) {
    RUN(latestQueuedRequestWinsPerDocumentAndPreservesIdentity);
    RUN(cancelRemovesQueuedWorkAndCancelsRunningWork);
    RUN(workerAndDirectExecutionShareStatusMapping);
    RUN(destructionCancelsRunningRequestBeforeJoining);
    return failed == 0 ? 0 : 1;
}
