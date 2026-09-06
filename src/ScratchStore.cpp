#include <ssg/ScratchStore.h>

#include <ssg/platform_files.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace ssg {
namespace {

void validateConfig(const ScratchStoreConfig& config) {
    if (config.maximumAge < std::chrono::seconds::zero()) {
        throw std::invalid_argument(
            "scratch maximum age must not be negative");
    }
    if (config.compactionThresholdBytes == 0) {
        throw std::invalid_argument(
            "scratch compaction threshold must be greater than zero");
    }
    if (config.durabilityTarget <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "scratch durability target must be greater than zero");
    }
}

void replaceCheckpoint(const std::filesystem::path& path,
                       const JournalRecoverySet& recovery) {
    const auto record = encodeJournalCheckpoint(recovery);
    replaceFileAtomically(path, record);
    setOwnerOnlyPermissions(path);
}

void applyDocument(JournalRecoverySet& recovery,
                    JournalDocument document) {
    const auto existing = std::find_if(
        recovery.documents.begin(), recovery.documents.end(),
        [&](const JournalDocument& candidate) {
            return candidate.key == document.key;
        });
    if (existing == recovery.documents.end()) {
        recovery.documents.push_back(std::move(document));
    } else {
        *existing = std::move(document);
    }
}

void applyRemove(JournalRecoverySet& recovery,
                  const JournalDocumentKey& key) {
    std::erase_if(recovery.documents, [&](const JournalDocument& document) {
        return document.key == key;
    });
}

std::uintmax_t directoryBytes(const std::filesystem::path& root) {
    std::uintmax_t result = 0;
    const auto listed = listDirectory(root, DirectoryTraversal::Recursive);
    if (listed.status == FileIoStatus::NotFound) return 0;
    if (!listed.ok()) return 0;
    for (const auto& entry : listed.entries) {
        const auto status = statFile(entry.path());
        if (!status || status->kind != FileKind::Regular) continue;
        if (status->size >
            std::numeric_limits<std::uintmax_t>::max() - result) {
            return std::numeric_limits<std::uintmax_t>::max();
        }
        result += status->size;
    }
    return result;
}

struct Remnant {
    std::string id;
    std::filesystem::path path;
    std::filesystem::path workspacePath;
};

std::vector<Remnant> restoredRemnants(
    const std::filesystem::path& scratchRoot) {
    std::vector<Remnant> result;
    const auto workspaces = scratchRoot / "workspaces";
    const auto workspaceEntries = listDirectory(workspaces);
    if (!workspaceEntries.ok()) return result;
    for (const auto& workspaceEntry : workspaceEntries.entries) {
        const auto workspaceStat = statFile(workspaceEntry.path());
        if (!workspaceStat ||
            workspaceStat->kind != FileKind::Directory) continue;
        const auto sessions = workspaceEntry.path() / "sessions";
        const auto sessionEntries = listDirectory(sessions);
        if (!sessionEntries.ok()) continue;
        for (const auto& sessionEntry : sessionEntries.entries) {
            const auto sessionStat = statFile(sessionEntry.path());
            if (!sessionStat ||
                sessionStat->kind != FileKind::Directory) continue;
            const auto marker = sessionEntry.path() / "restored";
            const auto markerStat = statFile(marker);
            if (!markerStat || markerStat->kind != FileKind::Regular) continue;
            result.push_back({sessionEntry.path().filename().string(),
                              sessionEntry.path(), workspaceEntry.path()});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const Remnant& left, const Remnant& right) {
                  return left.id < right.id;
              });
    return result;
}

bool olderThan(const Remnant& remnant,
                std::chrono::seconds maximumAge,
                std::chrono::system_clock::time_point now) {
    if (remnant.id.size() < 20) return false;
    std::uint64_t nanoseconds = 0;
    for (std::size_t index = 0; index < 20; ++index) {
        const char value = remnant.id[index];
        if (value < '0' || value > '9') return false;
        const auto digit = static_cast<std::uint64_t>(value - '0');
        if (nanoseconds >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        nanoseconds = nanoseconds * 10U + digit;
    }
    const auto created =
        std::chrono::system_clock::time_point{std::chrono::nanoseconds{
            nanoseconds}};
    return now - created > maximumAge;
}

void removeRemnant(const Remnant& remnant) {
    const auto removed = removeTree(remnant.path);
    if (!removed.ok() && removed.status != FileIoStatus::NotFound) {
        throw std::runtime_error("purge restored scratch remnant: " +
                                 removed.message);
    }
}

} // namespace

