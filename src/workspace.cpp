#include <ssg/workspace.h>

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

constexpr std::size_t maximum_recent_files = 32;
constexpr std::size_t maximum_drop_label_bytes = 255;

WorkspaceResult failure(WorkspaceError error, std::string message) {
    WorkspaceResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("failed to open file for reading: " +
                                 path.string());
    }
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

std::span<const std::byte> as_bytes(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::span<const std::uint8_t> as_unsigned_bytes(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return {bytes.data(), bytes.size()};
}

std::vector<std::byte> to_bytes(
    const std::vector<std::uint8_t>& bytes) {
    std::vector<std::byte> result(bytes.size());
    std::transform(bytes.begin(), bytes.end(), result.begin(),
                   [](std::uint8_t byte) {
                       return static_cast<std::byte>(byte);
                   });
    return result;
}

bool contains_nul(std::span<const std::uint8_t> bytes) {
    return std::find(bytes.begin(), bytes.end(), std::uint8_t{0}) !=
           bytes.end();
}

bool is_beneath(const std::filesystem::path& root,
                const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

PathSyntax native_syntax() noexcept {
#ifdef _WIN32
    return PathSyntax::windows;
#else
    return PathSyntax::linux;
#endif
}

std::string normalized_relative(std::string_view path) {
    return std::filesystem::path{path}.lexically_normal().generic_string();
}

LineTerminator default_terminator(const TextEncodingStatus& status) {
    switch (status.line_ending) {
        case LineEnding::crlf:
            return LineTerminator::crlf;
        case LineEnding::cr:
            return LineTerminator::cr;
        case LineEnding::lf:
        case LineEnding::mixed:
            return LineTerminator::lf;
    }
    return LineTerminator::lf;
}

DecodedText text_for_save(const DecodedText& original,
                          std::string current_text) {
    if (current_text == original.utf8) {
        return original;
    }
    DecodedText result;
    result.utf8 = std::move(current_text);
    result.status = original.status;
    result.line_terminators.clear();
    std::size_t line = 0;
    for (const char value : result.utf8) {
        if (value != '\n') {
            continue;
        }
        auto terminator = default_terminator(result.status);
        if (line < original.line_terminators.size() &&
            original.line_terminators[line] != LineTerminator::none) {
            terminator = original.line_terminators[line];
        }
        result.line_terminators.push_back(terminator);
        ++line;
    }
    if (!result.utf8.empty() && result.utf8.back() != '\n') {
        result.line_terminators.push_back(LineTerminator::none);
    }
    result.status.final_newline =
        !result.utf8.empty() && result.utf8.back() == '\n';
    return result;
}

void apply_terminator_edits(DecodedText& decoded,
                            const EditTransaction& transaction) {
    struct AnnotatedByte {
        char value;
        LineTerminator terminator = LineTerminator::none;
    };
    std::vector<AnnotatedByte> bytes;
    bytes.reserve(decoded.utf8.size());
    std::size_t terminator_index = 0;
    for (const char value : decoded.utf8) {
        auto terminator = LineTerminator::none;
        if (value == '\n') {
            terminator = decoded.line_terminators[terminator_index++];
        }
        bytes.push_back({value, terminator});
    }
    const auto inserted_terminator = default_terminator(decoded.status);
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
            begin + static_cast<std::size_t>((*edit)->erased_bytes);
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                    bytes.begin() + static_cast<std::ptrdiff_t>(end));
        std::vector<AnnotatedByte> inserted;
        inserted.reserve((*edit)->inserted_text.size());
        for (const char value : (*edit)->inserted_text) {
            inserted.push_back(
                {value, value == '\n' ? inserted_terminator
                                      : LineTerminator::none});
        }
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     inserted.begin(), inserted.end());
    }
    decoded.utf8.clear();
    decoded.line_terminators.clear();
    decoded.utf8.reserve(bytes.size());
    for (const auto byte : bytes) {
        decoded.utf8.push_back(byte.value);
        if (byte.value == '\n') {
            decoded.line_terminators.push_back(byte.terminator);
        }
    }
    if (!decoded.utf8.empty() && decoded.utf8.back() != '\n') {
        decoded.line_terminators.push_back(LineTerminator::none);
    }
    decoded.status.final_newline =
        !decoded.utf8.empty() && decoded.utf8.back() == '\n';
    std::optional<LineTerminator> uniform;
    bool mixed = false;
    for (const auto terminator : decoded.line_terminators) {
        if (terminator == LineTerminator::none) {
            continue;
        }
        if (!uniform) {
            uniform = terminator;
        } else if (*uniform != terminator) {
            mixed = true;
        }
    }
    if (mixed) {
        decoded.status.line_ending = LineEnding::mixed;
    } else if (uniform == LineTerminator::crlf) {
        decoded.status.line_ending = LineEnding::crlf;
    } else if (uniform == LineTerminator::cr) {
        decoded.status.line_ending = LineEnding::cr;
    } else {
        decoded.status.line_ending = LineEnding::lf;
    }
}

