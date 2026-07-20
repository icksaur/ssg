#include "ssg/scratch.h"

#include "ssg/platform_files.h"
#include "ssg/scratch_session.h"

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

class FilesystemScratchStorage final : public ScratchStorage {
public:
    void append_document(const std::filesystem::path& path,
                         const JournalDocument& document) override {
        ScratchJournal{path}.append_document(document);
    }

    void append_remove(const std::filesystem::path& path,
                       const JournalDocumentKey& key) override {
        ScratchJournal{path}.append_remove(key);
    }

    void replace_checkpoint(
        const std::filesystem::path& path,
        const JournalRecoverySet& recovery) override {
        const auto record = encode_checkpoint_record(recovery);
        replace_file_atomically(path, record);
        set_owner_only_permissions(path);
    }
};

void validate_config(const ScratchStoreConfig& config) {
    if (config.maximum_age < std::chrono::seconds::zero()) {
        throw std::invalid_argument(
            "scratch maximum age must not be negative");
    }
    if (config.compaction_threshold_bytes == 0) {
        throw std::invalid_argument(
            "scratch compaction threshold must be greater than zero");
    }
    if (config.durability_target <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "scratch durability target must be greater than zero");
    }
}

void apply_document(JournalRecoverySet& recovery,
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

void apply_remove(JournalRecoverySet& recovery,
                  const JournalDocumentKey& key) {
    std::erase_if(recovery.documents, [&](const JournalDocument& document) {
        return document.key == key;
    });
}

std::uintmax_t directory_bytes(const std::filesystem::path& root) {
    std::uintmax_t result = 0;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) return 0;
    for (std::filesystem::recursive_directory_iterator iterator{
             root, std::filesystem::directory_options::skip_permission_denied,
             error},
         end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        const auto size = iterator->file_size(error);
        if (!error) {
            if (size > std::numeric_limits<std::uintmax_t>::max() - result) {
                return std::numeric_limits<std::uintmax_t>::max();
            }
            result += size;
        }
        error.clear();
    }
    return result;
}

struct Remnant {
    std::string id;
    std::filesystem::path path;
    std::filesystem::path workspace_path;
};

std::vector<Remnant> restored_remnants(
    const std::filesystem::path& scratch_root) {
    std::vector<Remnant> result;
    const auto workspaces = scratch_root / "workspaces";
    std::error_code error;
    for (std::filesystem::directory_iterator workspace_iterator{workspaces,
                                                                 error},
         workspace_end;
         !error && workspace_iterator != workspace_end;
         workspace_iterator.increment(error)) {
        if (!workspace_iterator->is_directory()) continue;
        const auto sessions = workspace_iterator->path() / "sessions";
        std::error_code session_error;
        for (std::filesystem::directory_iterator session_iterator{sessions,
                                                                   session_error},
             session_end;
             !session_error && session_iterator != session_end;
             session_iterator.increment(session_error)) {
            if (!session_iterator->is_directory()) continue;
            const auto marker = session_iterator->path() / "restored";
            if (!std::filesystem::is_regular_file(marker)) continue;
            result.push_back({session_iterator->path().filename().string(),
                              session_iterator->path(),
                              workspace_iterator->path()});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const Remnant& left, const Remnant& right) {
                  return left.id < right.id;
              });
    return result;
}

bool older_than(const Remnant& remnant,
                std::chrono::seconds maximum_age,
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
    return now - created > maximum_age;
}

