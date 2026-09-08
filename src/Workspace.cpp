#include <ssg/Workspace.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace ssg {
namespace {

constexpr std::size_t kMaximumRecentFiles = 32;
constexpr std::size_t kMaximumDropLabelBytes = 255;

WorkspaceResult failure(WorkspaceError error, std::string message) {
    WorkspaceResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path) {
    auto result = readFile(path);
    if (!result.ok()) {
        throw std::runtime_error("failed to open file for reading: " +
                                 path.string() + ": " + result.message);
    }
    return std::move(result.bytes);
}

// The disk baseline a document's edits branch from: the mtime and size of the
// on-disk file plus a fast hash of `diskBytes` (the exact bytes just read from,
// or written to, that file). Best-effort — nullopt if the file cannot be stat'd,
// which draft recovery treats as "unknown baseline" (a conflict), never as
// "unchanged". `diskBytes` must be the literal file bytes, not the decoded
// buffer, so the hash matches a later re-read of the same file.
std::optional<DraftBaseline> captureDiskBaseline(
    const std::filesystem::path& absolute,
    std::span<const std::uint8_t> diskBytes) {
    std::optional<FileStat> status;
    try {
        status = statFile(absolute);
    } catch (const std::system_error&) {
        return std::nullopt;
    }
    if (!status) return std::nullopt;
    DraftBaseline baseline;
    baseline.mtimeNanos = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            status->mtime.time_since_epoch())
            .count());
    baseline.size = static_cast<std::uint64_t>(diskBytes.size());
    baseline.contentHash = fastContentHash(std::string_view{
        reinterpret_cast<const char*>(diskBytes.data()), diskBytes.size()});
    return baseline;
}

std::span<const std::byte> asBytes(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::span<const std::uint8_t> asUnsignedBytes(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return {bytes.data(), bytes.size()};
}

bool containsNul(std::string_view text) {
    return text.find('\0') != std::string_view::npos;
}

bool isBeneath(const std::filesystem::path& root,
                const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) {
            return false;
        }
    }
    return true;
}

PathSyntax nativeSyntax() noexcept {
#ifdef _WIN32
    return PathSyntax::windows;
#else
    return PathSyntax::Linux;
#endif
}

std::string normalizedRelative(std::string_view path) {
    return std::filesystem::path{path}.lexically_normal().generic_string();
}

LineTerminator defaultTerminator(const TextEncodingStatus& status) {
    switch (status.lineEnding) {
        case LineEnding::Crlf:
            return LineTerminator::Crlf;
        case LineEnding::Cr:
            return LineTerminator::Cr;
        case LineEnding::Lf:
        case LineEnding::Mixed:
            return LineTerminator::Lf;
    }
    return LineTerminator::Lf;
}

LineTerminator lineEndingTerminator(LineEnding lineEnding) {
    switch (lineEnding) {
        case LineEnding::Crlf:
            return LineTerminator::Crlf;
        case LineEnding::Cr:
            return LineTerminator::Cr;
        case LineEnding::Lf:
        case LineEnding::Mixed:
            return LineTerminator::Lf;
    }
    return LineTerminator::Lf;
}

DecodedText textForSave(const DecodedText& original,
                          std::string currentText) {
    if (currentText == original.utf8) {
        return original;
    }
    DecodedText result;
    result.utf8 = std::move(currentText);
    result.status = original.status;
    result.lineTerminators.clear();
    std::size_t line = 0;
    for (const char value : result.utf8) {
        if (value != '\n') {
            continue;
        }
        auto terminator = defaultTerminator(result.status);
        if (line < original.lineTerminators.size() &&
            original.lineTerminators[line] != LineTerminator::None) {
            terminator = original.lineTerminators[line];
        }
        result.lineTerminators.push_back(terminator);
        ++line;
    }
    if (!result.utf8.empty() && result.utf8.back() != '\n') {
        result.lineTerminators.push_back(LineTerminator::None);
    }
    result.status.finalNewline =
        !result.utf8.empty() && result.utf8.back() == '\n';
    return result;
}

void applyTerminatorEdits(DecodedText& decoded, std::string_view preEditText,
                          const EditTransaction& transaction) {
    struct AnnotatedByte {
        char value;
        LineTerminator terminator = LineTerminator::None;
    };
    std::vector<AnnotatedByte> bytes;
    bytes.reserve(preEditText.size());
    std::size_t terminatorIndex = 0;
    for (const char value : preEditText) {
        auto terminator = LineTerminator::None;
        if (value == '\n') {
            terminator = decoded.lineTerminators[terminatorIndex++];
        }
        bytes.push_back({value, terminator});
    }
    const auto insertedTerminator = defaultTerminator(decoded.status);
    std::vector<const TextEdit*> ordered;
    ordered.reserve(transaction.edits.size());
    for (const auto& edit : transaction.edits) {
        ordered.push_back(&edit);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const TextEdit* left, const TextEdit* right) {
                  return left->offset.value() < right->offset.value();
              });
    for (auto edit = ordered.rbegin(); edit != ordered.rend(); ++edit) {
        const auto begin =
            static_cast<std::size_t>((*edit)->offset.value());
        const auto end =
            begin + static_cast<std::size_t>((*edit)->erasedBytes);
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                    bytes.begin() + static_cast<std::ptrdiff_t>(end));
        std::vector<AnnotatedByte> inserted;
        inserted.reserve((*edit)->insertedText.size());
        for (const char value : (*edit)->insertedText) {
            inserted.push_back(
                {value, value == '\n' ? insertedTerminator
                                      : LineTerminator::None});
        }
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     inserted.begin(), inserted.end());
    }
    decoded.lineTerminators.clear();
    for (const auto byte : bytes) {
        if (byte.value == '\n') {
            decoded.lineTerminators.push_back(byte.terminator);
        }
    }
    if (!bytes.empty() && bytes.back().value != '\n') {
        decoded.lineTerminators.push_back(LineTerminator::None);
    }
    decoded.status.finalNewline =
        !bytes.empty() && bytes.back().value == '\n';
    std::optional<LineTerminator> uniform;
    bool mixed = false;
    for (const auto terminator : decoded.lineTerminators) {
        if (terminator == LineTerminator::None) {
            continue;
        }
        if (!uniform) {
            uniform = terminator;
        } else if (*uniform != terminator) {
            mixed = true;
        }
    }
    if (mixed) {
        decoded.status.lineEnding = LineEnding::Mixed;
    } else if (uniform == LineTerminator::Crlf) {
        decoded.status.lineEnding = LineEnding::Crlf;
    } else if (uniform == LineTerminator::Cr) {
        decoded.status.lineEnding = LineEnding::Cr;
    } else {
        decoded.status.lineEnding = LineEnding::Lf;
    }
}