std::string sanitize_label(std::string_view suggested) {
    auto label = std::filesystem::path{suggested}.filename().string();
    label.erase(std::remove_if(label.begin(), label.end(), [](char value) {
                    const auto byte = static_cast<unsigned char>(value);
                    return byte < 0x20 || value == '/' || value == '\\';
                }),
                label.end());
    if (label.size() > maximum_drop_label_bytes) {
        label.resize(maximum_drop_label_bytes);
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
        std::string display_label;
        FileContentKind content_kind;
        DecodedText decoded;
        std::vector<std::uint8_t> raw_bytes;
        Document document;
        std::string persisted_text;
    };

    struct CompensationState {
        FileDocumentId id;
        JournalDocumentKey prior_key;
        std::string prior_label;
        bool was_deleted = false;
        bool restores_document = false;
        bool mark_dirty_on_restore = false;
        std::optional<Entry> deleted_entry;
        std::optional<DecodedText> prior_decoded;
        std::string prior_persisted_text;
    };

    struct ReplacedWorkspace {
        WorkspaceReplacementId id;
        std::filesystem::path root;
        std::vector<Entry> entries;
        std::vector<std::string> recent;
    };

    Impl(std::filesystem::path canonical_root, RecoveryActions& actions)
        : root(std::move(canonical_root)), recovery(actions) {}

    std::filesystem::path root;
    RecoveryActions& recovery;
    std::uint64_t next_document = 1;
    std::uint64_t next_workspace_replacement = 1;
    std::vector<Entry> entries;
    std::vector<std::string> recent;
    std::unordered_map<std::string, CompensationState> compensations;
    std::optional<ReplacedWorkspace> replaced_workspace;

    Entry* find(FileDocumentId id) {
        const auto found = std::find_if(
            entries.begin(), entries.end(),
            [id](const Entry& entry) { return entry.id == id; });
        return found == entries.end() ? nullptr : &*found;
    }

    const Entry* find(FileDocumentId id) const {
        return const_cast<Impl*>(this)->find(id);
    }

    Entry* find_path(std::string_view path) {
        const auto found = std::find_if(entries.begin(), entries.end(),
                                        [path](const Entry& entry) {
                                            return entry.key.kind() ==
                                                       JournalDocumentKeyKind::
                                                           saved &&
                                                   entry.key.saved_path() ==
                                                       path;
                                        });
        return found == entries.end() ? nullptr : &*found;
    }

    std::optional<std::filesystem::path> resolve(std::string_view raw,
                                                 bool must_exist,
                                                 WorkspaceResult& error) const {
        const auto supplied = std::filesystem::path{raw};
        const auto traverses = std::find(supplied.begin(), supplied.end(),
                                         std::filesystem::path{".."}) !=
                                supplied.end();
        const auto normalized = normalized_relative(raw);
        const auto validation =
            validate_workspace_relative_path(normalized, native_syntax());
        if (supplied.is_absolute() || traverses || !validation.valid()) {
            error = failure(WorkspaceError::invalid_path,
                            "path must be workspace-relative and contain no "
                            "traversal");
            return std::nullopt;
        }
        const auto relative = std::filesystem::path{normalized};
        std::error_code code;
        const auto candidate = must_exist
                                   ? std::filesystem::weakly_canonical(
                                         root / relative, code)
                                   : std::filesystem::weakly_canonical(
                                         root / relative.parent_path(), code) /
                                         relative.filename();
        if (code) {
            error = failure(WorkspaceError::io_failed, code.message());
            return std::nullopt;
        }
        if (!is_beneath(root, candidate)) {
            error = failure(WorkspaceError::path_outside_workspace,
                            "path resolves outside the workspace");
            return std::nullopt;
        }
        if (must_exist && !std::filesystem::exists(candidate)) {
            error = failure(WorkspaceError::not_found, "path does not exist");
            return std::nullopt;
        }
        return candidate;
    }

    void touch_recent(std::string path) {
        recent.erase(std::remove(recent.begin(), recent.end(), path),
                     recent.end());
        recent.insert(recent.begin(), std::move(path));
        if (recent.size() > maximum_recent_files) {
            recent.resize(maximum_recent_files);
        }
    }

    WorkspaceResult add_bytes(std::vector<std::uint8_t> bytes,
                              JournalDocumentKey key,
                              std::string label,
                              bool dirty) {
        const auto id = FileDocumentId{next_document++};
        if (contains_nul(as_unsigned_bytes(bytes))) {
            entries.push_back({id, std::move(key), std::move(label),
                               FileContentKind::binary, {}, std::move(bytes),
                               Document{"", DocumentMode::read_only}, {}});
        } else {
            auto decoded = decode_text(as_unsigned_bytes(bytes));
            if (!decoded.accepted()) {
                entries.push_back(
                    {id, std::move(key), std::move(label),
                     FileContentKind::decode_failure, {}, std::move(bytes),
                     Document{"", DocumentMode::read_only}, {}});
            } else {
                const auto persisted = dirty ? std::string{} : decoded.text->utf8;
                auto document = Document{decoded.text->utf8};
                entries.push_back(
                    {id, std::move(key), std::move(label),
                     FileContentKind::text, std::move(*decoded.text),
                     std::move(bytes), std::move(document), persisted});
            }
        }
        WorkspaceResult result;
        result.document = id;
        return result;
    }

    WorkspaceResult save_to(Entry& entry, std::string path,
                            const std::filesystem::path& absolute) {
        if (entry.content_kind != FileContentKind::text) {
            return failure(WorkspaceError::read_only,
                           "read-only content cannot be saved");
        }
        const auto current = entry.document.snapshot().text;
        auto decoded = text_for_save(entry.decoded, current);
        const auto encoded = encode_text(decoded);
        if (!encoded.accepted()) {
            return failure(WorkspaceError::decode_failed,
                           encoded.error->message);
        }
        try {
            WorkspaceResult result;
            if (std::filesystem::exists(absolute)) {
                const auto prior_persisted = entry.persisted_text;
                const auto replacement = to_bytes(encoded.bytes);
                const auto action =
                    recovery.overwrite_file(absolute, replacement);
                if (!action.accepted()) {
                    return failure(WorkspaceError::recovery_failed,
                                   action.error->message);
                }
                result.compensation = action.compensation;
                if (action.compensation) {
                    CompensationState state{entry.id, entry.key,
                                            entry.display_label};
                    state.mark_dirty_on_restore = true;
                    state.prior_persisted_text = prior_persisted;
                    compensations.emplace(
                        std::string{action.compensation->value()},
                        std::move(state));
                }
            } else {
                replace_file_atomically(absolute, as_bytes(encoded.bytes));
            }
            entry.key = JournalDocumentKey::saved(path);
            entry.display_label =
                std::filesystem::path{path}.filename().string();
            entry.decoded = std::move(decoded);
            entry.raw_bytes = encoded.bytes;
            entry.persisted_text = current;
            touch_recent(std::move(path));
            result.document = entry.id;
            return result;
        } catch (const std::exception& exception) {
            return failure(WorkspaceError::io_failed, exception.what());
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
    FileDocumentId document_id) const {
    const auto* entry = impl_->find(document_id);
    if (!entry) {
        return std::nullopt;
    }
    const auto text = entry->document.snapshot().text;
    return WorkspaceDocumentState{
        entry->id,
        entry->key,
        entry->display_label,
        entry->content_kind,
        entry->decoded.status,
        entry->key.kind() == JournalDocumentKeyKind::untitled ||
            text != entry->persisted_text,
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
        return {DocumentError::invalid_range, Revision{0},
                "workspace document does not exist"};
    }
    const auto result = entry->document.apply(transaction);
    if (result.accepted()) {
        apply_terminator_edits(entry->decoded, transaction);
    }
    return result;
}

std::vector<std::string> Workspace::recent_files() const {
    return impl_->recent;
}

WorkspaceResult Workspace::open_directory(
    const std::filesystem::path& path) {
    std::error_code code;
    const auto canonical = std::filesystem::canonical(path, code);
    if (code || !std::filesystem::is_directory(canonical)) {
        return failure(WorkspaceError::invalid_workspace,
                       "workspace path must be an existing directory");
    }
    const auto replacement =
        WorkspaceReplacementId{impl_->next_workspace_replacement++};
    impl_->replaced_workspace = Impl::ReplacedWorkspace{
        replacement, impl_->root, std::move(impl_->entries),
        std::move(impl_->recent)};
    impl_->root = canonical;
    impl_->entries.clear();
    impl_->recent.clear();
    WorkspaceResult result;
    result.workspace_compensation = replacement;
    return result;
}

WorkspaceResult Workspace::restore_workspace(
    WorkspaceReplacementId replacement) {
    if (!impl_->replaced_workspace ||
        impl_->replaced_workspace->id != replacement) {
        return failure(WorkspaceError::not_found,
                       "workspace compensation does not exist");
    }
    impl_->root = std::move(impl_->replaced_workspace->root);
    impl_->entries = std::move(impl_->replaced_workspace->entries);
    impl_->recent = std::move(impl_->replaced_workspace->recent);
    impl_->replaced_workspace.reset();
    return {};
}

WorkspaceResult Workspace::new_document(std::string_view suggested_label) {
    return impl_->add_bytes(
        {}, JournalDocumentKey::untitled(UntitledDocumentId::generate()),
        suggested_label.empty() ? "Untitled" : sanitize_label(suggested_label),
        true);
}

WorkspaceResult Workspace::open_file(std::string_view raw_path) {
    WorkspaceResult path_error;
    const auto absolute = impl_->resolve(raw_path, true, path_error);
    if (!absolute) {
        return path_error;
    }
    const auto path = normalized_relative(raw_path);
    if (auto* existing = impl_->find_path(path)) {
        WorkspaceResult result;
        result.document = existing->id;
        impl_->touch_recent(path);
        return result;
    }
    if (!std::filesystem::is_regular_file(*absolute)) {
        return failure(WorkspaceError::not_found,
                       "path is not a regular file");
    }
    try {
        auto result = impl_->add_bytes(
            read_file(*absolute), JournalDocumentKey::saved(path),
            absolute->filename().string(), false);
        impl_->touch_recent(path);
        return result;
    } catch (const std::exception& exception) {
        return failure(WorkspaceError::io_failed, exception.what());
    }
}

WorkspaceResult Workspace::open_recent(std::size_t index) {
    if (index >= impl_->recent.size()) {
        return failure(WorkspaceError::not_found,
                       "recent-file index does not exist");
    }
    const auto path = impl_->recent[index];
    const auto result = open_file(path);
    if (result.error == WorkspaceError::not_found ||
        result.error == WorkspaceError::io_failed) {
        impl_->recent.erase(
            std::remove(impl_->recent.begin(), impl_->recent.end(), path),
            impl_->recent.end());
    }
    return result;
}

WorkspaceResult Workspace::open_dropped_content(
    const InvocationPrincipal& principal,
    std::span<const std::uint8_t> bytes,
    std::string_view suggested_label) {
    if (principal.origin() == InvocationOrigin::lua ||
        !principal.has_capability(CapabilityId{"local_file_drop"})) {
        return failure(WorkspaceError::capability_denied,
                       "local file drop capability is required");
    }
    return impl_->add_bytes(
        {bytes.begin(), bytes.end()},
        JournalDocumentKey::untitled(UntitledDocumentId::generate()),
        sanitize_label(suggested_label), true);
}

WorkspaceResult Workspace::save(FileDocumentId id) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::not_found,
                       "workspace document does not exist");
    }
    if (entry->key.kind() != JournalDocumentKeyKind::saved) {
        return failure(WorkspaceError::invalid_path,
                       "untitled document requires save_as");
    }
    WorkspaceResult path_error;
    const auto absolute =
        impl_->resolve(entry->key.saved_path(), false, path_error);
    if (!absolute) {
        return path_error;
    }
    return impl_->save_to(*entry, entry->key.saved_path(), *absolute);
}