void remove_remnant(const Remnant& remnant) {
    std::error_code error;
    std::filesystem::remove_all(remnant.path, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "purge restored scratch remnant", remnant.path, error);
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

    Impl(std::filesystem::path scratch_root,
         ScratchStoreConfig config,
         ScratchSession session,
         JournalRecoverySet recovery,
         std::unique_ptr<ScratchStorage> owned_storage,
         ScratchStorage& storage)
        : scratch_root_(std::move(scratch_root)),
          config_(config),
          session_(std::move(session)),
          recovery_(std::move(recovery)),
          owned_storage_(std::move(owned_storage)),
          storage_(storage),
          worker_([this] { run(); }) {}

    ~Impl() { shutdown(); }

    void update_document(JournalDocument document) {
        std::lock_guard lock{mutex_};
        require_accepting();
        apply_document(recovery_, document);
        enqueue_locked({JobKind::Document, ++accepted_generation_, recovery_,
                        std::move(document), std::nullopt});
    }

    void remove_document(JournalDocumentKey key) {
        std::lock_guard lock{mutex_};
        require_accepting();
        apply_remove(recovery_, key);
        enqueue_locked({JobKind::Remove, ++accepted_generation_, recovery_,
                        std::nullopt, std::move(key)});
    }

    void compact() {
        std::lock_guard lock{mutex_};
        require_accepting();
        enqueue_locked({JobKind::Checkpoint, ++accepted_generation_, recovery_,
                        std::nullopt, std::nullopt});
    }

    JournalRecoverySet recovery() const {
        std::lock_guard lock{mutex_};
        return recovery_;
    }

    ScratchDurabilityState durability_state() const {
        std::lock_guard lock{mutex_};
        ScratchDurabilityState result;
        result.accepted_generation = accepted_generation_;
        result.durable_generation = durable_generation_;
        result.failure = failure_;
        if (!failure_.empty()) {
            result.kind = ScratchDurability::Failed;
        } else if (durable_generation_ < accepted_generation_) {
            result.kind = ScratchDurability::Pending;
            result.overdue =
                pending_since_.has_value() &&
                std::chrono::steady_clock::now() - *pending_since_ >
                    config_.durability_target;
        }
        return result;
    }

    bool wait_until_durable(std::chrono::milliseconds timeout) const {
        std::unique_lock lock{mutex_};
        const auto target = accepted_generation_;
        condition_.wait_for(lock, timeout, [&] {
            return !failure_.empty() || durable_generation_ >= target;
        });
        return failure_.empty() && durable_generation_ >= target;
    }

    ScratchQuotaResult apply_quotas() {
        ScratchQuotaResult result;
        auto remnants = restored_remnants(scratch_root_);
        const auto now = std::chrono::system_clock::now();
        for (const auto& remnant : remnants) {
            if (!older_than(remnant, config_.maximum_age, now)) continue;
            remove_remnant(remnant);
            result.evicted_session_ids.push_back(remnant.id);
        }

        auto remaining = restored_remnants(scratch_root_);
        result.remaining_bytes = directory_bytes(scratch_root_);
        for (const auto& remnant : remaining) {
            if (result.remaining_bytes <= config_.maximum_bytes) break;
            remove_remnant(remnant);
            result.evicted_session_ids.push_back(remnant.id);
            result.remaining_bytes = directory_bytes(scratch_root_);
        }
        result.within_byte_quota =
            result.remaining_bytes <= config_.maximum_bytes;
        return result;
    }

    std::size_t purge_workspace() {
        const auto current_workspace = session_.path().parent_path().parent_path();
        return purge([&](const Remnant& remnant) {
            return remnant.workspace_path == current_workspace;
        });
    }

    std::size_t purge_all() {
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

    std::filesystem::path session_path() const { return session_.path(); }
    std::filesystem::path journal_path() const { return session_.journal_path(); }

private:
    template <typename Predicate>
    std::size_t purge(Predicate predicate) {
        std::size_t count = 0;
        for (const auto& remnant : restored_remnants(scratch_root_)) {
            if (!predicate(remnant)) continue;
            remove_remnant(remnant);
            ++count;
        }
        return count;
    }

    void require_accepting() const {
        if (!accepting_) {
            throw std::logic_error("scratch store is shut down");
        }
        if (!failure_.empty()) {
            throw std::runtime_error("scratch durability failed: " + failure_);
        }
    }

    void enqueue_locked(Job job) {
        if (!pending_since_) pending_since_ = std::chrono::steady_clock::now();
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
                    storage_.append_document(session_.journal_path(),
                                             *job.document);
                    break;
                case JobKind::Remove:
                    storage_.append_remove(session_.journal_path(), *job.key);
                    break;
                case JobKind::Checkpoint:
                    storage_.replace_checkpoint(session_.journal_path(),
                                                job.snapshot);
                    break;
                }
                if (job.kind != JobKind::Checkpoint) {
                    std::error_code error;
                    const auto bytes =
                        std::filesystem::file_size(session_.journal_path(),
                                                   error);
                    if (!error &&
                        bytes >= config_.compaction_threshold_bytes) {
                        storage_.replace_checkpoint(session_.journal_path(),
                                                    job.snapshot);
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
            durable_generation_ = job.generation;
            if (durable_generation_ == accepted_generation_) {
                pending_since_.reset();
            }
            condition_.notify_all();
        }
    }

    std::filesystem::path scratch_root_;
    ScratchStoreConfig config_;
    ScratchSession session_;
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    JournalRecoverySet recovery_;
    std::deque<Job> jobs_;
    std::uint64_t accepted_generation_ = 0;
    std::uint64_t durable_generation_ = 0;
    std::optional<std::chrono::steady_clock::time_point> pending_since_;
    std::string failure_;
    bool accepting_ = true;
    bool stopping_ = false;
    std::unique_ptr<ScratchStorage> owned_storage_;
    ScratchStorage& storage_;
    std::thread worker_;
};

ScratchStore ScratchStore::create_with_storage(
    const std::filesystem::path& scratch_root,
    const std::filesystem::path& canonical_workspace,
    ScratchStoreConfig config,
    std::unique_ptr<ScratchStorage> owned_storage,
    ScratchStorage& storage) {
    validate_config(config);
    auto session = ScratchSession::create(scratch_root, canonical_workspace);
    JournalRecoverySet recovery;
    if (auto remnant = session.claim_newest_restorable()) {
        recovery = remnant->replay().recovery;
        storage.replace_checkpoint(session.journal_path(), recovery);
        set_owner_only_permissions(session.journal_path());
        remnant->mark_restored();
    }
    return ScratchStore{std::make_unique<ScratchStore::Impl>(
        scratch_root, config, std::move(session), std::move(recovery),
        std::move(owned_storage), storage)};
}

ScratchStore ScratchStore::create(
    const std::filesystem::path& scratch_root,
    const std::filesystem::path& canonical_workspace,
    ScratchStoreConfig config) {
    auto storage = std::make_unique<FilesystemScratchStorage>();
    auto& reference = *storage;
    return create_with_storage(scratch_root, canonical_workspace, config,
                               std::move(storage), reference);
}

ScratchStore ScratchStore::create(
    const std::filesystem::path& scratch_root,
    const std::filesystem::path& canonical_workspace,
    ScratchStoreConfig config,
    ScratchStorage& storage) {
    return create_with_storage(scratch_root, canonical_workspace, config,
                               nullptr, storage);
}

ScratchStore::ScratchStore(std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

ScratchStore::~ScratchStore() = default;
ScratchStore::ScratchStore(ScratchStore&&) noexcept = default;
ScratchStore& ScratchStore::operator=(ScratchStore&&) noexcept = default;

JournalRecoverySet ScratchStore::recovery() const { return impl_->recovery(); }
std::filesystem::path ScratchStore::session_path() const {
    return impl_->session_path();
}
std::filesystem::path ScratchStore::journal_path() const {
    return impl_->journal_path();
}
void ScratchStore::update_document(JournalDocument document) {
    impl_->update_document(std::move(document));
}
void ScratchStore::remove_document(JournalDocumentKey key) {
    impl_->remove_document(std::move(key));
}
void ScratchStore::compact() { impl_->compact(); }
ScratchDurabilityState ScratchStore::durability_state() const {
    return impl_->durability_state();
}
bool ScratchStore::wait_until_durable(
    std::chrono::milliseconds timeout) const {
    return impl_->wait_until_durable(timeout);
}
ScratchQuotaResult ScratchStore::apply_quotas() {
    return impl_->apply_quotas();
}
std::size_t ScratchStore::purge_workspace() {
    return impl_->purge_workspace();
}
std::size_t ScratchStore::purge_all() { return impl_->purge_all(); }
void ScratchStore::shutdown() { impl_->shutdown(); }

} // namespace ssg