std::string sanitizeLabel(std::string_view suggested) {
    auto label = std::filesystem::path{suggested}.filename().string();
    label.erase(std::remove_if(label.begin(), label.end(), [](char value) {
                    const auto byte = static_cast<unsigned char>(value);
                    return byte < 0x20 || value == '/' || value == '\\';
                }),
                label.end());
    if (label.size() > kMaximumDropLabelBytes) {
        label.resize(kMaximumDropLabelBytes);
        while (!label.empty() &&
               (static_cast<unsigned char>(label.back()) & 0xc0U) == 0x80U) {
            label.pop_back();
        }
    }
    return label.empty() ? "Dropped content" : label;
}

}  // namespace

bool containsBinaryNul(
    std::span<const std::uint8_t> bytes) noexcept {
    return std::find(bytes.begin(), bytes.end(), std::uint8_t{0}) !=
           bytes.end();
}

class Workspace::Impl {
public:
    // The document's authoritative external baseline: the disk state its edits
    // branch from. Present carries the branched-from bytes' size+hash+mtime;
    // Missing means the file was observed absent (a keep_buffer dismissal of a
    // removal). An absent `present` with `missing==false` is an unknown baseline
    // (a best-effort stat failed on open), which draft recovery treats as a
    // conflict. Open/save/reload/rename set Present-from-disk; keep_buffer advances
    // it to Present(dismissed bytes) or Missing. Implicitly constructs from the
    // optional<DraftBaseline> the capture path produces, so those sites are
    // unchanged.
    struct ExternalBaseline {
        std::optional<DraftBaseline> present;
        bool missing = false;

        ExternalBaseline() = default;
        ExternalBaseline(std::optional<DraftBaseline> disk)  // NOLINT: implicit
            : present(std::move(disk)) {}
        static ExternalBaseline removed() {
            ExternalBaseline value;
            value.missing = true;
            return value;
        }

        friend bool operator==(const ExternalBaseline&,
                               const ExternalBaseline&) = default;
    };

    struct Entry {
        FileDocumentId id;
        JournalDocumentKey key;
        std::string displayLabel;
        FileContentKind contentKind;
        // utf8 is the disk baseline; lineTerminators follow the live document.
        DecodedText decoded;
        std::vector<std::uint8_t> rawBytes;
        Document document;
        TextEncodingStatus persistedStatus;
        ExternalBaseline baseline;
        // False for a document named on the command line whose file does not
        // exist yet: it has a path but has never been written. Such a document
        // is unsaved regardless of content, and its first write must not clobber
        // a file that appeared at that path in the meantime.
        bool persisted = true;
    };

    struct CompensationState {
        FileDocumentId id;
        JournalDocumentKey priorKey;
        std::string priorLabel;
        std::optional<std::filesystem::path> archivedPath;
    };

    Impl(std::filesystem::path canonicalRoot, RecoveryManager& actions,
         std::filesystem::path archiveRoot)
        : root(std::move(canonicalRoot)),
          recovery(actions),
          archive(std::move(archiveRoot)) {}

    std::filesystem::path root;
    RecoveryManager& recovery;
    // The durable home for deleted files, separate from `recovery` because that
    // is a bounded evicting undo ring (see FileArchive.h).
    FileArchive archive;
    std::uint64_t nextDocument = 1;
    std::vector<Entry> entries;
    std::vector<std::string> recent;
    std::unordered_map<std::string, CompensationState> compensations;
    // Notified with the relative path each time saveTo writes a file, so the
    // runtime can correlate SSG's own writes against watcher events. Installed by
    // the runtime; nullptr in isolation (Workspace tests do not observe saves).
    std::function<void(const std::filesystem::path&)> saveObserver;

    Entry* find(FileDocumentId id) {
        const auto found = std::find_if(
            entries.begin(), entries.end(),
            [id](const Entry& entry) { return entry.id == id; });
        return found == entries.end() ? nullptr : &*found;
    }

    const Entry* find(FileDocumentId id) const {
        return const_cast<Impl*>(this)->find(id);
    }

    Entry* findPath(std::string_view path) {
        const auto found = std::find_if(entries.begin(), entries.end(),
                                        [path](const Entry& entry) {
                                            return entry.key.kind() ==
                                                       JournalDocumentKeyKind::
                                                           Saved &&
                                                   entry.key.savedPath() ==
                                                       path;
                                        });
        return found == entries.end() ? nullptr : &*found;
    }