WorkspaceResult Workspace::save_all() {
    WorkspaceResult aggregate;
    const auto ids = documents();
    for (const auto id : ids) {
        const auto current = state(id);
        if (!current || !current->dirty ||
            current->key.kind() != JournalDocumentKeyKind::saved) {
            continue;
        }
        const auto saved = save(id);
        if (!saved.accepted()) {
            aggregate.failures.push_back(
                {id, saved.error, std::move(saved.message)});
        }
    }
    if (!aggregate.failures.empty()) {
        aggregate.error = WorkspaceError::partial_failure;
        aggregate.message = "one or more documents could not be saved";
    }
    return aggregate;
}

WorkspaceResult Workspace::save_as(FileDocumentId id,
                                   std::string_view raw_path) {
    auto* entry = impl_->find(id);
    if (!entry) {
        return failure(WorkspaceError::not_found,
                       "workspace document does not exist");
    }
    WorkspaceResult path_error;
    const auto absolute = impl_->resolve(raw_path, false, path_error);
    if (!absolute) {
        return path_error;
    }
    const auto path = normalized_relative(raw_path);
    if (const auto* duplicate = impl_->find_path(path);
        duplicate && duplicate->id != id) {
        return failure(WorkspaceError::already_open,
                       "destination is already open");
    }
    return impl_->save_to(*entry, path, *absolute);
}

