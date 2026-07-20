#pragma once

#include "ssg/scratch_journal.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ssg {

struct ScratchStoreConfig {
    std::uintmax_t maximum_bytes = 256U * 1024U * 1024U;
    std::chrono::seconds maximum_age = std::chrono::hours{24 * 30};
    std::uintmax_t compaction_threshold_bytes = 4U * 1024U * 1024U;
    std::chrono::milliseconds durability_target{100};
};

enum class ScratchDurability {
    Durable,
    Pending,
    Failed,
};

struct ScratchDurabilityState {
    ScratchDurability kind = ScratchDurability::Durable;
    std::uint64_t accepted_generation = 0;
    std::uint64_t durable_generation = 0;
    bool overdue = false;
    std::string failure;
};

struct ScratchQuotaResult {
    std::vector<std::string> evicted_session_ids;
    std::uintmax_t remaining_bytes = 0;
    bool within_byte_quota = true;
};

class ScratchStorage {
public:
    virtual ~ScratchStorage() = default;

    virtual void append_document(const std::filesystem::path& path,
                                 const JournalDocument& document) = 0;
    virtual void append_remove(const std::filesystem::path& path,
                               const JournalDocumentKey& key) = 0;
    virtual void replace_checkpoint(
        const std::filesystem::path& path,
        const JournalRecoverySet& recovery) = 0;
};

class ScratchStore {
public:
    [[nodiscard]] static ScratchStore create(
        const std::filesystem::path& scratch_root,
        const std::filesystem::path& canonical_workspace,
        ScratchStoreConfig config = {});
    [[nodiscard]] static ScratchStore create(
        const std::filesystem::path& scratch_root,
        const std::filesystem::path& canonical_workspace,
        ScratchStoreConfig config,
        ScratchStorage& storage);

    ~ScratchStore();
    ScratchStore(ScratchStore&&) noexcept;
    ScratchStore& operator=(ScratchStore&&) noexcept;
    ScratchStore(const ScratchStore&) = delete;
    ScratchStore& operator=(const ScratchStore&) = delete;

    [[nodiscard]] JournalRecoverySet recovery() const;
    [[nodiscard]] std::filesystem::path session_path() const;
    [[nodiscard]] std::filesystem::path journal_path() const;

    void update_document(JournalDocument document);
    void remove_document(JournalDocumentKey key);
    void compact();

    [[nodiscard]] ScratchDurabilityState durability_state() const;
    [[nodiscard]] bool wait_until_durable(
        std::chrono::milliseconds timeout) const;

    [[nodiscard]] ScratchQuotaResult apply_quotas();
    [[nodiscard]] std::size_t purge_workspace();
    [[nodiscard]] std::size_t purge_all();
    void shutdown();

private:
    class Impl;
    explicit ScratchStore(std::unique_ptr<Impl> implementation) noexcept;
    [[nodiscard]] static ScratchStore create_with_storage(
        const std::filesystem::path& scratch_root,
        const std::filesystem::path& canonical_workspace,
        ScratchStoreConfig config,
        std::unique_ptr<ScratchStorage> owned_storage,
        ScratchStorage& storage);

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