    std::optional<std::filesystem::path> resolve(std::string_view raw,
                                                 bool mustExist,
                                                 WorkspaceResult& error) const {
        const auto supplied = std::filesystem::path{raw};
        const auto traverses = std::find(supplied.begin(), supplied.end(),
                                         std::filesystem::path{".."}) !=
                                supplied.end();
        const auto normalized = normalizedRelative(raw);
        const auto validation =
            validateWorkspaceRelativePath(normalized, nativeSyntax());
        if (supplied.is_absolute() || traverses || !validation.valid()) {
            error = failure(WorkspaceError::InvalidPath,
                            "path must be workspace-relative and contain no "
                            "traversal");
            return std::nullopt;
        }
        const auto relative = std::filesystem::path{normalized};
        std::error_code code;
        const auto candidate = mustExist
                                   ? weaklyCanonicalPath(root / relative, code)
                                   : weaklyCanonicalPath(
                                         root / relative.parent_path(), code) /
                                         relative.filename();
        if (code) {
            error = failure(WorkspaceError::IoFailed, code.message());
            return std::nullopt;
        }
        if (!isBeneath(root, candidate)) {
            error = failure(WorkspaceError::PathOutsideWorkspace,
                            "path resolves outside the workspace");
            return std::nullopt;
        }
        if (mustExist) {
            try {
                if (!statFile(candidate)) {
                    error =
                        failure(WorkspaceError::NotFound, "path does not exist");
                    return std::nullopt;
                }
            } catch (const std::system_error& exception) {
                error = failure(WorkspaceError::IoFailed, exception.what());
                return std::nullopt;
            }
        }
        return candidate;
    }

    void touchRecent(std::string path) {
        recent.erase(std::remove(recent.begin(), recent.end(), path),
                     recent.end());
        recent.insert(recent.begin(), std::move(path));
        if (recent.size() > kMaximumRecentFiles) {
            recent.resize(kMaximumRecentFiles);
        }
    }

    void rememberCompensation(const RecoveryRecordId& id,
                              CompensationState state) {
        compensations.insert_or_assign(std::string{id.value()},
                                       std::move(state));
        const auto retained = recovery.records();
        std::erase_if(compensations, [&](const auto& item) {
            return std::none_of(
                retained.begin(), retained.end(), [&](const RecoveryRecord& record) {
                    return record.id.value() == item.first;
                });
        });
    }

    WorkspaceResult addBytes(std::vector<std::uint8_t> bytes,
                              JournalDocumentKey key,
                              std::string label,
                              DocumentMode mode = DocumentMode::Edit) {
        const auto id = FileDocumentId{nextDocument++};
        const bool hasNul = containsBinaryNul(asUnsignedBytes(bytes));
        if (hasNul) {
            entries.push_back({id, std::move(key), std::move(label),
                               FileContentKind::Binary, {}, std::move(bytes),
                               Document{"", DocumentMode::ReadOnly}, {}});
        } else {
            auto decoded = decodeText(asUnsignedBytes(bytes));
            if (!decoded.accepted()) {
                entries.push_back(
                    {id, std::move(key), std::move(label),
                     FileContentKind::DecodeFailure, {}, std::move(bytes),
                     Document{"", DocumentMode::ReadOnly}, {}});
            } else {
                auto proof = decoded.validated();
                const auto persistedStatus = decoded.text->status;
                Document document{std::move(proof), mode};
                entries.push_back(
                    {id, std::move(key), std::move(label),
                     FileContentKind::Text, std::move(*decoded.text),
                     std::move(bytes), std::move(document), persistedStatus});
            }
        }
        WorkspaceResult result;
        result.document = id;
        return result;
    }

    // `mayOverwrite` distinguishes saving a document over the path it already
    // lives at (allowed, and the only allowed overwrite) from writing to a name
    // the user just supplied (must never clobber). Passing the intent in is
    // what lets one function serve both without guessing.
    WorkspaceResult saveTo(Entry& entry, std::string path,
                            const std::filesystem::path& absolute,
                            bool mayOverwrite) {
        if (entry.contentKind != FileContentKind::Text) {
            return failure(WorkspaceError::ReadOnly,
                           "read-only content cannot be saved");
        }
        const auto current = entry.document.snapshot().text;
        auto decoded = textForSave(entry.decoded, current);
        const auto encoded = encodeText(decoded);
        if (!encoded.accepted()) {
            return failure(WorkspaceError::DecodeFailed,
                           encoded.error->message);
        }
        try {
            if (!mayOverwrite) {
                // The filesystem decides whether the name was taken, so a file
                // created between here and the write cannot be destroyed. An
                // exists() check followed by a write would lose that race
                // silently, which is the whole reason for the clash rule.
                const auto created = createFileExclusively(
                    absolute, asBytes(encoded.bytes));
                if (!created.ok()) {
                    return failure(
                        created.status == FileIoStatus::AlreadyExists
                            ? WorkspaceError::AlreadyOpen
                            : WorkspaceError::IoFailed,
                        created.status == FileIoStatus::AlreadyExists
                            ? "destination already exists"
                            : created.message);
                }
            } else {
                replaceFileAtomically(absolute, asBytes(encoded.bytes));
            }
            entry.key = JournalDocumentKey::saved(path);
            entry.persisted = true;
            entry.displayLabel =
                std::filesystem::path{path}.filename().string();
            entry.decoded = std::move(decoded);
            entry.rawBytes = encoded.bytes;
            entry.persistedStatus = entry.decoded.status;
            // The just-written disk bytes become the new branched-from baseline,
            // so a draft made after this save is compared against what we wrote.
            entry.baseline = captureDiskBaseline(absolute, entry.rawBytes);
            if (saveObserver) {
                // Ordered before the write is reported (the reconcile correlates a
                // waiting expectation), so an SSG save never reads as an external
                // modification. The library owns this; a client never participates.
                saveObserver(std::filesystem::path{path});
            }
            touchRecent(std::move(path));
            WorkspaceResult result;
            result.document = entry.id;
            return result;
        } catch (const std::exception& exception) {
            return failure(WorkspaceError::IoFailed, exception.what());
        }
    }
};