WorkspaceResult Workspace::reload(FileDocumentId id) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::saved) {
        return failure(WorkspaceError::not_found,
                       "saved workspace document does not exist");
    }
    WorkspaceResult path_error;
    const auto absolute =
        impl_->resolve(entry->key.saved_path(), true, path_error);
    if (!absolute) {
        return path_error;
    }
    try {
        const auto bytes = read_file(*absolute);
        if (contains_nul(as_unsigned_bytes(bytes))) {
            return failure(WorkspaceError::decode_failed,
                           "binary file cannot replace an editable document");
        }
        auto decoded = decode_text(as_unsigned_bytes(bytes));
        if (!decoded.accepted()) {
            return failure(WorkspaceError::decode_failed,
                           decoded.error->message);
        }
        std::optional<JournalDocument> document_before{
            JournalDocument{entry->key, entry->document.mode(),
                            state(id)->dirty, entry->document.snapshot().text}};
        const JournalDocument replacement{
            entry->key, entry->document.mode(), false, decoded.text->utf8};
        const auto action =
            impl_->recovery.reload_document(document_before, replacement);
        if (!action.accepted()) {
            return failure(WorkspaceError::recovery_failed,
                           action.error->message);
        }
        if (action.compensation) {
            Impl::CompensationState state{id, entry->key,
                                          entry->display_label};
            state.restores_document = true;
            state.prior_decoded = entry->decoded;
            state.prior_persisted_text = entry->persisted_text;
            impl_->compensations.emplace(
                std::string{action.compensation->value()}, std::move(state));
        }
        entry->document = Document{decoded.text->utf8};
        entry->decoded = std::move(*decoded.text);
        entry->raw_bytes = bytes;
        entry->persisted_text = entry->document.snapshot().text;
        WorkspaceResult result;
        result.document = id;
        result.compensation = action.compensation;
        return result;
    } catch (const std::exception& exception) {
        return failure(WorkspaceError::io_failed, exception.what());
    }
}

