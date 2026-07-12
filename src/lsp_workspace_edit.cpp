#include <ssg/lsp_workspace_edit.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ssg {
namespace {

std::string json_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20) {
                result += "\\u00";
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0f]);
            } else {
                result.push_back(static_cast<char>(byte));
            }
        }
    }
    result.push_back('"');
    return result;
}

void append_utf8(std::string& target, std::uint32_t value) {
    if (value <= 0x7f) {
        target.push_back(static_cast<char>(value));
    } else if (value <= 0x7ff) {
        target.push_back(static_cast<char>(0xc0 | (value >> 6)));
        target.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        target.push_back(static_cast<char>(0xe0 | (value >> 12)));
        target.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        target.push_back(static_cast<char>(0xf0 | (value >> 18)));
        target.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

struct Json {
    enum class Kind { null_value, boolean, number, string, array, object };
    Kind kind = Kind::null_value;
    bool boolean = false;
    std::string scalar;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    [[nodiscard]] const Json* member(std::string_view name) const {
        const auto found = object.find(std::string{name});
        return found == object.end() ? nullptr : &found->second;
    }

    [[nodiscard]] std::optional<std::int64_t> integer() const {
        if (kind != Kind::number) return std::nullopt;
        std::int64_t value = 0;
        const auto parsed =
            std::from_chars(scalar.data(), scalar.data() + scalar.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != scalar.data() + scalar.size()) {
            return std::nullopt;
        }
        return value;
    }
};

class JsonParser {
public:
    JsonParser(std::string_view input, std::size_t maximum_depth)
        : input_(input), maximum_depth_(maximum_depth) {}

    [[nodiscard]] std::optional<Json> parse() {
        auto value = parse_value(0);
        skip_space();
        return value && offset_ == input_.size() ? value : std::nullopt;
    }

private:
    void skip_space() {
        while (offset_ < input_.size() &&
               (input_[offset_] == ' ' || input_[offset_] == '\t' ||
                input_[offset_] == '\r' || input_[offset_] == '\n')) {
            ++offset_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (offset_ >= input_.size() || input_[offset_] != expected) {
            return false;
        }
        ++offset_;
        return true;
    }

    [[nodiscard]] std::optional<std::uint32_t> hex4() {
        if (offset_ + 4 > input_.size()) return std::nullopt;
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char ch = input_[offset_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value += ch - '0';
            else if (ch >= 'a' && ch <= 'f') value += ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') value += ch - 'A' + 10;
            else return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] std::optional<std::string> parse_string() {
        if (!consume('"')) return std::nullopt;
        std::string result;
        while (offset_ < input_.size()) {
            const char ch = input_[offset_++];
            if (ch == '"') return result;
            if (static_cast<unsigned char>(ch) < 0x20) return std::nullopt;
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            if (offset_ >= input_.size()) return std::nullopt;
            switch (input_[offset_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                auto first = hex4();
                if (!first) return std::nullopt;
                std::uint32_t value = *first;
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (offset_ + 2 > input_.size() || input_[offset_] != '\\' ||
                        input_[offset_ + 1] != 'u') {
                        return std::nullopt;
                    }
                    offset_ += 2;
                    auto second = hex4();
                    if (!second || *second < 0xdc00 || *second > 0xdfff) {
                        return std::nullopt;
                    }
                    value = 0x10000 + ((value - 0xd800) << 10) +
                            (*second - 0xdc00);
                } else if (value >= 0xdc00 && value <= 0xdfff) {
                    return std::nullopt;
                }
                append_utf8(result, value);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Json> parse_value(std::size_t depth) {
        skip_space();
        if (depth > maximum_depth_ || offset_ >= input_.size()) {
            return std::nullopt;
        }
        if (input_[offset_] == '"') {
            auto text = parse_string();
            if (!text) return std::nullopt;
            Json value;
            value.kind = Json::Kind::string;
            value.scalar = std::move(*text);
            return value;
        }
        if (input_[offset_] == '{') {
            ++offset_;
            Json value;
            value.kind = Json::Kind::object;
            if (consume('}')) return value;
            while (true) {
                auto key = parse_string();
                if (!key || !consume(':')) return std::nullopt;
                auto child = parse_value(depth + 1);
                if (!child ||
                    !value.object.emplace(std::move(*key), std::move(*child))
                         .second) {
                    return std::nullopt;
                }
                if (consume('}')) return value;
                if (!consume(',')) return std::nullopt;
            }
        }
        if (input_[offset_] == '[') {
            ++offset_;
            Json value;
            value.kind = Json::Kind::array;
            if (consume(']')) return value;
            while (true) {
                auto child = parse_value(depth + 1);
                if (!child) return std::nullopt;
                value.array.push_back(std::move(*child));
                if (consume(']')) return value;
                if (!consume(',')) return std::nullopt;
            }
        }
        if (input_.substr(offset_, 4) == "null") {
            offset_ += 4;
            return Json{};
        }
        if (input_.substr(offset_, 4) == "true") {
            offset_ += 4;
            Json value;
            value.kind = Json::Kind::boolean;
            value.boolean = true;
            return value;
        }
        if (input_.substr(offset_, 5) == "false") {
            offset_ += 5;
            Json value;
            value.kind = Json::Kind::boolean;
            value.boolean = false;
            return value;
        }
        const auto start = offset_;
        if (input_[offset_] == '-') ++offset_;
        if (offset_ >= input_.size()) return std::nullopt;
        if (input_[offset_] == '0') {
            ++offset_;
        } else if (input_[offset_] >= '1' && input_[offset_] <= '9') {
            while (offset_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
        } else {
            return std::nullopt;
        }
        if (offset_ < input_.size() && input_[offset_] == '.') {
            ++offset_;
            const auto fraction = offset_;
            while (offset_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
            if (fraction == offset_) return std::nullopt;
        }
        if (offset_ < input_.size() &&
            (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < input_.size() &&
                (input_[offset_] == '+' || input_[offset_] == '-')) {
                ++offset_;
            }
            const auto exponent = offset_;
            while (offset_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
            if (exponent == offset_) return std::nullopt;
        }
        Json value;
        value.kind = Json::Kind::number;
        value.scalar = std::string{input_.substr(start, offset_ - start)};
        return value;
    }

    std::string_view input_;
    std::size_t maximum_depth_;
    std::size_t offset_ = 0;
};

std::string json_stringify(const Json& value) {
    switch (value.kind) {
    case Json::Kind::null_value: return "null";
    case Json::Kind::boolean: return value.boolean ? "true" : "false";
    case Json::Kind::number: return value.scalar;
    case Json::Kind::string: return json_escape(value.scalar);
    case Json::Kind::array: {
        std::string out{"["};
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            if (index != 0) out.push_back(',');
            out += json_stringify(value.array[index]);
        }
        out.push_back(']');
        return out;
    }
    case Json::Kind::object: {
        std::string out{"{"};
        std::size_t index = 0;
        for (const auto& [key, child] : value.object) {
            if (index++ != 0) out.push_back(',');
            out += json_escape(key);
            out.push_back(':');
            out += json_stringify(child);
        }
        out.push_back('}');
        return out;
    }
    }
    return {};
}

std::optional<LspPosition> parse_position(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* line = value.member("line");
    const auto* character = value.member("character");
    if (!line || !character) return std::nullopt;
    const auto parsed_line = line->integer();
    const auto parsed_character = character->integer();
    if (!parsed_line || !parsed_character || *parsed_line < 0 ||
        *parsed_character < 0) {
        return std::nullopt;
    }
    return LspPosition{static_cast<std::uint64_t>(*parsed_line),
                       static_cast<std::uint64_t>(*parsed_character)};
}

std::optional<LspRange> parse_range(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* start = value.member("start");
    const auto* end = value.member("end");
    if (!start || !end) return std::nullopt;
    const auto parsed_start = parse_position(*start);
    const auto parsed_end = parse_position(*end);
    if (!parsed_start || !parsed_end ||
        parsed_end->line < parsed_start->line ||
        (parsed_end->line == parsed_start->line &&
         parsed_end->character < parsed_start->character)) {
        return std::nullopt;
    }
    return LspRange{*parsed_start, *parsed_end};
}

struct ParsedTextEdit {
    LspRange range;
    std::string new_text;
};

std::optional<ParsedTextEdit> parse_text_edit(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* range = value.member("range");
    const auto* text = value.member("newText");
    if (!range || !text || text->kind != Json::Kind::string) {
        return std::nullopt;
    }
    const auto parsed_range = parse_range(*range);
    if (!parsed_range) return std::nullopt;
    return ParsedTextEdit{*parsed_range, text->scalar};
}

struct ParsedDocumentEdit {
    std::string uri;
    std::optional<std::int64_t> version;
    std::vector<ParsedTextEdit> edits;
};

enum class ParsedFileOperationKind : std::uint8_t { create, rename, remove };

struct ParsedFileOperation {
    ParsedFileOperationKind kind = ParsedFileOperationKind::create;
    std::string uri;
    std::string secondary_uri;
    bool overwrite = false;
    bool ignore_if_exists = false;
    bool recursive = false;
    bool ignore_if_not_exists = false;
};

using ParsedOperation = std::variant<ParsedDocumentEdit, ParsedFileOperation>;

std::optional<ParsedDocumentEdit> parse_document_edit_entry(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* text_document = value.member("textDocument");
    const auto* edits = value.member("edits");
    if (!text_document || !edits || text_document->kind != Json::Kind::object ||
        edits->kind != Json::Kind::array) {
        return std::nullopt;
    }
    const auto* uri = text_document->member("uri");
    if (!uri || uri->kind != Json::Kind::string || uri->scalar.empty()) {
        return std::nullopt;
    }
    ParsedDocumentEdit parsed;
    parsed.uri = uri->scalar;
    if (const auto* version = text_document->member("version")) {
        if (version->kind == Json::Kind::null_value) {
            parsed.version = std::nullopt;
        } else {
            const auto parsed_version = version->integer();
            if (!parsed_version) return std::nullopt;
            parsed.version = *parsed_version;
        }
    }
    parsed.edits.reserve(edits->array.size());
    for (const auto& edit : edits->array) {
        auto parsed_edit = parse_text_edit(edit);
        if (!parsed_edit) return std::nullopt;
        parsed.edits.push_back(std::move(*parsed_edit));
    }
    return parsed;
}

std::optional<ParsedFileOperation> parse_file_operation_entry(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* kind = value.member("kind");
    if (!kind || kind->kind != Json::Kind::string) return std::nullopt;
    ParsedFileOperation parsed;
    const auto* options = value.member("options");
    if (options && options->kind != Json::Kind::object) return std::nullopt;

    const auto boolean_option =
        [&](std::string_view name, bool fallback,
            bool& target) -> bool {
        target = fallback;
        if (!options) return true;
        const auto* option = options->member(name);
        if (!option) return true;
        if (option->kind != Json::Kind::boolean) return false;
        target = option->boolean;
        return true;
    };

    if (kind->scalar == "create") {
        parsed.kind = ParsedFileOperationKind::create;
        const auto* uri = value.member("uri");
        if (!uri || uri->kind != Json::Kind::string || uri->scalar.empty()) {
            return std::nullopt;
        }
        parsed.uri = uri->scalar;
        if (!boolean_option("overwrite", false, parsed.overwrite) ||
            !boolean_option("ignoreIfExists", false, parsed.ignore_if_exists)) {
            return std::nullopt;
        }
        return parsed;
    }
    if (kind->scalar == "rename") {
        parsed.kind = ParsedFileOperationKind::rename;
        const auto* old_uri = value.member("oldUri");
        const auto* new_uri = value.member("newUri");
        if (!old_uri || !new_uri || old_uri->kind != Json::Kind::string ||
            new_uri->kind != Json::Kind::string || old_uri->scalar.empty() ||
            new_uri->scalar.empty()) {
            return std::nullopt;
        }
        parsed.uri = old_uri->scalar;
        parsed.secondary_uri = new_uri->scalar;
        if (!boolean_option("overwrite", false, parsed.overwrite) ||
            !boolean_option("ignoreIfExists", false, parsed.ignore_if_exists)) {
            return std::nullopt;
        }
        return parsed;
    }
    if (kind->scalar == "delete") {
        parsed.kind = ParsedFileOperationKind::remove;
        const auto* uri = value.member("uri");
        if (!uri || uri->kind != Json::Kind::string || uri->scalar.empty()) {
            return std::nullopt;
        }
        parsed.uri = uri->scalar;
        if (!boolean_option("recursive", false, parsed.recursive) ||
            !boolean_option("ignoreIfNotExists", false,
                            parsed.ignore_if_not_exists)) {
            return std::nullopt;
        }
        return parsed;
    }
    return std::nullopt;
}

std::optional<std::vector<ParsedOperation>> parse_workspace_edit(
    const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    std::vector<ParsedOperation> operations;
    const auto* document_changes = value.member("documentChanges");
    if (!document_changes) {
        const auto* changes = value.member("changes");
        if (!changes) return operations;
        if (changes->kind != Json::Kind::object) return std::nullopt;
        for (const auto& [uri, entries] : changes->object) {
            if (entries.kind != Json::Kind::array) return std::nullopt;
            ParsedDocumentEdit parsed;
            parsed.uri = uri;
            parsed.edits.reserve(entries.array.size());
            for (const auto& entry : entries.array) {
                auto edit = parse_text_edit(entry);
                if (!edit) return std::nullopt;
                parsed.edits.push_back(std::move(*edit));
            }
            operations.emplace_back(std::move(parsed));
        }
    } else {
        if (document_changes->kind != Json::Kind::array) return std::nullopt;
        for (const auto& entry : document_changes->array) {
            if (entry.kind != Json::Kind::object) return std::nullopt;
            if (entry.member("textDocument")) {
                auto parsed = parse_document_edit_entry(entry);
                if (!parsed) return std::nullopt;
                operations.emplace_back(std::move(*parsed));
            } else {
                auto parsed = parse_file_operation_entry(entry);
                if (!parsed) return std::nullopt;
                operations.emplace_back(std::move(*parsed));
            }
        }
    }
    return operations;
}

struct PlannedDocumentOperation {
    std::string uri;
    Revision expected_revision{0};
    std::string old_text;
    std::string new_text;
};

struct PlannedFileOperation {
    ParsedFileOperation source;
    LspWorkspaceFileNode before;
    LspWorkspaceFileNode before_secondary;
};

using PlannedOperation = std::variant<PlannedDocumentOperation, PlannedFileOperation>;

struct ResolvedEditRange {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string replacement;
};

std::optional<std::vector<ResolvedEditRange>> resolve_ranges(
    std::string_view text, const std::vector<ParsedTextEdit>& edits,
    std::string& message) {
    std::vector<ResolvedEditRange> ranges;
    ranges.reserve(edits.size());
    for (const auto& edit : edits) {
        const auto start = lsp_position_to_byte_offset(text, edit.range.start);
        const auto end = lsp_position_to_byte_offset(text, edit.range.end);
        if (!start.accepted() || !end.accepted() || end.offset < start.offset) {
            message = "workspace edit range is outside the target document";
            return std::nullopt;
        }
        ranges.push_back({static_cast<std::size_t>(start.offset.value()),
                          static_cast<std::size_t>(end.offset.value()),
                          edit.new_text});
    }
    std::stable_sort(ranges.begin(), ranges.end(),
                     [](const auto& left, const auto& right) {
        if (left.start != right.start) return left.start < right.start;
        return left.end < right.end;
    });
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index].start < ranges[index - 1].end) {
            message = "workspace edit contains overlapping text ranges";
            return std::nullopt;
        }
    }
    return ranges;
}

std::optional<std::string> apply_text_edits(
    std::string_view text, const std::vector<ParsedTextEdit>& edits,
    LspWorkspaceEditError& error, std::string& message) {
    auto resolved = resolve_ranges(text, edits, message);
    if (!resolved) {
        error = message.find("overlapping") == std::string::npos
                    ? LspWorkspaceEditError::invalid_position
                    : LspWorkspaceEditError::overlapping_edits;
        return std::nullopt;
    }
    std::string updated{text};
    for (auto edit = resolved->rbegin(); edit != resolved->rend(); ++edit) {
        updated.replace(edit->start, edit->end - edit->start, edit->replacement);
    }
    return updated;
}

LspWorkspaceEditApplyResult failure(LspWorkspaceEditError error,
                                    std::string message) {
    return {error, std::move(message), std::nullopt};
}

struct AppliedOperation {
    std::vector<LspWorkspaceEditRecoveryOperation> inverse;
};

bool set_recovery_document_revisions(
    LspWorkspaceEditDocuments& documents,
    std::vector<LspWorkspaceEditRecoveryOperation>& operations) {
    std::map<std::string, std::uint64_t> next_revisions;
    for (auto& operation : operations) {
        if (operation.kind != LspWorkspaceEditRecoveryKind::document_text) {
            continue;
        }
        auto [found, inserted] =
            next_revisions.try_emplace(operation.uri, std::uint64_t{0});
        if (inserted) {
            const auto snapshot = documents.snapshot(operation.uri);
            if (!snapshot) return false;
            found->second = snapshot->revision.value();
        }
        if (found->second == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        operation.expected_revision = Revision{found->second++};
    }
    return true;
}

LspWorkspaceEditApplyResult execute_recovery_operations(
    LspWorkspaceEditDocuments& documents, LspWorkspaceFileOperations& files,
    const std::vector<LspWorkspaceEditRecoveryOperation>& operations) {
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        const auto remaining = [&] {
            return LspWorkspaceEditRecoveryRecord{
                std::vector<LspWorkspaceEditRecoveryOperation>{
                    operations.begin() +
                        static_cast<std::ptrdiff_t>(index),
                    operations.end()}};
        };
        switch (operation.kind) {
        case LspWorkspaceEditRecoveryKind::document_text: {
            const auto restored = documents.apply(
                operation.uri, operation.expected_revision, operation.text);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::rollback_failed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::restore_path: {
            const auto restored = files.restore_path(operation.uri, operation.node);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::rollback_failed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::write_file: {
            const auto restored = files.write_file(operation.uri, operation.text);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::rollback_failed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::delete_file: {
            const auto restored = files.delete_path(operation.uri, true);
            if (!restored.accepted() &&
                restored.error != LspWorkspaceFileError::not_found) {
                return {LspWorkspaceEditError::rollback_failed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::rename_file: {
            const auto restored = files.rename_path(
                operation.uri, operation.secondary_uri, operation.overwrite);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::rollback_failed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        }
    }
    return {LspWorkspaceEditError::none, {}, std::nullopt};
}

LspWorkspaceEditApplyResult rollback(
    LspWorkspaceEditDocuments& documents, LspWorkspaceFileOperations& files,
    const std::vector<AppliedOperation>& applied) {
    std::vector<LspWorkspaceEditRecoveryOperation> operations;
    for (auto operation = applied.rbegin(); operation != applied.rend();
         ++operation) {
        operations.insert(operations.end(), operation->inverse.begin(),
                          operation->inverse.end());
    }
    if (!set_recovery_document_revisions(documents, operations)) {
        return {LspWorkspaceEditError::rollback_failed,
                "workspace edit rollback revisions are unavailable",
                LspWorkspaceEditRecoveryRecord{std::move(operations)}};
    }
    return execute_recovery_operations(documents, files, operations);
}

} // namespace

LspWorkspaceEditCommandSet lsp_workspace_edit_command_set() { return {}; }

LspWorkspaceEditApplier::LspWorkspaceEditApplier(
    LspWorkspaceEditDocuments& documents, LspWorkspaceFileOperations& files,
    LspWorkspaceEditConfig config)
    : documents_(&documents), files_(&files), config_(config) {
    if (config_.maximum_json_depth == 0) {
        throw std::invalid_argument("LSP workspace edit depth must be non-zero");
    }
}

LspWorkspaceEditApplyResult LspWorkspaceEditApplier::apply(
    std::string_view workspace_edit_json) {
    const auto parsed_root =
        JsonParser{workspace_edit_json, config_.maximum_json_depth}.parse();
    if (!parsed_root) {
        return failure(LspWorkspaceEditError::malformed_edit,
                       "workspace edit payload is not valid JSON");
    }
    const auto parsed_operations = parse_workspace_edit(*parsed_root);
    if (!parsed_operations) {
        return failure(LspWorkspaceEditError::malformed_edit,
                       "workspace edit payload is malformed");
    }

    struct SimulatedDocument {
        Revision revision{0};
        std::int64_t version = 0;
        std::string text;
    };

    std::map<std::string, SimulatedDocument> documents;
    std::map<std::string, LspWorkspaceFileNode> files;
    std::vector<PlannedOperation> plan;
    plan.reserve(parsed_operations->size());

    const auto load_document = [&](std::string_view uri,
                                   std::map<std::string, SimulatedDocument>& cache,
                                   LspWorkspaceEditError& error,
                                   std::string& message)
        -> SimulatedDocument* {
        const auto key = std::string{uri};
        if (const auto found = cache.find(key); found != cache.end()) {
            return &found->second;
        }
        const auto snapshot = documents_->snapshot(uri);
        if (!snapshot) {
            error = LspWorkspaceEditError::unknown_document;
            message = "workspace edit references an unknown document";
            return nullptr;
        }
        return &cache.emplace(
                         key,
                         SimulatedDocument{snapshot->revision, snapshot->version,
                                           snapshot->text})
                    .first->second;
    };

    const auto load_file = [&](std::string_view uri,
                               std::map<std::string, LspWorkspaceFileNode>& cache,
                               LspWorkspaceEditError& error,
                               std::string& message)
        -> LspWorkspaceFileNode* {
        const auto key = std::string{uri};
        if (const auto found = cache.find(key); found != cache.end()) {
            return &found->second;
        }
        LspWorkspaceFileNode node;
        const auto snapshot = files_->snapshot(uri, node);
        if (!snapshot.accepted()) {
            error = LspWorkspaceEditError::file_conflict;
            message = snapshot.message;
            return nullptr;
        }
        return &cache.emplace(key, std::move(node)).first->second;
    };

    for (const auto& operation : *parsed_operations) {
        LspWorkspaceEditError error = LspWorkspaceEditError::none;
        std::string message;
        if (std::holds_alternative<ParsedDocumentEdit>(operation)) {
            const auto& edit = std::get<ParsedDocumentEdit>(operation);
            auto* document =
                load_document(edit.uri, documents, error, message);
            if (!document) return failure(error, std::move(message));
            if (edit.version && *edit.version != document->version) {
                return failure(LspWorkspaceEditError::stale_revision,
                               "workspace edit carries a stale document version");
            }
            auto updated = apply_text_edits(document->text, edit.edits, error,
                                            message);
            if (!updated) return failure(error, std::move(message));
            plan.emplace_back(PlannedDocumentOperation{
                edit.uri, document->revision, document->text, *updated});
            document->text = std::move(*updated);
            document->revision = Revision{document->revision.value() + 1};
            ++document->version;
            continue;
        }

        const auto& file = std::get<ParsedFileOperation>(operation);
        auto* current = load_file(file.uri, files, error, message);
        if (!current) return failure(error, std::move(message));

        if (file.kind == ParsedFileOperationKind::create) {
            if (current->kind == LspWorkspaceFileNodeKind::directory) {
                return failure(LspWorkspaceEditError::file_conflict,
                               "workspace edit cannot create over a directory");
            }
            if (current->kind == LspWorkspaceFileNodeKind::file &&
                file.ignore_if_exists) {
                continue;
            }
            if (current->kind == LspWorkspaceFileNodeKind::file &&
                !file.overwrite) {
                return failure(
                    LspWorkspaceEditError::file_conflict,
                    "workspace edit create target already exists");
            }
            plan.emplace_back(PlannedFileOperation{file, *current, {}});
            *current = {LspWorkspaceFileNodeKind::file, {}};
            continue;
        }

        if (file.kind == ParsedFileOperationKind::rename) {
            auto* destination = load_file(file.secondary_uri, files, error,
                                          message);
            if (!destination) return failure(error, std::move(message));
            if (current->kind == LspWorkspaceFileNodeKind::missing) {
                return failure(
                    LspWorkspaceEditError::file_conflict,
                    "workspace edit rename source does not exist");
            }
            if (destination->kind != LspWorkspaceFileNodeKind::missing &&
                file.ignore_if_exists) {
                continue;
            }
            if (destination->kind != LspWorkspaceFileNodeKind::missing &&
                !file.overwrite) {
                return failure(
                    LspWorkspaceEditError::file_conflict,
                    "workspace edit rename destination already exists");
            }
            plan.emplace_back(PlannedFileOperation{file, *current, *destination});
            *destination = *current;
            *current = {LspWorkspaceFileNodeKind::missing, {}};
            continue;
        }

        if (current->kind == LspWorkspaceFileNodeKind::missing &&
            file.ignore_if_not_exists) {
            continue;
        }
        if (current->kind == LspWorkspaceFileNodeKind::directory &&
            !file.recursive) {
            return failure(LspWorkspaceEditError::file_conflict,
                           "workspace edit directory delete requires recursive "
                           "option");
        }
        if (current->kind == LspWorkspaceFileNodeKind::missing) {
            return failure(LspWorkspaceEditError::file_conflict,
                           "workspace edit delete target does not exist");
        }
        plan.emplace_back(PlannedFileOperation{file, *current, {}});
        *current = {LspWorkspaceFileNodeKind::missing, {}};
    }

    std::vector<AppliedOperation> applied;
    applied.reserve(plan.size());

    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
        document_revision_budgets;
    for (const auto& operation : plan) {
        if (!std::holds_alternative<PlannedDocumentOperation>(operation)) {
            continue;
        }
        const auto& edit = std::get<PlannedDocumentOperation>(operation);
        auto [budget, inserted] = document_revision_budgets.try_emplace(
            edit.uri,
            std::pair{edit.expected_revision.value(), std::uint64_t{0}});
        ++budget->second.second;
    }
    for (const auto& [uri, budget] : document_revision_budgets) {
        (void)uri;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (budget.second > maximum / 2 ||
            budget.first > maximum - budget.second * 2) {
            return failure(
                LspWorkspaceEditError::apply_failed,
                "workspace edit lacks revision capacity for compensation");
        }
    }

    for (const auto& operation : plan) {
        if (std::holds_alternative<PlannedDocumentOperation>(operation)) {
            const auto& edit = std::get<PlannedDocumentOperation>(operation);
            const auto changed =
                documents_->apply(edit.uri, edit.expected_revision, edit.new_text);
            if (!changed.accepted()) {
                const auto undone = rollback(*documents_, *files_, applied);
                if (undone.accepted()) {
                    return failure(LspWorkspaceEditError::apply_failed,
                                   "workspace document write failed: " +
                                       changed.message);
                }
                return undone;
            }
            applied.push_back({{LspWorkspaceEditRecoveryOperation{
                LspWorkspaceEditRecoveryKind::document_text,
                edit.uri,
                {},
                changed.revision,
                edit.old_text,
                {},
                false,
                false,
            }}});
            continue;
        }

        const auto& file = std::get<PlannedFileOperation>(operation);
        if (file.source.kind == ParsedFileOperationKind::create) {
            const auto created =
                files_->create_file(file.source.uri, file.source.overwrite);
            if (!created.accepted()) {
                const auto undone = rollback(*documents_, *files_, applied);
                if (undone.accepted()) {
                    return failure(LspWorkspaceEditError::apply_failed,
                                   "workspace file create failed: " +
                                       created.message);
                }
                return undone;
            }
            if (file.before.kind == LspWorkspaceFileNodeKind::missing) {
                applied.push_back({{LspWorkspaceEditRecoveryOperation{
                    LspWorkspaceEditRecoveryKind::restore_path,
                    file.source.uri,
                    {},
                    Revision{0},
                    {},
                    file.before,
                    true,
                    false,
                }}});
            } else {
                applied.push_back({{LspWorkspaceEditRecoveryOperation{
                    LspWorkspaceEditRecoveryKind::restore_path,
                    file.source.uri,
                    {},
                    Revision{0},
                    {},
                    file.before,
                    false,
                    false,
                }}});
            }
            continue;
        }

        if (file.source.kind == ParsedFileOperationKind::rename) {
            const auto renamed = files_->rename_path(file.source.uri,
                                                     file.source.secondary_uri,
                                                     file.source.overwrite);
            if (!renamed.accepted()) {
                const auto undone = rollback(*documents_, *files_, applied);
                if (undone.accepted()) {
                    return failure(LspWorkspaceEditError::apply_failed,
                                   "workspace file rename failed: " +
                                       renamed.message);
                }
                return undone;
            }
            std::vector<LspWorkspaceEditRecoveryOperation> inverse{ {
                LspWorkspaceEditRecoveryKind::restore_path,
                file.source.uri,
                {},
                Revision{0},
                {},
                file.before,
                false,
                false,
            }};
            inverse.push_back({
                LspWorkspaceEditRecoveryKind::restore_path,
                file.source.secondary_uri,
                {},
                Revision{0},
                {},
                file.before_secondary,
                false,
                false,
            });
            applied.push_back({std::move(inverse)});
            continue;
        }

        const auto removed =
            files_->delete_path(file.source.uri, file.source.recursive);
        if (!removed.accepted()) {
            const auto undone = rollback(*documents_, *files_, applied);
            if (undone.accepted()) {
                return failure(LspWorkspaceEditError::apply_failed,
                               "workspace file delete failed: " +
                                   removed.message);
            }
            return undone;
        }
        applied.push_back({{LspWorkspaceEditRecoveryOperation{
            LspWorkspaceEditRecoveryKind::restore_path,
            file.source.uri,
            {},
            Revision{0},
            {},
            file.before,
            false,
            false,
        }}});
    }

    LspWorkspaceEditRecoveryRecord recovery;
    for (auto operation = applied.rbegin(); operation != applied.rend();
         ++operation) {
        recovery.operations.insert(recovery.operations.end(),
                                   operation->inverse.begin(),
                                   operation->inverse.end());
    }
    if (!set_recovery_document_revisions(*documents_, recovery.operations)) {
        const auto undone = rollback(*documents_, *files_, applied);
        if (!undone.accepted()) return undone;
        return failure(LspWorkspaceEditError::apply_failed,
                       "workspace edit recovery revisions are unavailable");
    }
    return {LspWorkspaceEditError::none, {}, recovery};
}

LspWorkspaceEditApplyResult LspWorkspaceEditApplier::recover(
    const LspWorkspaceEditRecoveryRecord& recovery) {
    return execute_recovery_operations(*documents_, *files_, recovery.operations);
}

LspWorkspaceEditController::LspWorkspaceEditController(
    LspSyncClient& client, LspWorkspaceEditApplier& applier)
    : client_(&client), applier_(&applier) {}

void LspWorkspaceEditController::supersede() {
    if (active_id_ == 0) return;
    const auto found = pending_.find(active_id_);
    if (found != pending_.end() &&
        found->second.disposition == Disposition::active) {
        found->second.disposition = Disposition::superseded;
        (void)client_->cancel(active_id_);
    }
    active_id_ = 0;
}

LspRenameRequestResult LspWorkspaceEditController::request_rename(
    std::string uri, Revision revision, ByteOffset position, std::string new_name) {
    if (new_name.empty()) {
        return {0, LspRenameError::invalid_argument,
                "LSP rename new name must not be empty"};
    }
    const auto snapshot = client_->document_snapshot(uri);
    if (!snapshot) {
        return {0, LspRenameError::unknown_document,
                "LSP rename request targets an unknown document"};
    }
    if (snapshot->revision != revision) {
        return {0, LspRenameError::stale_revision,
                "LSP rename request carries a stale revision"};
    }
    const auto lsp_position =
        byte_offset_to_lsp_position(snapshot->text, position);
    if (!lsp_position.accepted()) {
        return {0, LspRenameError::invalid_position,
                "LSP rename request position is invalid"};
    }

    supersede();
    const auto params =
        "{\"textDocument\":{\"uri\":" + json_escape(uri) +
        "},\"position\":{\"line\":" +
        std::to_string(lsp_position.position.line) + ",\"character\":" +
        std::to_string(lsp_position.position.character) +
        "},\"newName\":" + json_escape(new_name) + "}";
    const auto sent = client_->request("textDocument/rename", params);
    if (!sent.accepted()) {
        return {0, LspRenameError::sync_error, sent.message};
    }
    pending_.emplace(sent.id,
                     Pending{std::move(uri), revision, Disposition::active});
    active_id_ = sent.id;
    return {sent.id, LspRenameError::none, {}};
}

LspRenamePollResult LspWorkspaceEditController::poll(Revision current_revision) {
    const auto transport = client_->poll();
    if (!transport.accepted()) {
        return {transport.error, transport.message, {}};
    }

    LspRenamePollResult result;
    for (auto& response : client_->take_completed_responses()) {
        const auto found = pending_.find(response.id);
        if (found == pending_.end()) continue;
        const auto pending = found->second;
        pending_.erase(found);
        if (active_id_ == response.id) active_id_ = 0;

        LspRenamePublication publication;
        publication.request_id = response.id;
        publication.message = std::move(response.message);

        if (pending.disposition == Disposition::superseded) {
            publication.result = LspRenamePublishResult::superseded;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (pending.disposition == Disposition::cancelled ||
            response.status == LspCompletedResponseStatus::cancelled) {
            publication.result = LspRenamePublishResult::cancelled;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (response.status == LspCompletedResponseStatus::server_error) {
            publication.result = LspRenamePublishResult::server_error;
            result.publications.push_back(std::move(publication));
            continue;
        }

        const auto snapshot = client_->document_snapshot(pending.uri);
        if (current_revision != pending.revision || !snapshot ||
            snapshot->revision != pending.revision) {
            publication.result = LspRenamePublishResult::stale_revision;
            result.publications.push_back(std::move(publication));
            continue;
        }

        const auto root =
            JsonParser{response.payload_json, 64}.parse();
        const auto* payload = root && root->kind == Json::Kind::object
                                  ? root->member("result")
                                  : nullptr;
        if (!payload) {
            publication.result = LspRenamePublishResult::malformed_response;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (payload->kind == Json::Kind::null_value) {
            publication.result = LspRenamePublishResult::accepted;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (payload->kind != Json::Kind::object) {
            publication.result = LspRenamePublishResult::malformed_response;
            result.publications.push_back(std::move(publication));
            continue;
        }

        const auto applied = applier_->apply(json_stringify(*payload));
        if (!applied.accepted()) {
            publication.result = LspRenamePublishResult::edit_rejected;
            publication.message = applied.message;
            publication.recovery = applied.recovery;
        } else {
            publication.result = LspRenamePublishResult::accepted;
            publication.recovery = applied.recovery;
        }
        result.publications.push_back(std::move(publication));
    }
    return result;
}

} // namespace ssg