Workspace::Workspace(std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

Workspace Workspace::create(const std::filesystem::path& root,
                            RecoveryManager& recovery,
                            std::optional<std::filesystem::path> archiveRoot) {
    std::error_code code;
    const auto canonical = canonicalPath(root, code);
    const auto status = code ? std::optional<FileStat>{} : statFile(canonical);
    if (code || !status || status->kind != FileKind::Directory) {
        throw std::invalid_argument("workspace root must be an existing directory");
    }
    // Defaults beside the other editor-private state, so a caller that does not
    // care still gets a real archive rather than none.
    auto archive = archiveRoot ? std::move(*archiveRoot)
                               : canonical / ".ssg" / "archive";
    return Workspace{
        std::make_unique<Impl>(canonical, recovery, std::move(archive))};
}

Workspace::~Workspace() = default;
Workspace::Workspace(Workspace&&) noexcept = default;
Workspace& Workspace::operator=(Workspace&&) noexcept = default;

const std::filesystem::path& Workspace::root() const noexcept {
    return impl_->root;
}

void Workspace::setSaveObserver(
    std::function<void(const std::filesystem::path&)> observer) {
    impl_->saveObserver = std::move(observer);
}

FileArchivePruneReport Workspace::pruneArchive(
    std::chrono::system_clock::time_point now) {
    return impl_->archive.prune(now, kFileArchiveRetention);
}

std::vector<FileDocumentId> Workspace::documents() const {
    std::vector<FileDocumentId> ids;
    ids.reserve(impl_->entries.size());
    for (const auto& entry : impl_->entries) {
        ids.push_back(entry.id);
    }
    return ids;
}

std::optional<WorkspaceDocumentState> Workspace::state(
    FileDocumentId documentId) const {
    const auto* entry = impl_->find(documentId);
    if (!entry) {
        return std::nullopt;
    }
    const auto text = entry->document.snapshot().text;
    const bool untitled = entry->key.kind() == JournalDocumentKeyKind::Untitled;
    // An untitled buffer used to be dirty unconditionally.  Technically true --
    // it has never been written anywhere -- but it made the state useless: every
    // session opens on an empty scratch buffer, so every session began showing
    // unsaved changes nobody had made, and the badge stopped meaning anything.
    // An untitled buffer is unsaved exactly when it holds something to lose.
    const bool dirty =
        untitled ? !text.empty()
                 : (!entry->persisted || text != entry->decoded.utf8 ||
                    entry->decoded.status != entry->persistedStatus);
    return WorkspaceDocumentState{
        entry->id,
        entry->key,
        entry->displayLabel,
        entry->contentKind,
        entry->decoded.status,
        dirty,
    };
}

std::optional<DraftBaseline> Workspace::baselineFor(
    FileDocumentId documentId) const {
    const auto* entry = impl_->find(documentId);
    return entry ? entry->baseline.present : std::nullopt;
}

bool Workspace::matchesExternalBaseline(
    FileDocumentId documentId,
    const std::optional<std::string>& observedContent) const {
    const auto* entry = impl_->find(documentId);
    if (!entry) return false;
    const auto& baseline = entry->baseline;
    if (!observedContent) {
        // A file observed absent matches only a Missing baseline (a dismissed
        // removal). An Unknown observation is never passed here.
        return baseline.missing;
    }
    if (!baseline.present) return false;
    return baseline.present->size == observedContent->size() &&
           baseline.present->contentHash == fastContentHash(*observedContent);
}

bool Workspace::commitExternalDismissal(
    FileDocumentId documentId, bool removed,
    const std::optional<std::string>& dismissedContent) {
    auto* entry = impl_->find(documentId);
    if (!entry) return false;
    if (removed) {
        entry->baseline = Impl::ExternalBaseline::removed();
    } else {
        DraftBaseline advanced;
        const std::string_view bytes =
            dismissedContent ? std::string_view{*dismissedContent}
                             : std::string_view{};
        advanced.size = static_cast<std::uint64_t>(bytes.size());
        advanced.contentHash = fastContentHash(bytes);
        // mtimeNanos is left unset: the observed mtime that pairs with these
        // dismissed bytes is not available here, and stat-ing the path now would
        // record newer disk metadata against the older dismissed content. The
        // raise/skip and draft-classify comparisons use size+contentHash, so mtime
        // is never load-bearing; a misleading mixed value must not be stored.
        entry->baseline = Impl::ExternalBaseline{advanced};
    }
    return true;
}

std::optional<std::string> Workspace::rawDiskContent(
    FileDocumentId documentId) const {
    const auto* entry = impl_->find(documentId);
    if (!entry) return std::nullopt;
    return std::string{entry->rawBytes.begin(), entry->rawBytes.end()};
}

bool Workspace::restoreDraft(FileDocumentId documentId,
                             std::string_view draftContent) {
    auto* entry = impl_->find(documentId);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::Saved) {
        return false;
    }
    if (entry->contentKind != FileContentKind::Text) return false;
    // Keep decoded.utf8 / persistedStatus / baseline (the disk state) so
    // state() derives dirty from draft-vs-disk; only the live buffer changes.
    std::string text{draftContent};
    entry->document = Document{text, entry->document.mode()};
    // decoded.utf8 remains the disk baseline used by the dirty check.
    // lineTerminators follows the live buffer because applyTerminatorEdits indexes
    // it against the pre-edit buffer text.
    const auto terminator = defaultTerminator(entry->decoded.status);
    entry->decoded.lineTerminators.clear();
    for (const char value : text) {
        if (value == '\n') entry->decoded.lineTerminators.push_back(terminator);
    }
    if (!text.empty() && text.back() != '\n') {
        entry->decoded.lineTerminators.push_back(LineTerminator::None);
    }
    entry->decoded.status.finalNewline = !text.empty() && text.back() == '\n';
    return true;
}

