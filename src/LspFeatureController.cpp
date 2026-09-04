#include <ssg/LspFeatureController.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <stdexcept>
#include <utility>

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
    std::string scalar;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    [[nodiscard]] const Json* member(std::string_view name) const {
        const auto found = object.find(std::string{name});
        return found == object.end() ? nullptr : &found->second;
    }
    [[nodiscard]] std::optional<std::uint64_t> unsignedInteger() const {
        if (kind != Kind::Number) return std::nullopt;
        std::uint64_t value = 0;
        const auto parsed =
            std::from_chars(scalar.data(), scalar.data() + scalar.size(), value);
        return parsed.ec == std::errc{} &&
                       parsed.ptr == scalar.data() + scalar.size()
                   ? std::optional{value}
                   : std::nullopt;
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
    std::optional<std::uint32_t> hex4() {
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
    std::optional<std::string> parseString() {
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
                    if (offset_ + 2 > input_.size() ||
                        input_[offset_] != '\\' ||
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
    std::optional<Json> parseValue(std::size_t depth) {
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
        for (const auto word : {"null", "true", "false"}) {
            const std::string_view literal{word};
            if (input_.substr(offset_, literal.size()) == literal) {
                offset_ += literal.size();
                Json value;
                value.kind = literal == "null" ? Json::Kind::NullValue
                                               : Json::Kind::Boolean;
                return value;
            }
        }
        const auto start = offset_;
        if (input_[offset_] == '-') ++offset_;
        while (offset_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
            ++offset_;
        }
        if (start == offset_ ||
            (input_[start] == '-' && start + 1 == offset_)) {
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
        Json value;
        value.kind = Json::Kind::Number;
        value.scalar = std::string{input_.substr(start, offset_ - start)};
        return value;
    }

    std::string_view input_;
    std::size_t maximumDepth_;
    std::size_t offset_ = 0;
};

std::optional<LspPosition> parsePosition(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* line = value.member("line");
    const auto* character = value.member("character");
    if (!line || !character) return std::nullopt;
    const auto lineValue = line->unsignedInteger();
    const auto characterValue = character->unsignedInteger();
    if (!lineValue || !characterValue) return std::nullopt;
    return LspPosition{*lineValue, *characterValue};
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

const Json* responseResult(const Json& root) {
    return root.kind == Json::Kind::Object ? root.member("result") : nullptr;
}

std::optional<std::vector<LspCompletionItem>> parseCompletion(
    const Json& result, std::size_t maximumItems) {
    if (result.kind == Json::Kind::NullValue) {
        return std::vector<LspCompletionItem>{};
    }
    const Json* values = &result;
    if (result.kind == Json::Kind::Object) values = result.member("items");
    if (!values || values->kind != Json::Kind::Array ||
        values->array.size() > maximumItems) {
        return std::nullopt;
    }
    std::vector<LspCompletionItem> items;
    items.reserve(values->array.size());
    for (const auto& value : values->array) {
        if (value.kind != Json::Kind::Object) return std::nullopt;
        const auto* label = value.member("label");
        if (!label || label->kind != Json::Kind::String) return std::nullopt;
        LspCompletionItem item;
        item.label = label->scalar;
        item.sortText = item.label;
        item.insertText = item.label;
        if (const auto* detail = value.member("detail")) {
            if (detail->kind != Json::Kind::String) return std::nullopt;
            item.detail = detail->scalar;
        }
        if (const auto* sort = value.member("sortText")) {
            if (sort->kind != Json::Kind::String) return std::nullopt;
            item.sortText = sort->scalar;
        }
        if (const auto* insert = value.member("insertText")) {
            if (insert->kind != Json::Kind::String) return std::nullopt;
            item.insertText = insert->scalar;
        }
        if (const auto* edit = value.member("textEdit")) {
            if (edit->kind != Json::Kind::Object) return std::nullopt;
            const auto* range = edit->member("range");
            const auto* text = edit->member("newText");
            if (!range || !text || text->kind != Json::Kind::String) {
                return std::nullopt;
            }
            item.replacementRange = parseRange(*range);
            if (!item.replacementRange) return std::nullopt;
            item.insertText = text->scalar;
        }
        items.push_back(std::move(item));
    }
    std::stable_sort(items.begin(), items.end(), [](const auto& left,
                                                    const auto& right) {
        return left.sortText < right.sortText;
    });
    return items;
}

std::optional<LspHover> parseHover(const Json& result) {
    if (result.kind == Json::Kind::NullValue) return LspHover{};
    if (result.kind != Json::Kind::Object) return std::nullopt;
    const auto* contents = result.member("contents");
    if (!contents) return std::nullopt;
    LspHover hover;
    if (contents->kind == Json::Kind::String) {
        hover.contents = contents->scalar;
    } else if (contents->kind == Json::Kind::Object) {
        const auto* value = contents->member("value");
        if (!value || value->kind != Json::Kind::String) return std::nullopt;
        hover.contents = value->scalar;
    } else {
        return std::nullopt;
    }
    if (const auto* range = result.member("range")) {
        hover.range = parseRange(*range);
        if (!hover.range) return std::nullopt;
    }
    return hover;
}

std::optional<LspNavigationTarget> parseLocation(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* uri = value.member("uri");
    const auto* range = value.member("range");
    if (!uri) uri = value.member("targetUri");
    if (!range) range = value.member("targetSelectionRange");
    if (!uri || uri->kind != Json::Kind::String || !range) {
        return std::nullopt;
    }
    auto parsedRange = parseRange(*range);
    if (!parsedRange) return std::nullopt;
    return LspNavigationTarget{uri->scalar, *parsedRange};
}

std::optional<std::vector<LspNavigationTarget>> parseLocations(
    const Json& result, std::size_t maximumTargets) {
    if (result.kind == Json::Kind::NullValue) {
        return std::vector<LspNavigationTarget>{};
    }
    std::vector<LspNavigationTarget> targets;
    if (result.kind == Json::Kind::Array) {
        if (result.array.size() > maximumTargets) return std::nullopt;
        targets.reserve(result.array.size());
        for (const auto& value : result.array) {
            auto target = parseLocation(value);
            if (!target) return std::nullopt;
            targets.push_back(std::move(*target));
        }
    } else {
        auto target = parseLocation(result);
        if (!target || maximumTargets == 0) return std::nullopt;
        targets.push_back(std::move(*target));
    }
    return targets;
}

} // namespace

LspFeatureController::LspFeatureController(LspSyncClient& client,
                                           LspFeatureConfig config)
    : client_(client), config_(config) {
    if (config.maximumCompletionItems == 0 ||
        config.maximumNavigationTargets == 0 ||
        config.maximumJsonDepth == 0) {
        throw std::invalid_argument("LSP feature limits must be non-zero");
    }
}

LspFeatureRequestResult LspFeatureController::requestCompletion(
    std::string uri, std::uint64_t revision, ByteOffset position) {
    return request(Kind::Completion, std::move(uri), revision, position);
}
LspFeatureRequestResult LspFeatureController::requestHover(
    std::string uri, std::uint64_t revision, ByteOffset position) {
    return request(Kind::Hover, std::move(uri), revision, position);
}
LspFeatureRequestResult LspFeatureController::requestDefinition(
    std::string uri, std::uint64_t revision, ByteOffset position) {
    return request(Kind::Definition, std::move(uri), revision, position);
}
LspFeatureRequestResult LspFeatureController::requestReferences(
    std::string uri, std::uint64_t revision, ByteOffset position) {
    return request(Kind::References, std::move(uri), revision, position);
}

LspFeatureRequestResult LspFeatureController::request(
    Kind kind, std::string uri, std::uint64_t revision, ByteOffset position) {
    const auto snapshot = client_.documentSnapshot(uri);
    if (!snapshot) {
        return {0, LspFeatureError::UnknownDocument,
                "LSP feature request targets an unknown document"};
    }
    if (snapshot->revision != revision) {
        return {0, LspFeatureError::StaleRevision,
                "LSP feature request carries a stale revision"};
    }
    const auto lspPosition =
        byteOffsetToLspPosition(snapshot->text, position);
    if (!lspPosition.accepted()) {
        return {0, LspFeatureError::InvalidPosition,
                "LSP feature request position is invalid"};
    }
    supersede(kind);
    std::string method;
    switch (kind) {
    case Kind::Completion: method = "textDocument/completion"; break;
    case Kind::Hover: method = "textDocument/hover"; break;
    case Kind::Definition: method = "textDocument/definition"; break;
    case Kind::References: method = "textDocument/references"; break;
    }
    auto params =
        "{\"textDocument\":{\"uri\":" + jsonEscape(uri) +
        "},\"position\":{\"line\":" +
        std::to_string(lspPosition.position.line) + ",\"character\":" +
        std::to_string(lspPosition.position.character) + "}";
    if (kind == Kind::References) {
        params += ",\"context\":{\"includeDeclaration\":true}";
    }
    params += "}";
    const auto sent = client_.request(std::move(method), std::move(params));
    if (!sent.accepted()) {
        return {0, LspFeatureError::SyncError, sent.message};
    }
    const auto generation = ++generation_;
    pending_.emplace(sent.id,
                     Pending{kind, std::move(uri), revision, generation,
                             Disposition::Active});
    activeIds_[static_cast<std::size_t>(kind)] = sent.id;
    if (kind == Kind::Completion) {
        state_.completion.visible = true;
        state_.completion.loading = true;
    }
    state_.status = "loading";
    changed();
    return {sent.id, LspFeatureError::None, {}};
}

void LspFeatureController::supersede(Kind kind) {
    cancel(kind, Disposition::Superseded);
}

void LspFeatureController::cancel(Kind kind, Disposition disposition) {
    auto& id = activeIds_[static_cast<std::size_t>(kind)];
    if (id == 0) return;
    const auto found = pending_.find(id);
    if (found != pending_.end() &&
        found->second.disposition == Disposition::Active) {
        found->second.disposition = disposition;
        (void)client_.cancel(id);
    }
    id = 0;
}

LspFeaturePollResult LspFeatureController::poll(std::uint64_t currentRevision) {
    const auto transport = client_.poll();
    if (!transport.accepted()) {
        return {transport.error, transport.message, {}};
    }
    LspFeaturePollResult result;
    for (auto& response : client_.takeCompletedResponses()) {
        const auto found = pending_.find(response.id);
        if (found == pending_.end()) continue;
        const auto pending = found->second;
        pending_.erase(found);
        auto& active =
            activeIds_[static_cast<std::size_t>(pending.kind)];
        if (active == response.id) active = 0;

        LspFeaturePublishResult outcome;
        if (pending.disposition == Disposition::Superseded) {
            outcome = LspFeaturePublishResult::Superseded;
        } else if (pending.disposition == Disposition::Cancelled ||
                   response.status == LspCompletedResponseStatus::Cancelled) {
            outcome = LspFeaturePublishResult::Cancelled;
        } else if (response.status ==
                   LspCompletedResponseStatus::ServerError) {
            outcome = LspFeaturePublishResult::ServerError;
        } else {
            const auto snapshot = client_.documentSnapshot(pending.uri);
            if (currentRevision != pending.revision || !snapshot ||
                snapshot->revision != pending.revision) {
                outcome = LspFeaturePublishResult::StaleRevision;
            } else {
                const auto root =
                    JsonParser{response.payloadJson, config_.maximumJsonDepth}
                        .parse();
                const auto* payload = root ? responseResult(*root) : nullptr;
                bool accepted = false;
                if (payload && pending.kind == Kind::Completion) {
                    auto items =
                        parseCompletion(*payload,
                                         config_.maximumCompletionItems);
                    if (items) {
                        state_.completion.items = std::move(*items);
                        state_.completion.loading = false;
                        state_.completion.visible =
                            !state_.completion.items.empty();
                        state_.completion.selectedIndex =
                            state_.completion.items.empty()
                                ? std::nullopt
                                : std::optional<std::size_t>{0};
                        accepted = true;
                    }
                } else if (payload && pending.kind == Kind::Hover) {
                    auto hover = parseHover(*payload);
                    if (hover) {
                        state_.hover =
                            hover->contents.empty()
                                ? std::nullopt
                                : std::optional<LspHover>{std::move(*hover)};
                        accepted = true;
                    }
                } else if (payload) {
                    auto locations = parseLocations(
                        *payload, config_.maximumNavigationTargets);
                    if (locations) {
                        state_.navigation.targets = std::move(*locations);
                        state_.navigation.selectedIndex =
                            state_.navigation.targets.empty()
                                ? std::nullopt
                                : std::optional<std::size_t>{0};
                        state_.navigation.userNavigation = true;
                        state_.navigation.revealPrimaryCaret =
                            !state_.navigation.targets.empty();
                        accepted = true;
                    }
                }
                outcome = accepted
                              ? LspFeaturePublishResult::Accepted
                              : LspFeaturePublishResult::MalformedResponse;
                if (accepted) {
                    state_.status = "ready";
                    changed();
                }
            }
        }
        if (pending.disposition == Disposition::Active &&
            outcome != LspFeaturePublishResult::Accepted) {
            if (pending.kind == Kind::Completion) {
                state_.completion.loading = false;
            }
            switch (outcome) {
            case LspFeaturePublishResult::StaleRevision:
                state_.status = "stale LSP response rejected";
                break;
            case LspFeaturePublishResult::MalformedResponse:
                state_.status = "malformed LSP response rejected";
                break;
            case LspFeaturePublishResult::ServerError:
                state_.status = "LSP server request failed";
                break;
            default:
                break;
            }
            changed();
        }
        result.publications.push_back(
            {response.id, outcome, std::move(response.message)});
    }
    return result;
}

void LspFeatureController::selectNextCompletion() {
    auto& completion = state_.completion;
    if (!completion.selectedIndex || completion.items.empty()) return;
    completion.selectedIndex =
        (*completion.selectedIndex + 1) % completion.items.size();
    changed();
}

void LspFeatureController::selectPreviousCompletion() {
    auto& completion = state_.completion;
    if (!completion.selectedIndex || completion.items.empty()) return;
    completion.selectedIndex =
        (*completion.selectedIndex + completion.items.size() - 1) %
        completion.items.size();
    changed();
}

LspCompletionAcceptance LspFeatureController::acceptCompletion() {
    const auto selected = state_.completion.selectedIndex;
    if (!selected || *selected >= state_.completion.items.size()) {
        return {false, {}, std::nullopt, "no completion is selected"};
    }
    const auto item = state_.completion.items[*selected];
    state_.completion = {};
    state_.status = "completion accepted";
    changed();
    return {true, item.insertText, item.replacementRange, {}};
}

void LspFeatureController::dismissCompletion() {
    cancel(Kind::Completion, Disposition::Cancelled);
    state_.completion = {};
    state_.status = "completion dismissed";
    changed();
}

void LspFeatureController::dismissHover() {
    cancel(Kind::Hover, Disposition::Cancelled);
    state_.hover.reset();
    state_.status = "hover dismissed";
    changed();
}

void LspFeatureController::changed() {
    state_.revision = std::uint64_t{state_.revision + 1};
}

} // namespace ssg