WorkspaceResult Workspace::rename_file(FileDocumentId id,
                                      std::string_view raw_path) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::saved) {
        return failure(WorkspaceError::not_found,
                       "saved workspace document does not exist");
    }
    WorkspaceResult source_error;
    const auto source =
        impl_->resolve(entry->key.saved_path(), true, source_error);
    if (!source) {
        return source_error;
    }
    WorkspaceResult destination_error;
    const auto destination =
        impl_->resolve(raw_path, false, destination_error);
    if (!destination) {
        return destination_error;
    }
    const auto path = normalized_relative(raw_path);
    if (const auto* duplicate = impl_->find_path(path);
        duplicate && duplicate->id != id) {
        return failure(WorkspaceError::already_open,
                       "destination is already open");
    }
    const auto action = impl_->recovery.rename_path(*source, *destination);
    if (!action.accepted()) {
        return failure(WorkspaceError::recovery_failed,
                       action.error->message);
    }
    if (action.compensation) {
        impl_->compensations.emplace(
            std::string{action.compensation->value()},
            Impl::CompensationState{id, entry->key, entry->display_label});
    }
    entry->key = JournalDocumentKey::saved(path);
    entry->display_label = destination->filename().string();
    impl_->touch_recent(path);
    WorkspaceResult result;
    result.document = id;
    result.compensation = action.compensation;
    return result;
}