const Document& Workspace::document(FileDocumentId id) const {
    const auto* entry = impl_->find(id);
    if (!entry) {
        throw std::out_of_range("workspace document does not exist");
    }
    return entry->document;
}

const Document* Workspace::tryDocument(FileDocumentId id) const noexcept {
    const auto* entry = impl_->find(id);
    return entry ? &entry->document : nullptr;
}

TransactionResult Workspace::apply(
    FileDocumentId id, const EditTransaction& transaction) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return {DocumentError::InvalidRange, std::uint64_t{0},
                "workspace document does not exist"};
    }
    const auto currentText = entry->document.snapshot().text;
    const auto result = entry->document.apply(transaction);
    if (result.accepted()) {
        applyTerminatorEdits(entry->decoded, currentText, transaction);
    }
    return result;
}

std::vector<std::string> Workspace::recentFiles() const {
    return impl_->recent;
}

WorkspaceResult Workspace::openDirectory(
    const std::filesystem::path& path) {
    std::error_code code;
    const auto canonical = canonicalPath(path, code);
    const auto status = code ? std::optional<FileStat>{} : statFile(canonical);
    if (code || !status || status->kind != FileKind::Directory) {
        return failure(WorkspaceError::InvalidWorkspace,
                       "workspace path must be an existing directory");
    }
    impl_->root = canonical;
    impl_->entries.clear();
    impl_->recent.clear();
    impl_->compensations.clear();
    return {};
}

WorkspaceResult Workspace::newDocument(std::string_view suggestedLabel) {
    return impl_->addBytes(
        {}, JournalDocumentKey::untitled(UntitledDocumentId::generate()),
        suggestedLabel.empty() ? std::string{kNewBufferLabel} : sanitizeLabel(suggestedLabel),
        DocumentMode::Edit);
}

WorkspaceResult Workspace::newFile(std::string_view rawPath) {
    WorkspaceResult pathError;
    const auto absolute = impl_->resolve(rawPath, false, pathError);
    if (!absolute) {
        return pathError;
    }
    const auto path = normalizedRelative(rawPath);
    if (impl_->findPath(path)) {
        return failure(WorkspaceError::AlreadyOpen,
                       "destination is already open");
    }
    if (const auto status = statFile(*absolute); status) {
        return failure(WorkspaceError::AlreadyOpen, "path already exists");
    }
    auto result = impl_->addBytes({}, JournalDocumentKey::saved(path),
                                  absolute->filename().string());
    if (result.accepted() && result.document) {
        if (auto* entry = impl_->find(*result.document)) {
            entry->persisted = false;
        }
    }
    return result;
}

WorkspaceResult Workspace::openVirtualDocument(std::string_view suggestedLabel,
                                               std::string_view initialText,
                                               DocumentMode mode) {
    return impl_->addBytes(
        std::vector<std::uint8_t>{initialText.begin(), initialText.end()},
        JournalDocumentKey::untitled(UntitledDocumentId::generate()),
        suggestedLabel.empty() ? std::string{kNewBufferLabel} : sanitizeLabel(suggestedLabel),
        mode);
}

WorkspaceResult Workspace::openFile(std::string_view rawPath) {
    WorkspaceResult pathError;
    const auto absolute = impl_->resolve(rawPath, true, pathError);
    if (!absolute) {
        return pathError;
    }
    const auto path = normalizedRelative(rawPath);
    if (auto* existing = impl_->findPath(path)) {
        WorkspaceResult result;
        result.document = existing->id;
        impl_->touchRecent(path);
        return result;
    }
    const auto status = statFile(*absolute);
    if (!status || status->kind != FileKind::Regular) {
        return failure(WorkspaceError::NotFound,
                       "path is not a regular file");
    }
    try {
        auto bytes = readFileBytes(*absolute);
        auto baseline = captureDiskBaseline(*absolute, bytes);
        auto result = impl_->addBytes(
            std::move(bytes), JournalDocumentKey::saved(path),
            absolute->filename().string());
        if (result.accepted() && result.document) {
            if (auto* entry = impl_->find(*result.document)) {
                entry->baseline = baseline;
            }
        }
        impl_->touchRecent(path);
        return result;
    } catch (const std::exception& exception) {
        return failure(WorkspaceError::IoFailed, exception.what());
    }
}

WorkspaceResult Workspace::openRecent(std::size_t index) {
    if (index >= impl_->recent.size()) {
        return failure(WorkspaceError::NotFound,
                       "recent-file index does not exist");
    }
    const auto path = impl_->recent[index];
    const auto result = openFile(path);
    if (result.error == WorkspaceError::NotFound ||
        result.error == WorkspaceError::IoFailed) {
        impl_->recent.erase(
            std::remove(impl_->recent.begin(), impl_->recent.end(), path),
            impl_->recent.end());
    }
    return result;
}

WorkspaceResult Workspace::openDroppedContent(
    std::span<const std::uint8_t> bytes,
    std::string_view suggestedLabel) {
    return impl_->addBytes(
        {bytes.begin(), bytes.end()},
        JournalDocumentKey::untitled(UntitledDocumentId::generate()),
        sanitizeLabel(suggestedLabel), DocumentMode::Edit);
}

WorkspaceResult Workspace::save(FileDocumentId id) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::NotFound,
                       "workspace document does not exist");
    }
    if (entry->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure(WorkspaceError::InvalidPath,
                       "untitled document requires save_as");
    }
    WorkspaceResult pathError;
    const auto absolute =
        impl_->resolve(entry->key.savedPath(), false, pathError);
    if (!absolute) {
        return pathError;
    }
    // Saving a document over its own path is the one permitted overwrite. A
    // named-but-never-written document has no own file yet, so its first write
    // must not clobber one that appeared in the meantime.
    return impl_->saveTo(*entry, entry->key.savedPath(), *absolute,
                         entry->persisted);
}