class ScratchStore::Impl {
public:
    enum class JobKind {
        Document,
        Remove,
        Checkpoint,
    };

    struct Job {
        JobKind kind;
        std::uint64_t generation;
        JournalRecoverySet snapshot;
        std::optional<JournalDocument> document;
        std::optional<JournalDocumentKey> key;
    };

    Impl(std::filesystem::path scratchRoot,
         ScratchStoreConfig config,
         ScratchSession session,
         JournalRecoverySet recovery)
        : scratchRoot_(std::move(scratchRoot)),
          config_(config),
          session_(std::move(session)),
          recovery_(std::move(recovery)),
          worker_([this] { run(); }) {}

    ~Impl() { shutdown(); }

    void updateDocument(JournalDocument document) {
        std::lock_guard lock{mutex_};
        requireAccepting();
        applyDocument(recovery_, document);
        enqueueLocked({JobKind::Document, ++acceptedGeneration_, recovery_,
                        std::move(document), std::nullopt});
    }

    void removeDocument(JournalDocumentKey key) {
        std::lock_guard lock{mutex_};
        requireAccepting();
        applyRemove(recovery_, key);
        enqueueLocked({JobKind::Remove, ++acceptedGeneration_, recovery_,
                        std::nullopt, std::move(key)});
    }

    void compact() {
        std::lock_guard lock{mutex_};
        requireAccepting();
        enqueueLocked({JobKind::Checkpoint, ++acceptedGeneration_, recovery_,
                        std::nullopt, std::nullopt});
    }

    JournalRecoverySet recovery() const {
        std::lock_guard lock{mutex_};
        return recovery_;
    }

    ScratchDurabilityState durabilityState() const {
        std::lock_guard lock{mutex_};
        ScratchDurabilityState result;
        result.acceptedGeneration = acceptedGeneration_;
        result.durableGeneration = durableGeneration_;
        result.failure = failure_;
        if (!failure_.empty()) {
            result.kind = ScratchDurability::Failed;
        } else if (durableGeneration_ < acceptedGeneration_) {
            result.kind = ScratchDurability::Pending;
            result.overdue =
                pendingSince_.has_value() &&
                std::chrono::steady_clock::now() - *pendingSince_ >
                    config_.durabilityTarget;
        }
        return result;
    }

    bool waitUntilDurable(std::chrono::milliseconds timeout) const {
        std::unique_lock lock{mutex_};
        const auto target = acceptedGeneration_;
        condition_.wait_for(lock, timeout, [&] {
            return !failure_.empty() || durableGeneration_ >= target;
        });
        return failure_.empty() && durableGeneration_ >= target;
    }

    ScratchQuotaResult applyQuotas() {
        ScratchQuotaResult result;
        auto remnants = restoredRemnants(scratchRoot_);
        const auto now = std::chrono::system_clock::now();
        for (const auto& remnant : remnants) {
            if (!olderThan(remnant, config_.maximumAge, now)) continue;
            removeRemnant(remnant);
            result.evictedSessionIds.push_back(remnant.id);
        }

        auto remaining = restoredRemnants(scratchRoot_);
        result.remainingBytes = directoryBytes(scratchRoot_);
        for (const auto& remnant : remaining) {
            if (result.remainingBytes <= config_.maximumBytes) break;
            removeRemnant(remnant);
            result.evictedSessionIds.push_back(remnant.id);
            result.remainingBytes = directoryBytes(scratchRoot_);
        }
        result.withinByteQuota =
            result.remainingBytes <= config_.maximumBytes;
        return result;
    }

    std::size_t purgeWorkspace() {
        const auto currentWorkspace = session_.path().parent_path().parent_path();
        return purge([&](const Remnant& remnant) {
            return remnant.workspacePath == currentWorkspace;
        });
    }

    std::size_t purgeAll() {
        return purge([](const Remnant&) { return true; });
    }

