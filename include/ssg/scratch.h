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
    std::uintmax_t maximumBytes = 256U * 1024U * 1024U;
    std::chrono::seconds maximumAge = std::chrono::hours{24 * 30};
    std::uintmax_t compactionThresholdBytes = 4U * 1024U * 1024U;
    std::chrono::milliseconds durabilityTarget{100};
};

enum class ScratchDurability {
    Durable,
    Pending,
    Failed,
};

struct ScratchDurabilityState {
    ScratchDurability kind = ScratchDurability::Durable;
    std::uint64_t acceptedGeneration = 0;
    std::uint64_t durableGeneration = 0;
    bool overdue = false;
    std::string failure;
};

struct ScratchQuotaResult {
    std::vector<std::string> evictedSessionIds;
    std::uintmax_t remainingBytes = 0;
    bool withinByteQuota = true;
};

class ScratchStorage {
public:
    virtual ~ScratchStorage() = default;

    virtual void appendDocument(const std::filesystem::path& path,
                                 const JournalDocument& document) = 0;
    virtual void appendRemove(const std::filesystem::path& path,
                               const JournalDocumentKey& key) = 0;
    virtual void replaceCheckpoint(
        const std::filesystem::path& path,
        const JournalRecoverySet& recovery) = 0;
};

class ScratchStore {
public:
    [[nodiscard]] static ScratchStore create(
        const std::filesystem::path& scratchRoot,
        const std::filesystem::path& canonicalWorkspace,
        ScratchStoreConfig config = {});
    [[nodiscard]] static ScratchStore create(
        const std::filesystem::path& scratchRoot,
        const std::filesystem::path& canonicalWorkspace,
        ScratchStoreConfig config,
        ScratchStorage& storage);

    ~ScratchStore();
    ScratchStore(ScratchStore&&) noexcept;
    ScratchStore& operator=(ScratchStore&&) noexcept;
    ScratchStore(const ScratchStore&) = delete;
    ScratchStore& operator=(const ScratchStore&) = delete;

    [[nodiscard]] JournalRecoverySet recovery() const;
    [[nodiscard]] std::filesystem::path sessionPath() const;
    [[nodiscard]] std::filesystem::path journalPath() const;

    void updateDocument(JournalDocument document);
    void removeDocument(JournalDocumentKey key);
    void compact();

    [[nodiscard]] ScratchDurabilityState durabilityState() const;
    [[nodiscard]] bool waitUntilDurable(
        std::chrono::milliseconds timeout) const;

    [[nodiscard]] ScratchQuotaResult applyQuotas();
    [[nodiscard]] std::size_t purgeWorkspace();
    [[nodiscard]] std::size_t purgeAll();
    void shutdown();

private:
    class Impl;
    explicit ScratchStore(std::unique_ptr<Impl> implementation) noexcept;
    [[nodiscard]] static ScratchStore createWithStorage(
        const std::filesystem::path& scratchRoot,
        const std::filesystem::path& canonicalWorkspace,
        ScratchStoreConfig config,
        std::unique_ptr<ScratchStorage> ownedStorage,
        ScratchStorage& storage);

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
