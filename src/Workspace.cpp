#include <ssg/Workspace.h>

#include <ssg/open_metrics.h>
#include <ssg/SharedBytes.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
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

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("failed to open file for reading: " +
                                 path.string());
    }
    OpenPhaseTimer timer{OpenPhase::Read};
    std::vector<std::uint8_t> bytes;
    std::error_code sizeError;
    const auto hint = std::filesystem::file_size(path, sizeError);
    if (!sizeError) {
        bytes.reserve(static_cast<std::size_t>(hint));
    }
    // Read to EOF in chunks: size the buffer from the stat hint but never trust
    // it (the file may grow/shrink under us), and copy each chunk into the
    // reserved vector without zero-initializing capacity first.
    std::array<char, 1U << 16> chunk;
    while (stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
           stream.gcount() > 0) {
        const auto got = static_cast<std::size_t>(stream.gcount());
        bytes.insert(bytes.end(),
                     reinterpret_cast<const std::uint8_t*>(chunk.data()),
                     reinterpret_cast<const std::uint8_t*>(chunk.data()) + got);
    }
    return bytes;
}

std::span<const std::byte> asBytes(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::span<const std::uint8_t> asUnsignedBytes(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return {bytes.data(), bytes.size()};
}

std::vector<std::byte> toBytes(
    const std::vector<std::uint8_t>& bytes) {
    std::vector<std::byte> result(bytes.size());
    std::transform(bytes.begin(), bytes.end(), result.begin(),
                   [](std::uint8_t byte) {
                       return static_cast<std::byte>(byte);
                   });
    return result;
}

bool containsNul(std::span<const std::uint8_t> bytes) {
    return std::find(bytes.begin(), bytes.end(), std::uint8_t{0}) !=
           bytes.end();
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

void applyTerminatorEdits(DecodedText& decoded,
                            const EditTransaction& transaction) {
    struct AnnotatedByte {
        char value;
        LineTerminator terminator = LineTerminator::None;
    };
    std::vector<AnnotatedByte> bytes;
    bytes.reserve(decoded.utf8.size());
    std::size_t terminatorIndex = 0;
    for (const char value : decoded.utf8) {
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
    decoded.utf8.clear();
    decoded.lineTerminators.clear();
    decoded.utf8.reserve(bytes.size());
    for (const auto byte : bytes) {
        decoded.utf8.push_back(byte.value);
        if (byte.value == '\n') {
            decoded.lineTerminators.push_back(byte.terminator);
        }
    }
    if (!decoded.utf8.empty() && decoded.utf8.back() != '\n') {
        decoded.lineTerminators.push_back(LineTerminator::None);
    }
    decoded.status.finalNewline =
        !decoded.utf8.empty() && decoded.utf8.back() == '\n';
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

class Workspace::Impl {
public:
    struct Entry {
        FileDocumentId id;
        JournalDocumentKey key;
        std::string displayLabel;
        FileContentKind contentKind;
        DecodedText decoded;
        std::vector<std::uint8_t> rawBytes;
        Document document;
        SharedBytes persistedText;
        TextEncodingStatus persistedStatus;
    };

    struct CompensationState {
        FileDocumentId id;
        JournalDocumentKey priorKey;
        std::string priorLabel;
        bool wasDeleted = false;
        bool restoresDocument = false;
        bool markDirtyOnRestore = false;
        std::optional<Entry> deletedEntry;
        std::optional<DecodedText> priorDecoded;
        SharedBytes priorPersistedText;
        TextEncodingStatus priorPersistedStatus;
    };

    struct ReplacedWorkspace {
        WorkspaceReplacementId id;
        std::filesystem::path root;
        std::vector<Entry> entries;
        std::vector<std::string> recent;
    };

    Impl(std::filesystem::path canonicalRoot, RecoveryActions& actions)
        : root(std::move(canonicalRoot)), recovery(actions) {}

    std::filesystem::path root;
    RecoveryActions& recovery;
    std::uint64_t nextDocument = 1;
    std::uint64_t nextWorkspaceReplacement = 1;
    std::vector<Entry> entries;
    std::vector<std::string> recent;
    std::unordered_map<std::string, CompensationState> compensations;
    std::optional<ReplacedWorkspace> replacedWorkspace;

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
                                   ? std::filesystem::weakly_canonical(
                                         root / relative, code)
                                   : std::filesystem::weakly_canonical(
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
        if (mustExist && !std::filesystem::exists(candidate)) {
            error = failure(WorkspaceError::NotFound, "path does not exist");
            return std::nullopt;
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

    WorkspaceResult addBytes(std::vector<std::uint8_t> bytes,
                              JournalDocumentKey key,
                              std::string label,
                              bool dirty) {
        const auto id = FileDocumentId{nextDocument++};
        bool hasNul;
        {
            OpenPhaseTimer timer{OpenPhase::NulScan};
            hasNul = containsNul(asUnsignedBytes(bytes));
        }
        if (hasNul) {
            entries.push_back({id, std::move(key), std::move(label),
                               FileContentKind::Binary, {}, std::move(bytes),
                               Document{"", DocumentMode::ReadOnly}, {}, {}});
        } else {
            auto decoded = TextCodec{}.decode(asUnsignedBytes(bytes));
            if (!decoded.accepted()) {
                entries.push_back(
                    {id, std::move(key), std::move(label),
                     FileContentKind::DecodeFailure, {}, std::move(bytes),
                     Document{"", DocumentMode::ReadOnly}, {}, {}});
            } else {
                auto proof = decoded.validated();
                const SharedBytes persisted =
                    dirty ? SharedBytes{} : proof.bytes();
                const auto persistedStatus = decoded.text->status;
                Document document = [&] {
                    OpenPhaseTimer timer{OpenPhase::DocumentBuild};
                    return Document{std::move(proof)};
                }();
                entries.push_back(
                    {id, std::move(key), std::move(label),
                     FileContentKind::Text, std::move(*decoded.text),
                     std::move(bytes), std::move(document), persisted,
                     persistedStatus});
            }
        }
        WorkspaceResult result;
        result.document = id;
        return result;
    }

    WorkspaceResult saveTo(Entry& entry, std::string path,
                            const std::filesystem::path& absolute) {
        if (entry.contentKind != FileContentKind::Text) {
            return failure(WorkspaceError::ReadOnly,
                           "read-only content cannot be saved");
        }
        const auto current = entry.document.snapshot().text;
        auto decoded = textForSave(entry.decoded, current);
        const auto encoded = TextCodec{}.encode(decoded);
        if (!encoded.accepted()) {
            return failure(WorkspaceError::DecodeFailed,
                           encoded.error->message);
        }
        try {
            WorkspaceResult result;
            if (std::filesystem::exists(absolute)) {
                const auto priorPersisted = entry.persistedText;
                const auto replacement = toBytes(encoded.bytes);
                const auto action =
                    recovery.overwriteFile(absolute, replacement);
                if (!action.accepted()) {
                    return failure(WorkspaceError::RecoveryFailed,
                                   action.error->message);
                }
                result.compensation = action.compensation;
                if (action.compensation) {
                    CompensationState state{entry.id, entry.key,
                                            entry.displayLabel};
                    state.markDirtyOnRestore = true;
                    state.priorPersistedText = priorPersisted;
                    state.priorPersistedStatus = entry.persistedStatus;
                    compensations.emplace(
                        std::string{action.compensation->value()},
                        std::move(state));
                }
            } else {
                replaceFileAtomically(absolute, asBytes(encoded.bytes));
            }
            entry.key = JournalDocumentKey::saved(path);
            entry.displayLabel =
                std::filesystem::path{path}.filename().string();
            entry.decoded = std::move(decoded);
            entry.rawBytes = encoded.bytes;
            entry.persistedText = SharedBytes::owning(current);
            entry.persistedStatus = entry.decoded.status;
            touchRecent(std::move(path));
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
                            RecoveryActions& recovery) {
    std::error_code code;
    const auto canonical = std::filesystem::canonical(root, code);
    if (code || !std::filesystem::is_directory(canonical)) {
        throw std::invalid_argument("workspace root must be an existing directory");
    }
    return Workspace{std::make_unique<Impl>(canonical, recovery)};
}

Workspace::~Workspace() = default;
Workspace::Workspace(Workspace&&) noexcept = default;
Workspace& Workspace::operator=(Workspace&&) noexcept = default;

const std::filesystem::path& Workspace::root() const noexcept {
    return impl_->root;
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
    OpenPhaseTimer timer{OpenPhase::StateDirtyCheck};
    const auto text = entry->document.snapshot().text;
    const bool dirty =
        entry->key.kind() == JournalDocumentKeyKind::Untitled ||
        text != entry->persistedText.view() ||
        entry->decoded.status != entry->persistedStatus;
    return WorkspaceDocumentState{
        entry->id,
        entry->key,
        entry->displayLabel,
        entry->contentKind,
        entry->decoded.status,
        dirty,
    };
}

const Document& Workspace::document(FileDocumentId id) const {
    const auto* entry = impl_->find(id);
    if (!entry) {
        throw std::out_of_range("workspace document does not exist");
    }
    return entry->document;
}

TransactionResult Workspace::apply(
    FileDocumentId id, const EditTransaction& transaction) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return {DocumentError::InvalidRange, Revision{0},
                "workspace document does not exist"};
    }
    const auto result = entry->document.apply(transaction);
    if (result.accepted()) {
        applyTerminatorEdits(entry->decoded, transaction);
    }
    return result;
}

std::vector<std::string> Workspace::recentFiles() const {
    return impl_->recent;
}

WorkspaceResult Workspace::openDirectory(
    const std::filesystem::path& path) {
    std::error_code code;
    const auto canonical = std::filesystem::canonical(path, code);
    if (code || !std::filesystem::is_directory(canonical)) {
        return failure(WorkspaceError::InvalidWorkspace,
                       "workspace path must be an existing directory");
    }
    const auto replacement =
        WorkspaceReplacementId{impl_->nextWorkspaceReplacement++};
    impl_->replacedWorkspace = Impl::ReplacedWorkspace{
        replacement, impl_->root, std::move(impl_->entries),
        std::move(impl_->recent)};
    impl_->root = canonical;
    impl_->entries.clear();
    impl_->recent.clear();
    WorkspaceResult result;
    result.workspaceCompensation = replacement;
    return result;
}

WorkspaceResult Workspace::restoreWorkspace(
    WorkspaceReplacementId replacement) {
    if (!impl_->replacedWorkspace ||
        impl_->replacedWorkspace->id != replacement) {
        return failure(WorkspaceError::NotFound,
                       "workspace compensation does not exist");
    }
    impl_->root = std::move(impl_->replacedWorkspace->root);
    impl_->entries = std::move(impl_->replacedWorkspace->entries);
    impl_->recent = std::move(impl_->replacedWorkspace->recent);
    impl_->replacedWorkspace.reset();
    return {};
}

WorkspaceResult Workspace::newDocument(std::string_view suggestedLabel) {
    return impl_->addBytes(
        {}, JournalDocumentKey::untitled(UntitledDocumentId::generate()),
        suggestedLabel.empty() ? "Untitled" : sanitizeLabel(suggestedLabel),
        true);
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
    if (!std::filesystem::is_regular_file(*absolute)) {
        return failure(WorkspaceError::NotFound,
                       "path is not a regular file");
    }
    try {
        auto result = impl_->addBytes(
            readFile(*absolute), JournalDocumentKey::saved(path),
            absolute->filename().string(), false);
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
    const InvocationPrincipal& principal,
    std::span<const std::uint8_t> bytes,
    std::string_view suggestedLabel) {
    if (principal.origin() == InvocationOrigin::Lua ||
        !principal.hasCapability(CapabilityId{"local_file_drop"})) {
        return failure(WorkspaceError::CapabilityDenied,
                       "local file drop capability is required");
    }
    return impl_->addBytes(
        {bytes.begin(), bytes.end()},
        JournalDocumentKey::untitled(UntitledDocumentId::generate()),
        sanitizeLabel(suggestedLabel), true);
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
    return impl_->saveTo(*entry, entry->key.savedPath(), *absolute);
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
    return impl_->saveTo(*entry, path, *absolute);
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
        const auto bytes = readFile(*absolute);
        if (containsNul(asUnsignedBytes(bytes))) {
            return failure(WorkspaceError::DecodeFailed,
                           "binary file cannot replace an editable document");
        }
        auto decoded = TextCodec{}.decode(asUnsignedBytes(bytes));
        if (!decoded.accepted()) {
            return failure(WorkspaceError::DecodeFailed,
                           decoded.error->message);
        }
        std::optional<JournalDocument> documentBefore{
            JournalDocument{entry->key, entry->document.mode(),
                            state(id)->dirty, entry->document.snapshot().text}};
        const JournalDocument replacement{
            entry->key, entry->document.mode(), false, decoded.text->utf8};
        const auto action =
            impl_->recovery.reloadDocument(documentBefore, replacement);
        if (!action.accepted()) {
            return failure(WorkspaceError::RecoveryFailed,
                           action.error->message);
        }
        if (action.compensation) {
            Impl::CompensationState state{id, entry->key,
                                          entry->displayLabel};
            state.restoresDocument = true;
            state.priorDecoded = entry->decoded;
            state.priorPersistedText = entry->persistedText;
            state.priorPersistedStatus = entry->persistedStatus;
            impl_->compensations.emplace(
                std::string{action.compensation->value()}, std::move(state));
        }
        entry->document = Document{decoded.text->utf8};
        entry->decoded = std::move(*decoded.text);
        entry->rawBytes = bytes;
        entry->persistedText =
            SharedBytes::owning(entry->document.snapshot().text);
        entry->persistedStatus = entry->decoded.status;
        WorkspaceResult result;
        result.document = id;
        result.compensation = action.compensation;
        return result;
    } catch (const std::exception& exception) {
        return failure(WorkspaceError::IoFailed, exception.what());
    }
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
    if (containsNul(asUnsignedBytes(entry->rawBytes))) {
        return failure(WorkspaceError::DecodeFailed,
                       "binary file cannot be reopened with encoding");
    }
    auto decoded = TextCodec{}.decode(asUnsignedBytes(entry->rawBytes), encoding);
    if (!decoded.accepted()) {
        return failure(WorkspaceError::DecodeFailed, decoded.error->message);
    }
    entry->contentKind = FileContentKind::Text;
    entry->decoded = std::move(*decoded.text);
    entry->document = Document{entry->decoded.utf8};
    entry->persistedText = SharedBytes::owning(entry->decoded.utf8);
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
    const auto action = impl_->recovery.renamePath(*source, *destination);
    if (!action.accepted()) {
        return failure(WorkspaceError::RecoveryFailed,
                       action.error->message);
    }
    if (action.compensation) {
        impl_->compensations.emplace(
            std::string{action.compensation->value()},
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
        Impl::CompensationState state{id, removed->key, removed->displayLabel};
        state.wasDeleted = true;
        state.deletedEntry = std::move(*removed);
        impl_->compensations.emplace(
            std::string{action.compensation->value()}, std::move(state));
    }
    impl_->entries.erase(removed);
    return result;
}

WorkspaceResult Workspace::newDirectory(std::string_view rawPath) {
    WorkspaceResult pathError;
    const auto path = impl_->resolve(rawPath, false, pathError);
    if (!path) {
        return pathError;
    }
    std::error_code code;
    if (!std::filesystem::create_directory(*path, code) || code) {
        return failure(WorkspaceError::IoFailed,
                       code ? code.message() : "directory already exists");
    }
    return {};
}

WorkspaceResult Workspace::restore(
    const RecoveryRecordId& compensation) {
    const auto found =
        impl_->compensations.find(std::string{compensation.value()});
    if (found != impl_->compensations.end() &&
        found->second.restoresDocument) {
        auto* entry = impl_->find(found->second.id);
        if (!entry) {
            return failure(WorkspaceError::NotFound,
                           "document compensation target does not exist");
        }
        std::optional<JournalDocument> restoredDocument{
            JournalDocument{entry->key, entry->document.mode(), false,
                            entry->document.snapshot().text}};
        const auto restored = impl_->recovery.restoreDocument(
            compensation, restoredDocument);
        if (!restored.accepted()) {
            return failure(WorkspaceError::RecoveryFailed,
                           restored.error->message);
        }
        entry->document = Document{restoredDocument->utf8Content,
                                   restoredDocument->mode};
        if (found->second.priorDecoded) {
            entry->decoded = *found->second.priorDecoded;
        }
        entry->persistedText =
            restoredDocument->dirty
                ? found->second.priorPersistedText
                : SharedBytes::owning(restoredDocument->utf8Content);
        entry->persistedStatus = found->second.priorPersistedStatus;
        impl_->compensations.erase(found);
        return {};
    }
    const auto restored = impl_->recovery.restoreFilesystem(compensation);
    if (!restored.accepted()) {
        return failure(WorkspaceError::RecoveryFailed,
                       restored.error->message);
    }
    if (found != impl_->compensations.end()) {
        if (found->second.wasDeleted && found->second.deletedEntry) {
            impl_->entries.push_back(
                std::move(*found->second.deletedEntry));
        } else if (auto* entry = impl_->find(found->second.id)) {
            entry->key = found->second.priorKey;
            entry->displayLabel = found->second.priorLabel;
            if (found->second.markDirtyOnRestore) {
                entry->persistedText = found->second.priorPersistedText;
                entry->persistedStatus =
                    found->second.priorPersistedStatus;
            }
        }
        impl_->compensations.erase(found);
    }
    return {};
}

}  // namespace ssg