WorkspaceResult Workspace::saveAll() {
    WorkspaceResult aggregate;
    const auto ids = documents();
    for (const auto id : ids) {
        const auto current = state(id);
        if (!current || !current->dirty ||
            current->key.kind() != JournalDocumentKeyKind::Saved) {
            continue;
        }
        const auto saved = save(id);
        if (!saved.accepted()) {
            aggregate.failures.push_back(
                {id, saved.error, std::move(saved.message)});
        }
    }
    if (!aggregate.failures.empty()) {
        aggregate.error = WorkspaceError::PartialFailure;
        aggregate.message = "one or more documents could not be saved";
    }
    return aggregate;
}

WorkspaceResult Workspace::saveAs(FileDocumentId id,
                                   std::string_view rawPath) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::NotFound,
                       "workspace document does not exist");
    }
    WorkspaceResult pathError;
    const auto absolute = impl_->resolve(rawPath, false, pathError);
    if (!absolute) {
        return pathError;
    }
    const auto path = normalizedRelative(rawPath);
    if (const auto* duplicate = impl_->findPath(path);
        duplicate && duplicate->id != id) {
        return failure(WorkspaceError::AlreadyOpen,
                       "destination is already open");
    }
    // Save-as to the document's OWN current path is just a save, so it keeps
    // the self-overwrite permission. Any other name must not clobber.
    const bool ownPath = entry->key.kind() == JournalDocumentKeyKind::Saved &&
                         entry->persisted && entry->key.savedPath() == path;
    return impl_->saveTo(*entry, path, *absolute, ownPath);
}

WorkspaceResult Workspace::reload(FileDocumentId id) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure(WorkspaceError::NotFound,
                       "saved workspace document does not exist");
    }
    WorkspaceResult pathError;
    const auto absolute =
        impl_->resolve(entry->key.savedPath(), true, pathError);
    if (!absolute) {
        return pathError;
    }
    try {
        const auto bytes = readFileBytes(*absolute);
        if (containsBinaryNul(asUnsignedBytes(bytes))) {
            return failure(WorkspaceError::DecodeFailed,
                           "binary file cannot replace an editable document");
        }
        auto decoded = decodeText(asUnsignedBytes(bytes));
        if (!decoded.accepted()) {
            return failure(WorkspaceError::DecodeFailed,
                           decoded.error->message);
        }
        entry->document = Document{decoded.text->utf8};
        entry->decoded = std::move(*decoded.text);
        entry->rawBytes = bytes;
        entry->persistedStatus = entry->decoded.status;
        // Reload re-reads disk, so the baseline now branches from the reloaded
        // disk content.
        entry->baseline = captureDiskBaseline(*absolute, bytes);
        WorkspaceResult result;
        result.document = id;
        return result;
    } catch (const std::exception& exception) {
        return failure(WorkspaceError::IoFailed, exception.what());
    }
}

WorkspaceResult Workspace::reloadWithContent(FileDocumentId id,
                                             std::string content) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure(WorkspaceError::NotFound,
                       "saved workspace document does not exist");
    }
    if (entry->contentKind != FileContentKind::Text) {
        return failure(WorkspaceError::ReadOnly,
                       "read-only content cannot be reloaded");
    }
    WorkspaceResult pathError;
    const auto absolute =
        impl_->resolve(entry->key.savedPath(), false, pathError);
    if (!absolute) {
        return pathError;
    }
    // `content` is the RAW disk bytes, not UTF-8 text: decode them through the
    // document's existing encoding (the same decode the normal open/reload path
    // uses), so a non-UTF-8 document is neither corrupted nor rejected. The
    // binary guard is applied to the DECODED text, never the raw bytes: a
    // wide-encoding document (UTF-16) legitimately carries NUL bytes on disk, so
    // rejecting raw NUL would wrongly refuse valid content in its own encoding.
    const std::vector<std::uint8_t> bytes{content.begin(), content.end()};
    auto decoded =
        decodeText(asUnsignedBytes(bytes), entry->decoded.status.encoding);
    if (!decoded.accepted()) {
        return failure(WorkspaceError::DecodeFailed, decoded.error->message);
    }
    if (containsNul(decoded.text->utf8)) {
        return failure(WorkspaceError::DecodeFailed,
                       "binary file cannot replace an editable document");
    }
    entry->document = Document{decoded.text->utf8, entry->document.mode()};
    entry->decoded = std::move(*decoded.text);
    entry->rawBytes = bytes;
    entry->persistedStatus = entry->decoded.status;
    entry->baseline = captureDiskBaseline(*absolute, entry->rawBytes);
    WorkspaceResult result;
    result.document = id;
    return result;
}

