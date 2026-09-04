#include <ssg/LspWorkspaceEditController.h>

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

std::string jsonEscape(std::string_view value) {
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

void appendUtf8(std::string& target, std::uint32_t value) {
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
    enum class Kind { NullValue, Boolean, Number, String, Array, Object };
    Kind kind = Kind::NullValue;
    bool boolean = false;
    std::string scalar;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    [[nodiscard]] const Json* member(std::string_view name) const {
        const auto found = object.find(std::string{name});
        return found == object.end() ? nullptr : &found->second;
    }

    [[nodiscard]] std::optional<std::int64_t> integer() const {
        if (kind != Kind::Number) return std::nullopt;
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
    JsonParser(std::string_view input, std::size_t maximumDepth)
        : input_(input), maximumDepth_(maximumDepth) {}

    [[nodiscard]] std::optional<Json> parse() {
        auto value = parseValue(0);
        skipSpace();
        return value && offset_ == input_.size() ? value : std::nullopt;
    }

private:
    void skipSpace() {
        while (offset_ < input_.size() &&
               (input_[offset_] == ' ' || input_[offset_] == '\t' ||
                input_[offset_] == '\r' || input_[offset_] == '\n')) {
            ++offset_;
        }
    }

    bool consume(char expected) {
        skipSpace();
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

    [[nodiscard]] std::optional<std::string> parseString() {
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
                appendUtf8(result, value);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Json> parseValue(std::size_t depth) {
        skipSpace();
        if (depth > maximumDepth_ || offset_ >= input_.size()) {
            return std::nullopt;
        }
        if (input_[offset_] == '"') {
            auto text = parseString();
            if (!text) return std::nullopt;
            Json value;
            value.kind = Json::Kind::String;
            value.scalar = std::move(*text);
            return value;
        }
        if (input_[offset_] == '{') {
            ++offset_;
            Json value;
            value.kind = Json::Kind::Object;
            if (consume('}')) return value;
            while (true) {
                auto key = parseString();
                if (!key || !consume(':')) return std::nullopt;
                auto child = parseValue(depth + 1);
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
            value.kind = Json::Kind::Array;
            if (consume(']')) return value;
            while (true) {
                auto child = parseValue(depth + 1);
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
            value.kind = Json::Kind::Boolean;
            value.boolean = true;
            return value;
        }
        if (input_.substr(offset_, 5) == "false") {
            offset_ += 5;
            Json value;
            value.kind = Json::Kind::Boolean;
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
        value.kind = Json::Kind::Number;
        value.scalar = std::string{input_.substr(start, offset_ - start)};
        return value;
    }

    std::string_view input_;
    std::size_t maximumDepth_;
    std::size_t offset_ = 0;
};

std::string jsonStringify(const Json& value) {
    switch (value.kind) {
    case Json::Kind::NullValue: return "null";
    case Json::Kind::Boolean: return value.boolean ? "true" : "false";
    case Json::Kind::Number: return value.scalar;
    case Json::Kind::String: return jsonEscape(value.scalar);
    case Json::Kind::Array: {
        std::string out{"["};
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            if (index != 0) out.push_back(',');
            out += jsonStringify(value.array[index]);
        }
        out.push_back(']');
        return out;
    }
    case Json::Kind::Object: {
        std::string out{"{"};
        std::size_t index = 0;
        for (const auto& [key, child] : value.object) {
            if (index++ != 0) out.push_back(',');
            out += jsonEscape(key);
            out.push_back(':');
            out += jsonStringify(child);
        }
        out.push_back('}');
        return out;
    }
    }
    return {};
}

std::optional<LspPosition> parsePosition(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* line = value.member("line");
    const auto* character = value.member("character");
    if (!line || !character) return std::nullopt;
    const auto parsedLine = line->integer();
    const auto parsedCharacter = character->integer();
    if (!parsedLine || !parsedCharacter || *parsedLine < 0 ||
        *parsedCharacter < 0) {
        return std::nullopt;
    }
    return LspPosition{static_cast<std::uint64_t>(*parsedLine),
                       static_cast<std::uint64_t>(*parsedCharacter)};
}

std::optional<LspRange> parseRange(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* start = value.member("start");
    const auto* end = value.member("end");
    if (!start || !end) return std::nullopt;
    const auto parsedStart = parsePosition(*start);
    const auto parsedEnd = parsePosition(*end);
    if (!parsedStart || !parsedEnd ||
        parsedEnd->line < parsedStart->line ||
        (parsedEnd->line == parsedStart->line &&
         parsedEnd->character < parsedStart->character)) {
        return std::nullopt;
    }
    return LspRange{*parsedStart, *parsedEnd};
}

struct ParsedTextEdit {
    LspRange range;
    std::string newText;
};

std::optional<ParsedTextEdit> parseTextEdit(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* range = value.member("range");
    const auto* text = value.member("newText");
    if (!range || !text || text->kind != Json::Kind::String) {
        return std::nullopt;
    }
    const auto parsedRange = parseRange(*range);
    if (!parsedRange) return std::nullopt;
    return ParsedTextEdit{*parsedRange, text->scalar};
}

struct ParsedDocumentEdit {
    std::string uri;
    std::optional<std::int64_t> version;
    std::vector<ParsedTextEdit> edits;
};

enum class ParsedFileOperationKind : std::uint8_t { Create, Rename, Remove };

struct ParsedFileOperation {
    ParsedFileOperationKind kind = ParsedFileOperationKind::Create;
    std::string uri;
    std::string secondaryUri;
    bool overwrite = false;
    bool ignoreIfExists = false;
    bool recursive = false;
    bool ignoreIfNotExists = false;
};

using ParsedOperation = std::variant<ParsedDocumentEdit, ParsedFileOperation>;

std::optional<ParsedDocumentEdit> parseDocumentEditEntry(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* textDocument = value.member("textDocument");
    const auto* edits = value.member("edits");
    if (!textDocument || !edits || textDocument->kind != Json::Kind::Object ||
        edits->kind != Json::Kind::Array) {
        return std::nullopt;
    }
    const auto* uri = textDocument->member("uri");
    if (!uri || uri->kind != Json::Kind::String || uri->scalar.empty()) {
        return std::nullopt;
    }
    ParsedDocumentEdit parsed;
    parsed.uri = uri->scalar;
    if (const auto* version = textDocument->member("version")) {
        if (version->kind == Json::Kind::NullValue) {
            parsed.version = std::nullopt;
        } else {
            const auto parsedVersion = version->integer();
            if (!parsedVersion) return std::nullopt;
            parsed.version = *parsedVersion;
        }
    }
    parsed.edits.reserve(edits->array.size());
    for (const auto& edit : edits->array) {
        auto parsedEdit = parseTextEdit(edit);
        if (!parsedEdit) return std::nullopt;
        parsed.edits.push_back(std::move(*parsedEdit));
    }
    return parsed;
}

std::optional<ParsedFileOperation> parseFileOperationEntry(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* kind = value.member("kind");
    if (!kind || kind->kind != Json::Kind::String) return std::nullopt;
    ParsedFileOperation parsed;
    const auto* options = value.member("options");
    if (options && options->kind != Json::Kind::Object) return std::nullopt;

    const auto booleanOption =
        [&](std::string_view name, bool fallback,
            bool& target) -> bool {
        target = fallback;
        if (!options) return true;
        const auto* option = options->member(name);
        if (!option) return true;
        if (option->kind != Json::Kind::Boolean) return false;
        target = option->boolean;
        return true;
    };

    if (kind->scalar == "create") {
        parsed.kind = ParsedFileOperationKind::Create;
        const auto* uri = value.member("uri");
        if (!uri || uri->kind != Json::Kind::String || uri->scalar.empty()) {
            return std::nullopt;
        }
        parsed.uri = uri->scalar;
        if (!booleanOption("overwrite", false, parsed.overwrite) ||
            !booleanOption("ignoreIfExists", false, parsed.ignoreIfExists)) {
            return std::nullopt;
        }
        return parsed;
    }
    if (kind->scalar == "rename") {
        parsed.kind = ParsedFileOperationKind::Rename;
        const auto* oldUri = value.member("oldUri");
        const auto* newUri = value.member("newUri");
        if (!oldUri || !newUri || oldUri->kind != Json::Kind::String ||
            newUri->kind != Json::Kind::String || oldUri->scalar.empty() ||
            newUri->scalar.empty()) {
            return std::nullopt;
        }
        parsed.uri = oldUri->scalar;
        parsed.secondaryUri = newUri->scalar;
        if (!booleanOption("overwrite", false, parsed.overwrite) ||
            !booleanOption("ignoreIfExists", false, parsed.ignoreIfExists)) {
            return std::nullopt;
        }
        return parsed;
    }
    if (kind->scalar == "delete") {
        parsed.kind = ParsedFileOperationKind::Remove;
        const auto* uri = value.member("uri");
        if (!uri || uri->kind != Json::Kind::String || uri->scalar.empty()) {
            return std::nullopt;
        }
        parsed.uri = uri->scalar;
        if (!booleanOption("recursive", false, parsed.recursive) ||
            !booleanOption("ignoreIfNotExists", false,
                            parsed.ignoreIfNotExists)) {
            return std::nullopt;
        }
        return parsed;
    }
    return std::nullopt;
}

std::optional<std::vector<ParsedOperation>> parseWorkspaceEdit(
    const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    std::vector<ParsedOperation> operations;
    const auto* documentChanges = value.member("documentChanges");
    if (!documentChanges) {
        const auto* changes = value.member("changes");
        if (!changes) return operations;
        if (changes->kind != Json::Kind::Object) return std::nullopt;
        for (const auto& [uri, entries] : changes->object) {
            if (entries.kind != Json::Kind::Array) return std::nullopt;
            ParsedDocumentEdit parsed;
            parsed.uri = uri;
            parsed.edits.reserve(entries.array.size());
            for (const auto& entry : entries.array) {
                auto edit = parseTextEdit(entry);
                if (!edit) return std::nullopt;
                parsed.edits.push_back(std::move(*edit));
            }
            operations.emplace_back(std::move(parsed));
        }
    } else {
        if (documentChanges->kind != Json::Kind::Array) return std::nullopt;
        for (const auto& entry : documentChanges->array) {
            if (entry.kind != Json::Kind::Object) return std::nullopt;
            if (entry.member("textDocument")) {
                auto parsed = parseDocumentEditEntry(entry);
                if (!parsed) return std::nullopt;
                operations.emplace_back(std::move(*parsed));
            } else {
                auto parsed = parseFileOperationEntry(entry);
                if (!parsed) return std::nullopt;
                operations.emplace_back(std::move(*parsed));
            }
        }
    }
    return operations;
}

struct PlannedDocumentOperation {
    std::string uri;
    std::uint64_t expectedRevision{0};
    std::string oldText;
    std::string newText;
};

struct PlannedFileOperation {
    ParsedFileOperation source;
    LspWorkspaceFileNode before;
    LspWorkspaceFileNode beforeSecondary;
};

using PlannedOperation = std::variant<PlannedDocumentOperation, PlannedFileOperation>;

struct ResolvedEditRange {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string replacement;
};

std::optional<std::vector<ResolvedEditRange>> resolveRanges(
    std::string_view text, const std::vector<ParsedTextEdit>& edits,
    std::string& message) {
    std::vector<ResolvedEditRange> ranges;
    ranges.reserve(edits.size());
    for (const auto& edit : edits) {
        const auto start = lspPositionToByteOffset(text, edit.range.start);
        const auto end = lspPositionToByteOffset(text, edit.range.end);
        if (!start.accepted() || !end.accepted() || end.offset < start.offset) {
            message = "workspace edit range is outside the target document";
            return std::nullopt;
        }
        ranges.push_back({static_cast<std::size_t>(start.offset.value()),
                          static_cast<std::size_t>(end.offset.value()),
                          edit.newText});
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

std::optional<std::string> applyTextEdits(
    std::string_view text, const std::vector<ParsedTextEdit>& edits,
    LspWorkspaceEditError& error, std::string& message) {
    auto resolved = resolveRanges(text, edits, message);
    if (!resolved) {
        error = message.find("overlapping") == std::string::npos
                    ? LspWorkspaceEditError::InvalidPosition
                    : LspWorkspaceEditError::OverlappingEdits;
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

bool setRecoveryDocumentRevisions(
    LspWorkspaceEditDocuments& documents,
    std::vector<LspWorkspaceEditRecoveryOperation>& operations) {
    std::map<std::string, std::uint64_t> nextRevisions;
    for (auto& operation : operations) {
        if (operation.kind != LspWorkspaceEditRecoveryKind::DocumentText) {
            continue;
        }
        auto [found, inserted] =
            nextRevisions.try_emplace(operation.uri, std::uint64_t{0});
        if (inserted) {
            const auto snapshot = documents.snapshot(operation.uri);
            if (!snapshot) return false;
            found->second = snapshot->revision;
        }
        if (found->second == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        operation.expectedRevision = std::uint64_t{found->second++};
    }
    return true;
}

LspWorkspaceEditApplyResult executeRecoveryOperations(
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
        case LspWorkspaceEditRecoveryKind::DocumentText: {
            const auto restored = documents.apply(
                operation.uri, operation.expectedRevision, operation.text);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::RollbackFailed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::RestorePath: {
            const auto restored = files.restorePath(operation.uri, operation.node);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::RollbackFailed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::WriteFile: {
            const auto restored = files.writeFile(operation.uri, operation.text);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::RollbackFailed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::DeleteFile: {
            const auto restored = files.deletePath(operation.uri, true);
            if (!restored.accepted() &&
                restored.error != LspWorkspaceFileError::NotFound) {
                return {LspWorkspaceEditError::RollbackFailed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        case LspWorkspaceEditRecoveryKind::RenameFile: {
            const auto restored = files.renamePath(
                operation.uri, operation.secondaryUri, operation.overwrite);
            if (!restored.accepted()) {
                return {LspWorkspaceEditError::RollbackFailed,
                        "workspace edit rollback failed: " + restored.message,
                        remaining()};
            }
            break;
        }
        }
    }
    return {LspWorkspaceEditError::None, {}, std::nullopt};
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
    if (!setRecoveryDocumentRevisions(documents, operations)) {
        return {LspWorkspaceEditError::RollbackFailed,
                "workspace edit rollback revisions are unavailable",
                LspWorkspaceEditRecoveryRecord{std::move(operations)}};
    }
    return executeRecoveryOperations(documents, files, operations);
}

} // namespace

LspWorkspaceEditApplier::LspWorkspaceEditApplier(
    LspWorkspaceEditDocuments& documents, LspWorkspaceFileOperations& files,
    LspWorkspaceEditConfig config)
    : documents_(&documents), files_(&files), config_(config) {
    if (config_.maximumJsonDepth == 0) {
        throw std::invalid_argument("LSP workspace edit depth must be non-zero");
    }
}

LspWorkspaceEditApplyResult LspWorkspaceEditApplier::apply(
    std::string_view workspaceEditJson) {
    const auto parsedRoot =
        JsonParser{workspaceEditJson, config_.maximumJsonDepth}.parse();
    if (!parsedRoot) {
        return failure(LspWorkspaceEditError::MalformedEdit,
                       "workspace edit payload is not valid JSON");
    }
    const auto parsedOperations = parseWorkspaceEdit(*parsedRoot);
    if (!parsedOperations) {
        return failure(LspWorkspaceEditError::MalformedEdit,
                       "workspace edit payload is malformed");
    }

    struct SimulatedDocument {
        std::uint64_t revision{0};
        std::int64_t version = 0;
        std::string text;
    };

    std::map<std::string, SimulatedDocument> documents;
    std::map<std::string, LspWorkspaceFileNode> files;
    std::vector<PlannedOperation> plan;
    plan.reserve(parsedOperations->size());

    const auto loadDocument = [&](std::string_view uri,
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
            error = LspWorkspaceEditError::UnknownDocument;
            message = "workspace edit references an unknown document";
            return nullptr;
        }
        return &cache.emplace(
                         key,
                         SimulatedDocument{snapshot->revision, snapshot->version,
                                           snapshot->text})
                    .first->second;
    };

    const auto loadFile = [&](std::string_view uri,
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
            error = LspWorkspaceEditError::FileConflict;
            message = snapshot.message;
            return nullptr;
        }
        return &cache.emplace(key, std::move(node)).first->second;
    };

    for (const auto& operation : *parsedOperations) {
        LspWorkspaceEditError error = LspWorkspaceEditError::None;
        std::string message;
        if (std::holds_alternative<ParsedDocumentEdit>(operation)) {
            const auto& edit = std::get<ParsedDocumentEdit>(operation);
            auto* document =
                loadDocument(edit.uri, documents, error, message);
            if (!document) return failure(error, std::move(message));
            if (edit.version && *edit.version != document->version) {
                return failure(LspWorkspaceEditError::StaleRevision,
                               "workspace edit carries a stale document version");
            }
            auto updated = applyTextEdits(document->text, edit.edits, error,
                                            message);
            if (!updated) return failure(error, std::move(message));
            plan.emplace_back(PlannedDocumentOperation{
                edit.uri, document->revision, document->text, *updated});
            document->text = std::move(*updated);
            document->revision = std::uint64_t{document->revision + 1};
            ++document->version;
            continue;
        }

        const auto& file = std::get<ParsedFileOperation>(operation);
        auto* current = loadFile(file.uri, files, error, message);
        if (!current) return failure(error, std::move(message));

        if (file.kind == ParsedFileOperationKind::Create) {
            if (current->kind == LspWorkspaceFileNodeKind::Directory) {
                return failure(LspWorkspaceEditError::FileConflict,
                               "workspace edit cannot create over a directory");
            }
            if (current->kind == LspWorkspaceFileNodeKind::File &&
                file.ignoreIfExists) {
                continue;
            }
            if (current->kind == LspWorkspaceFileNodeKind::File &&
                !file.overwrite) {
                return failure(
                    LspWorkspaceEditError::FileConflict,
                    "workspace edit create target already exists");
            }
            plan.emplace_back(PlannedFileOperation{file, *current, {}});
            *current = {LspWorkspaceFileNodeKind::File, {}};
            continue;
        }

        if (file.kind == ParsedFileOperationKind::Rename) {
            auto* destination = loadFile(file.secondaryUri, files, error,
                                          message);
            if (!destination) return failure(error, std::move(message));
            if (current->kind == LspWorkspaceFileNodeKind::Missing) {
                return failure(
                    LspWorkspaceEditError::FileConflict,
                    "workspace edit rename source does not exist");
            }
            if (destination->kind != LspWorkspaceFileNodeKind::Missing &&
                file.ignoreIfExists) {
                continue;
            }
            if (destination->kind != LspWorkspaceFileNodeKind::Missing &&
                !file.overwrite) {
                return failure(
                    LspWorkspaceEditError::FileConflict,
                    "workspace edit rename destination already exists");
            }
            plan.emplace_back(PlannedFileOperation{file, *current, *destination});
            *destination = *current;
            *current = {LspWorkspaceFileNodeKind::Missing, {}};
            continue;
        }

        if (current->kind == LspWorkspaceFileNodeKind::Missing &&
            file.ignoreIfNotExists) {
            continue;
        }
        if (current->kind == LspWorkspaceFileNodeKind::Directory &&
            !file.recursive) {
            return failure(LspWorkspaceEditError::FileConflict,
                           "workspace edit directory delete requires recursive "
                           "option");
        }
        if (current->kind == LspWorkspaceFileNodeKind::Missing) {
            return failure(LspWorkspaceEditError::FileConflict,
                           "workspace edit delete target does not exist");
        }
        plan.emplace_back(PlannedFileOperation{file, *current, {}});
        *current = {LspWorkspaceFileNodeKind::Missing, {}};
    }

    std::vector<AppliedOperation> applied;
    applied.reserve(plan.size());

    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
        documentRevisionBudgets;
    for (const auto& operation : plan) {
        if (!std::holds_alternative<PlannedDocumentOperation>(operation)) {
            continue;
        }
        const auto& edit = std::get<PlannedDocumentOperation>(operation);
        auto [budget, inserted] = documentRevisionBudgets.try_emplace(
            edit.uri,
            std::pair{edit.expectedRevision, std::uint64_t{0}});
        ++budget->second.second;
    }
    for (const auto& [uri, budget] : documentRevisionBudgets) {
        (void)uri;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (budget.second > maximum / 2 ||
            budget.first > maximum - budget.second * 2) {
            return failure(
                LspWorkspaceEditError::ApplyFailed,
                "workspace edit lacks revision capacity for compensation");
        }
    }

    for (const auto& operation : plan) {
        if (std::holds_alternative<PlannedDocumentOperation>(operation)) {
            const auto& edit = std::get<PlannedDocumentOperation>(operation);
            const auto changed =
                documents_->apply(edit.uri, edit.expectedRevision, edit.newText);
            if (!changed.accepted()) {
                const auto undone = rollback(*documents_, *files_, applied);
                if (undone.accepted()) {
                    return failure(LspWorkspaceEditError::ApplyFailed,
                                   "workspace document write failed: " +
                                       changed.message);
                }
                return undone;
            }
            applied.push_back({{LspWorkspaceEditRecoveryOperation{
                LspWorkspaceEditRecoveryKind::DocumentText,
                edit.uri,
                {},
                changed.revision,
                edit.oldText,
                {},
                false,
                false,
            }}});
            continue;
        }

        const auto& file = std::get<PlannedFileOperation>(operation);
        if (file.source.kind == ParsedFileOperationKind::Create) {
            const auto created =
                files_->createFile(file.source.uri, file.source.overwrite);
            if (!created.accepted()) {
                const auto undone = rollback(*documents_, *files_, applied);
                if (undone.accepted()) {
                    return failure(LspWorkspaceEditError::ApplyFailed,
                                   "workspace file create failed: " +
                                       created.message);
                }
                return undone;
            }
            if (file.before.kind == LspWorkspaceFileNodeKind::Missing) {
                applied.push_back({{LspWorkspaceEditRecoveryOperation{
                    LspWorkspaceEditRecoveryKind::RestorePath,
                    file.source.uri,
                    {},
                    std::uint64_t{0},
                    {},
                    file.before,
                    true,
                    false,
                }}});
            } else {
                applied.push_back({{LspWorkspaceEditRecoveryOperation{
                    LspWorkspaceEditRecoveryKind::RestorePath,
                    file.source.uri,
                    {},
                    std::uint64_t{0},
                    {},
                    file.before,
                    false,
                    false,
                }}});
            }
            continue;
        }

        if (file.source.kind == ParsedFileOperationKind::Rename) {
            const auto renamed = files_->renamePath(file.source.uri,
                                                     file.source.secondaryUri,
                                                     file.source.overwrite);
            if (!renamed.accepted()) {
                const auto undone = rollback(*documents_, *files_, applied);
                if (undone.accepted()) {
                    return failure(LspWorkspaceEditError::ApplyFailed,
                                   "workspace file rename failed: " +
                                       renamed.message);
                }
                return undone;
            }
            std::vector<LspWorkspaceEditRecoveryOperation> inverse{ {
                LspWorkspaceEditRecoveryKind::RestorePath,
                file.source.uri,
                {},
                std::uint64_t{0},
                {},
                file.before,
                false,
                false,
            }};
            inverse.push_back({
                LspWorkspaceEditRecoveryKind::RestorePath,
                file.source.secondaryUri,
                {},
                std::uint64_t{0},
                {},
                file.beforeSecondary,
                false,
                false,
            });
            applied.push_back({std::move(inverse)});
            continue;
        }

        const auto removed =
            files_->deletePath(file.source.uri, file.source.recursive);
        if (!removed.accepted()) {
            const auto undone = rollback(*documents_, *files_, applied);
            if (undone.accepted()) {
                return failure(LspWorkspaceEditError::ApplyFailed,
                               "workspace file delete failed: " +
                                   removed.message);
            }
            return undone;
        }
        applied.push_back({{LspWorkspaceEditRecoveryOperation{
            LspWorkspaceEditRecoveryKind::RestorePath,
            file.source.uri,
            {},
            std::uint64_t{0},
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
    if (!setRecoveryDocumentRevisions(*documents_, recovery.operations)) {
        const auto undone = rollback(*documents_, *files_, applied);
        if (!undone.accepted()) return undone;
        return failure(LspWorkspaceEditError::ApplyFailed,
                       "workspace edit recovery revisions are unavailable");
    }
    return {LspWorkspaceEditError::None, {}, recovery};
}

LspWorkspaceEditApplyResult LspWorkspaceEditApplier::recover(
    const LspWorkspaceEditRecoveryRecord& recovery) {
    return executeRecoveryOperations(*documents_, *files_, recovery.operations);
}

LspWorkspaceEditController::LspWorkspaceEditController(
    LspSyncClient& client, LspWorkspaceEditApplier& applier)
    : client_(&client), applier_(&applier) {}

void LspWorkspaceEditController::supersede() {
    if (activeId_ == 0) return;
    const auto found = pending_.find(activeId_);
    if (found != pending_.end() &&
        found->second.disposition == Disposition::Active) {
        found->second.disposition = Disposition::Superseded;
        (void)client_->cancel(activeId_);
    }
    activeId_ = 0;
}

LspRenameRequestResult LspWorkspaceEditController::requestRename(
    std::string uri, std::uint64_t revision, ByteOffset position, std::string newName) {
    if (newName.empty()) {
        return {0, LspRenameError::InvalidArgument,
                "LSP rename new name must not be empty"};
    }
    const auto snapshot = client_->documentSnapshot(uri);
    if (!snapshot) {
        return {0, LspRenameError::UnknownDocument,
                "LSP rename request targets an unknown document"};
    }
    if (snapshot->revision != revision) {
        return {0, LspRenameError::StaleRevision,
                "LSP rename request carries a stale revision"};
    }
    const auto lspPosition =
        byteOffsetToLspPosition(snapshot->text, position);
    if (!lspPosition.accepted()) {
        return {0, LspRenameError::InvalidPosition,
                "LSP rename request position is invalid"};
    }

    supersede();
    const auto params =
        "{\"textDocument\":{\"uri\":" + jsonEscape(uri) +
        "},\"position\":{\"line\":" +
        std::to_string(lspPosition.position.line) + ",\"character\":" +
        std::to_string(lspPosition.position.character) +
        "},\"newName\":" + jsonEscape(newName) + "}";
    const auto sent = client_->request("textDocument/rename", params);
    if (!sent.accepted()) {
        return {0, LspRenameError::SyncError, sent.message};
    }
    pending_.emplace(sent.id,
                     Pending{std::move(uri), revision, Disposition::Active});
    activeId_ = sent.id;
    return {sent.id, LspRenameError::None, {}};
}

LspRenamePollResult LspWorkspaceEditController::poll(std::uint64_t currentRevision) {
    const auto transport = client_->poll();
    if (!transport.accepted()) {
        return {transport.error, transport.message, {}};
    }

    LspRenamePollResult result;
    for (auto& response : client_->takeCompletedResponses()) {
        const auto found = pending_.find(response.id);
        if (found == pending_.end()) continue;
        const auto pending = found->second;
        pending_.erase(found);
        if (activeId_ == response.id) activeId_ = 0;

        LspRenamePublication publication;
        publication.requestId = response.id;
        publication.message = std::move(response.message);

        if (pending.disposition == Disposition::Superseded) {
            publication.result = LspRenamePublishResult::Superseded;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (pending.disposition == Disposition::Cancelled ||
            response.status == LspCompletedResponseStatus::Cancelled) {
            publication.result = LspRenamePublishResult::Cancelled;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (response.status == LspCompletedResponseStatus::ServerError) {
            publication.result = LspRenamePublishResult::ServerError;
            result.publications.push_back(std::move(publication));
            continue;
        }

        const auto snapshot = client_->documentSnapshot(pending.uri);
        if (currentRevision != pending.revision || !snapshot ||
            snapshot->revision != pending.revision) {
            publication.result = LspRenamePublishResult::StaleRevision;
            result.publications.push_back(std::move(publication));
            continue;
        }

        const auto root =
            JsonParser{response.payloadJson, 64}.parse();
        const auto* payload = root && root->kind == Json::Kind::Object
                                  ? root->member("result")
                                  : nullptr;
        if (!payload) {
            publication.result = LspRenamePublishResult::MalformedResponse;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (payload->kind == Json::Kind::NullValue) {
            publication.result = LspRenamePublishResult::Accepted;
            result.publications.push_back(std::move(publication));
            continue;
        }
        if (payload->kind != Json::Kind::Object) {
            publication.result = LspRenamePublishResult::MalformedResponse;
            result.publications.push_back(std::move(publication));
            continue;
        }

        const auto applied = applier_->apply(jsonStringify(*payload));
        if (!applied.accepted()) {
            publication.result = LspRenamePublishResult::EditRejected;
            publication.message = applied.message;
            publication.recovery = applied.recovery;
        } else {
            publication.result = LspRenamePublishResult::Accepted;
            publication.recovery = applied.recovery;
        }
        result.publications.push_back(std::move(publication));
    }
    return result;
}

} // namespace ssg