    void shutdown() {
        {
            std::lock_guard lock{mutex_};
            if (!accepting_ && !worker_.joinable()) return;
            accepting_ = false;
            stopping_ = true;
            condition_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }

    std::filesystem::path sessionPath() const { return session_.path(); }
    std::filesystem::path journalPath() const { return session_.journalPath(); }

private:
    template <typename Predicate>
    std::size_t purge(Predicate predicate) {
        std::size_t count = 0;
        for (const auto& remnant : restoredRemnants(scratchRoot_)) {
            if (!predicate(remnant)) continue;
            removeRemnant(remnant);
            ++count;
        }
        return count;
    }

    void requireAccepting() const {
        if (!accepting_) {
            throw std::logic_error("scratch store is shut down");
        }
        if (!failure_.empty()) {
            throw std::runtime_error("scratch durability failed: " + failure_);
        }
    }

    void enqueueLocked(Job job) {
        if (!pendingSince_) pendingSince_ = std::chrono::steady_clock::now();
        jobs_.push_back(std::move(job));
        condition_.notify_all();
    }

    void run() {
        for (;;) {
            Job job = [&] {
                std::unique_lock lock{mutex_};
                condition_.wait(lock,
                                [&] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return Job{JobKind::Checkpoint, 0, {}};
                Job next = std::move(jobs_.front());
                jobs_.pop_front();
                return next;
            }();
            if (job.generation == 0) break;

            try {
                switch (job.kind) {
                case JobKind::Document:
                    ScratchJournal{session_.journalPath()}.appendDocument(
                        *job.document);
                    break;
                case JobKind::Remove:
                    ScratchJournal{session_.journalPath()}.appendRemove(
                        *job.key);
                    break;
                case JobKind::Checkpoint:
                    replaceCheckpoint(session_.journalPath(), job.snapshot);
                    break;
                }
                if (job.kind != JobKind::Checkpoint) {
                    const auto journal = statFile(session_.journalPath());
                    if (journal &&
                        journal->size >= config_.compactionThresholdBytes) {
                        replaceCheckpoint(session_.journalPath(), job.snapshot);
                    }
                }
            } catch (const std::exception& error) {
                std::lock_guard lock{mutex_};
                failure_ = error.what();
                jobs_.clear();
                condition_.notify_all();
                continue;
            } catch (...) {
                std::lock_guard lock{mutex_};
                failure_ = "unknown scratch storage failure";
                jobs_.clear();
                condition_.notify_all();
                continue;
            }

            std::lock_guard lock{mutex_};
            durableGeneration_ = job.generation;
            if (durableGeneration_ == acceptedGeneration_) {
                pendingSince_.reset();
            }
            condition_.notify_all();
        }
    }

    std::filesystem::path scratchRoot_;
    ScratchStoreConfig config_;
    ScratchSession session_;
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    JournalRecoverySet recovery_;
    std::deque<Job> jobs_;
    std::uint64_t acceptedGeneration_ = 0;
    std::uint64_t durableGeneration_ = 0;
    std::optional<std::chrono::steady_clock::time_point> pendingSince_;
    std::string failure_;
    bool accepting_ = true;
    bool stopping_ = false;
    std::thread worker_;
};

ScratchStore ScratchStore::create(
    const std::filesystem::path& scratchRoot,
    const std::filesystem::path& canonicalWorkspace,
    ScratchStoreConfig config) {
    validateConfig(config);
    auto session = ScratchSession::create(scratchRoot, canonicalWorkspace);
    JournalRecoverySet recovery;
    if (auto remnant = session.claimNewestRestorable()) {
        recovery = remnant->replay().recovery;
        replaceCheckpoint(session.journalPath(), recovery);
        remnant->markRestored();
    }
    return ScratchStore{std::make_unique<ScratchStore::Impl>(
        scratchRoot, config, std::move(session), std::move(recovery))};
}

ScratchStore::ScratchStore(std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

ScratchStore::~ScratchStore() = default;
ScratchStore::ScratchStore(ScratchStore&&) noexcept = default;
ScratchStore& ScratchStore::operator=(ScratchStore&&) noexcept = default;

JournalRecoverySet ScratchStore::recovery() const { return impl_->recovery(); }
std::filesystem::path ScratchStore::sessionPath() const {
    return impl_->sessionPath();
}
std::filesystem::path ScratchStore::journalPath() const {
    return impl_->journalPath();
}
void ScratchStore::updateDocument(JournalDocument document) {
    impl_->updateDocument(std::move(document));
}
void ScratchStore::removeDocument(JournalDocumentKey key) {
    impl_->removeDocument(std::move(key));
}
void ScratchStore::compact() { impl_->compact(); }
ScratchDurabilityState ScratchStore::durabilityState() const {
    return impl_->durabilityState();
}
bool ScratchStore::waitUntilDurable(
    std::chrono::milliseconds timeout) const {
    return impl_->waitUntilDurable(timeout);
}
ScratchQuotaResult ScratchStore::applyQuotas() {
    return impl_->applyQuotas();
}
std::size_t ScratchStore::purgeWorkspace() {
    return impl_->purgeWorkspace();
}
std::size_t ScratchStore::purgeAll() { return impl_->purgeAll(); }
void ScratchStore::shutdown() { impl_->shutdown(); }

} // namespace ssg