WorkspaceResult Workspace::adoptExternalRename(FileDocumentId id,
                                               std::string_view rawNewPath,
                                               std::string content,
                                               bool replaceBuffer) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure(WorkspaceError::NotFound,
                       "saved workspace document does not exist");
    }
    WorkspaceResult destinationError;
    const auto destination =
        impl_->resolve(rawNewPath, false, destinationError);
    if (!destination) {
        return destinationError;
    }
    const auto path = normalizedRelative(rawNewPath);
    if (const auto* duplicate = impl_->findPath(path);
        duplicate && duplicate->id != id) {
        return failure(WorkspaceError::AlreadyOpen,
                       "destination is already open");
    }
    // `content` is the RAW new-path disk bytes: decode them through the document's
    // existing encoding so a non-UTF-8 file adopts as text, not corrupted bytes.
    // The binary guard is applied to the DECODED text, never the raw bytes, so a
    // wide-encoding document (UTF-16) whose disk bytes legitimately contain NUL is
    // not wrongly rejected as binary.
    const std::vector<std::uint8_t> baselineBytes{content.begin(), content.end()};
    auto decoded = decodeText(asUnsignedBytes(baselineBytes),
                                      entry->decoded.status.encoding);
    if (!decoded.accepted()) {
        return failure(WorkspaceError::DecodeFailed, decoded.error->message);
    }
    if (containsNul(decoded.text->utf8)) {
        return failure(WorkspaceError::DecodeFailed,
                       "binary file cannot replace an editable document");
    }
    const std::string diskText = decoded.text->utf8;
    const std::string bufferContent =
        replaceBuffer ? diskText : entry->document.snapshot().text;
    entry->key = JournalDocumentKey::saved(path);
    entry->displayLabel = destination->filename().string();
    if (replaceBuffer) {
        entry->document = Document{diskText, entry->document.mode()};
        entry->decoded = *decoded.text;
    } else {
        entry->decoded.utf8 = diskText;
    }
    // The baseline is always the new-path disk content, so dirtiness is derived
    // from buffer-vs-disk regardless of whether the buffer followed. Persisted
    // status reflects the decoded disk bytes.
    entry->rawBytes = baselineBytes;
    entry->persistedStatus = decoded.text->status;
    entry->baseline = captureDiskBaseline(*destination, entry->rawBytes);
    impl_->touchRecent(path);
    WorkspaceResult result;
    result.document = id;
    return result;
}

WorkspaceResult Workspace::reopenWithEncoding(FileDocumentId id,
                                               TextEncoding encoding) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::NotFound,
                       "workspace document does not exist");
    }
    if (state(id)->dirty) {
        return failure(WorkspaceError::ReadOnly,
                       "dirty document cannot be reopened with encoding");
    }
    if (containsBinaryNul(asUnsignedBytes(entry->rawBytes))) {
        return failure(WorkspaceError::DecodeFailed,
                       "binary file cannot be reopened with encoding");
    }
    auto decoded = decodeText(asUnsignedBytes(entry->rawBytes), encoding);
    if (!decoded.accepted()) {
        return failure(WorkspaceError::DecodeFailed, decoded.error->message);
    }
    entry->contentKind = FileContentKind::Text;
    entry->decoded = std::move(*decoded.text);
    entry->document = Document{entry->decoded.utf8};
    entry->persistedStatus = entry->decoded.status;
    WorkspaceResult result;
    result.document = id;
    return result;
}

WorkspaceResult Workspace::setEncoding(FileDocumentId id,
                                       TextEncoding encoding) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::NotFound,
                       "workspace document does not exist");
    }
    if (entry->contentKind != FileContentKind::Text) {
        return failure(WorkspaceError::ReadOnly,
                       "read-only content cannot change encoding");
    }
    if (static_cast<std::uint8_t>(encoding) >
        static_cast<std::uint8_t>(TextEncoding::Iso88591)) {
        return failure(WorkspaceError::DecodeFailed,
                       "text encoding is not recognized");
    }
    entry->decoded.status.encoding = encoding;
    entry->decoded.status.hadBom =
        encoding == TextEncoding::Utf8Bom ||
        encoding == TextEncoding::Utf16le ||
        encoding == TextEncoding::Utf16be;
    WorkspaceResult result;
    result.document = id;
    return result;
}

WorkspaceResult Workspace::setLineEnding(FileDocumentId id,
                                          LineEnding lineEnding) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::NotFound,
                       "workspace document does not exist");
    }
    if (entry->contentKind != FileContentKind::Text) {
        return failure(WorkspaceError::ReadOnly,
                       "read-only content cannot change line endings");
    }
    if (lineEnding == LineEnding::Mixed) {
        return failure(WorkspaceError::DecodeFailed,
                       "line ending must be lf, crlf, or cr");
    }
    const auto terminator = lineEndingTerminator(lineEnding);
    for (auto& stored : entry->decoded.lineTerminators) {
        if (stored != LineTerminator::None) stored = terminator;
    }
    entry->decoded.status.lineEnding = lineEnding;
    WorkspaceResult result;
    result.document = id;
    return result;
}

WorkspaceResult Workspace::setFinalNewline(FileDocumentId id,
                                            bool finalNewline) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::NotFound,
                       "workspace document does not exist");
    }
    if (entry->contentKind != FileContentKind::Text) {
        return failure(WorkspaceError::ReadOnly,
                       "read-only content cannot change final newline");
    }
    auto snapshot = entry->document.snapshot();
    const bool hasFinalNewline =
        !snapshot.text.empty() && snapshot.text.back() == '\n';
    if (hasFinalNewline == finalNewline) {
        entry->decoded.status.finalNewline = finalNewline;
        WorkspaceResult result;
        result.document = id;
        return result;
    }
    EditTransaction transaction{snapshot.revision, {}};
    if (finalNewline) {
        transaction.edits.push_back(
            {ByteOffset{snapshot.text.size()}, 0, "\n"});
    } else {
        transaction.edits.push_back(
            {ByteOffset{snapshot.text.size() - 1}, 1, ""});
    }
    auto applied = apply(id, transaction);
    if (!applied.accepted()) {
        return failure(WorkspaceError::IoFailed, applied.message);
    }
    entry->decoded.status.finalNewline = finalNewline;
    WorkspaceResult result;
    result.document = id;
    return result;
}