WorkspaceResult Workspace::delete_file(FileDocumentId id) {
    auto* entry = impl_->find(id);
    if (!entry || entry->key.kind() != JournalDocumentKeyKind::saved) {
        return failure(WorkspaceError::not_found,
                       "saved workspace document does not exist");
    }
    WorkspaceResult path_error;
    const auto path =
        impl_->resolve(entry->key.saved_path(), true, path_error);
    if (!path) {
        return path_error;
    }
    const auto action = impl_->recovery.delete_path(*path);
    if (!action.accepted()) {
        return failure(WorkspaceError::recovery_failed,
                       action.error->message);
    }
    WorkspaceResult result;
    result.compensation = action.compensation;
    result.document = id;
    auto removed = std::find_if(
        impl_->entries.begin(), impl_->entries.end(),
        [id](const Impl::Entry& candidate) { return candidate.id == id; });
    if (action.compensation) {
        Impl::CompensationState state{id, removed->key, removed->display_label};
        state.was_deleted = true;
        state.deleted_entry = std::move(*removed);
        impl_->compensations.emplace(
            std::string{action.compensation->value()}, std::move(state));
    }
    impl_->entries.erase(removed);
    return result;
}

WorkspaceResult Workspace::new_directory(std::string_view raw_path) {
    WorkspaceResult path_error;
    const auto path = impl_->resolve(raw_path, false, path_error);
    if (!path) {
        return path_error;
    }
    std::error_code code;
    if (!std::filesystem::create_directory(*path, code) || code) {
        return failure(WorkspaceError::io_failed,
                       code ? code.message() : "directory already exists");
    }
    return {};
}

WorkspaceResult Workspace::restore(
    const RecoveryRecordId& compensation) {
    const auto found =
        impl_->compensations.find(std::string{compensation.value()});
    if (found != impl_->compensations.end() &&
        found->second.restores_document) {
        auto* entry = impl_->find(found->second.id);
        if (!entry) {
            return failure(WorkspaceError::not_found,
                           "document compensation target does not exist");
        }
        std::optional<JournalDocument> restored_document{
            JournalDocument{entry->key, entry->document.mode(), false,
                            entry->document.snapshot().text}};
        const auto restored = impl_->recovery.restore_document(
            compensation, restored_document);
        if (!restored.accepted()) {
            return failure(WorkspaceError::recovery_failed,
                           restored.error->message);
        }
        entry->document = Document{restored_document->utf8_content,
                                   restored_document->mode};
        if (found->second.prior_decoded) {
            entry->decoded = *found->second.prior_decoded;
        }
        entry->persisted_text = restored_document->dirty
                                    ? found->second.prior_persisted_text
                                    : restored_document->utf8_content;
        impl_->compensations.erase(found);
        return {};
    }
    const auto restored = impl_->recovery.restore_filesystem(compensation);
    if (!restored.accepted()) {
        return failure(WorkspaceError::recovery_failed,
                       restored.error->message);
    }
    if (found != impl_->compensations.end()) {
        if (found->second.was_deleted && found->second.deleted_entry) {
            impl_->entries.push_back(
                std::move(*found->second.deleted_entry));
        } else if (auto* entry = impl_->find(found->second.id)) {
            entry->key = found->second.prior_key;
            entry->display_label = found->second.prior_label;
            if (found->second.mark_dirty_on_restore) {
                entry->persisted_text = found->second.prior_persisted_text;
            }
        }
        impl_->compensations.erase(found);
    }
    return {};
}

}  // namespace ssg