WorkspaceResult Workspace::renameFile(FileDocumentId id,
                                      std::string_view rawPath) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure(WorkspaceError::NotFound,
                       "saved workspace document does not exist");
    }
    WorkspaceResult sourceError;
    const auto source =
        impl_->resolve(entry->key.savedPath(), true, sourceError);
    if (!source) {
        return sourceError;
    }
    WorkspaceResult destinationError;
    const auto destination =
        impl_->resolve(rawPath, false, destinationError);
    if (!destination) {
        return destinationError;
    }
    const auto path = normalizedRelative(rawPath);
    if (const auto* duplicate = impl_->findPath(path);
        duplicate && duplicate->id != id) {
        return failure(WorkspaceError::AlreadyOpen,
                       "destination is already open");
    }
    // renamePathNoClobber, not renamePath: the latter deliberately replaces the
    // destination. Letting the recovery action own the exclusion (rather than
    // claiming the name here first) keeps its snapshot honest -- the
    // destination is recorded as missing, so a rollback REMOVES it instead of
    // restoring an empty placeholder that was never really there.
    const auto action =
        impl_->recovery.renamePathNoClobber(*source, *destination);
    if (!action.accepted()) {
        return failure(WorkspaceError::RecoveryFailed,
                       action.error->message);
    }
    if (action.compensation) {
        impl_->rememberCompensation(
            *action.compensation,
            Impl::CompensationState{id, entry->key, entry->displayLabel});
    }
    entry->key = JournalDocumentKey::saved(path);
    entry->displayLabel = destination->filename().string();
    impl_->touchRecent(path);
    WorkspaceResult result;
    result.document = id;
    result.compensation = action.compensation;
    return result;
}

WorkspaceResult Workspace::deleteFile(FileDocumentId id) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure(WorkspaceError::NotFound,
                       "saved workspace document does not exist");
    }
    WorkspaceResult pathError;
    const auto path =
        impl_->resolve(entry->key.savedPath(), true, pathError);
    if (!path) {
        return pathError;
    }
    // The archive copy is durable BEFORE the delete runs. If it fails, the
    // delete fails and the file is untouched: a delete must never reduce the
    // number of copies below one, and this command deliberately asks for no
    // confirmation, so the archive is what makes that safe.
    const auto archived = impl_->archive.archive(impl_->root, *path);
    if (!archived.ok()) {
        return failure(WorkspaceError::IoFailed,
                       "could not archive the file before deleting it: " +
                           archived.message);
    }
    const auto action = impl_->recovery.deletePath(*path);
    if (!action.accepted()) {
        return failure(WorkspaceError::RecoveryFailed,
                       action.error->message);
    }
    WorkspaceResult result;
    result.compensation = action.compensation;
    result.document = id;
    auto removed = std::find_if(
        impl_->entries.begin(), impl_->entries.end(),
        [id](const Impl::Entry& candidate) { return candidate.id == id; });
    if (action.compensation) {
        impl_->rememberCompensation(
            *action.compensation,
            Impl::CompensationState{id, removed->key, removed->displayLabel,
                                    archived.archivedPath});
    }
    impl_->entries.erase(removed);
    return result;
}

WorkspaceResult Workspace::removeDocument(FileDocumentId id) {
    auto removed = std::find_if(
        impl_->entries.begin(), impl_->entries.end(),
        [id](const Impl::Entry& candidate) { return candidate.id == id; });
    if (removed == impl_->entries.end()) {
        return failure(WorkspaceError::NotFound,
                       "workspace document does not exist");
    }
    impl_->entries.erase(removed);
    WorkspaceResult result;
    result.document = id;
    return result;
}

WorkspaceResult Workspace::newDirectory(std::string_view rawPath) {
    WorkspaceResult pathError;
    const auto path = impl_->resolve(rawPath, false, pathError);
    if (!path) {
        return pathError;
    }
    const auto parent = statFile(path->parent_path());
    if (!parent || parent->kind != FileKind::Directory) {
        return failure(WorkspaceError::IoFailed,
                       "parent directory does not exist");
    }
    const auto created = createDirectoriesDurably(*path);
    if (!created.ok()) {
        return failure(WorkspaceError::IoFailed, created.message);
    }
    return {};
}

WorkspaceResult Workspace::restore(
    const RecoveryRecordId& compensation) {
    auto found =
        impl_->compensations.find(std::string{compensation.value()});
    std::vector<std::uint8_t> archivedBytes;
    if (found != impl_->compensations.end() &&
        found->second.archivedPath) {
        try {
            archivedBytes = readFileBytes(*found->second.archivedPath);
        } catch (const std::exception& exception) {
            return failure(WorkspaceError::IoFailed, exception.what());
        }
    }
    const auto restored = impl_->recovery.restoreFilesystem(compensation);
    if (!restored.accepted()) {
        if (found != impl_->compensations.end() && restored.error->code ==
                                                        RecoveryErrorCode::
                                                            RecordNotFound) {
            impl_->compensations.erase(found);
        }
        return failure(WorkspaceError::RecoveryFailed,
                       restored.error->message);
    }
    if (found != impl_->compensations.end()) {
        WorkspaceResult result;
        if (found->second.archivedPath) {
            const auto prior = found->second;
            auto added = impl_->addBytes(
                std::move(archivedBytes), prior.priorKey, prior.priorLabel);
            auto* entry = impl_->find(*added.document);
            entry->id = prior.id;
            WorkspaceResult pathError;
            const auto path =
                impl_->resolve(prior.priorKey.savedPath(), true, pathError);
            if (path) {
                entry->baseline =
                    captureDiskBaseline(*path, entry->rawBytes);
            }
            result.document = prior.id;
        } else if (auto* entry = impl_->find(found->second.id)) {
            entry->key = found->second.priorKey;
            entry->displayLabel = found->second.priorLabel;
            result.document = entry->id;
        }
        impl_->compensations.erase(found);
        return result;
    }
    return {};
}

}  // namespace ssg
