#include <ssg/lsp_features.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <stdexcept>
#include <utility>

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
    std::string scalar;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    [[nodiscard]] const Json* member(std::string_view name) const {
        const auto found = object.find(std::string{name});
        return found == object.end() ? nullptr : &found->second;
    }
    [[nodiscard]] std::optional<std::uint64_t> unsigned_integer() const {
        if (kind != Kind::number) return std::nullopt;
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
    std::optional<std::string> parse_string() {
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
                append_utf8(result, value);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }
    std::optional<Json> parse_value(std::size_t depth) {
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
        for (const auto word : {"null", "true", "false"}) {
            const std::string_view literal{word};
            if (input_.substr(offset_, literal.size()) == literal) {
                offset_ += literal.size();
                Json value;
                value.kind = literal == "null" ? Json::Kind::null_value
                                               : Json::Kind::boolean;
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
        value.kind = Json::Kind::number;
        value.scalar = std::string{input_.substr(start, offset_ - start)};
        return value;
    }

    std::string_view input_;
    std::size_t maximum_depth_;
    std::size_t offset_ = 0;
};

std::optional<LspPosition> parse_position(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* line = value.member("line");
    const auto* character = value.member("character");
    if (!line || !character) return std::nullopt;
    const auto line_value = line->unsigned_integer();
    const auto character_value = character->unsigned_integer();
    if (!line_value || !character_value) return std::nullopt;
    return LspPosition{*line_value, *character_value};
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

const Json* response_result(const Json& root) {
    return root.kind == Json::Kind::object ? root.member("result") : nullptr;
}

std::optional<std::vector<LspCompletionItem>> parse_completion(
    const Json& result, std::size_t maximum_items) {
    if (result.kind == Json::Kind::null_value) {
        return std::vector<LspCompletionItem>{};
    }
    const Json* values = &result;
    if (result.kind == Json::Kind::object) values = result.member("items");
    if (!values || values->kind != Json::Kind::array ||
        values->array.size() > maximum_items) {
        return std::nullopt;
    }
    std::vector<LspCompletionItem> items;
    items.reserve(values->array.size());
    for (const auto& value : values->array) {
        if (value.kind != Json::Kind::object) return std::nullopt;
        const auto* label = value.member("label");
        if (!label || label->kind != Json::Kind::string) return std::nullopt;
        LspCompletionItem item;
        item.label = label->scalar;
        item.sort_text = item.label;
        item.insert_text = item.label;
        if (const auto* detail = value.member("detail")) {
            if (detail->kind != Json::Kind::string) return std::nullopt;
            item.detail = detail->scalar;
        }
        if (const auto* sort = value.member("sortText")) {
            if (sort->kind != Json::Kind::string) return std::nullopt;
            item.sort_text = sort->scalar;
        }
        if (const auto* insert = value.member("insertText")) {
            if (insert->kind != Json::Kind::string) return std::nullopt;
            item.insert_text = insert->scalar;
        }
        if (const auto* edit = value.member("textEdit")) {
            if (edit->kind != Json::Kind::object) return std::nullopt;
            const auto* range = edit->member("range");
            const auto* text = edit->member("newText");
            if (!range || !text || text->kind != Json::Kind::string) {
                return std::nullopt;
            }
            item.replacement_range = parse_range(*range);
            if (!item.replacement_range) return std::nullopt;
            item.insert_text = text->scalar;
        }
        items.push_back(std::move(item));
    }
    std::stable_sort(items.begin(), items.end(), [](const auto& left,
                                                    const auto& right) {
        return left.sort_text < right.sort_text;
    });
    return items;
}

std::optional<LspHover> parse_hover(const Json& result) {
    if (result.kind == Json::Kind::null_value) return LspHover{};
    if (result.kind != Json::Kind::object) return std::nullopt;
    const auto* contents = result.member("contents");
    if (!contents) return std::nullopt;
    LspHover hover;
    if (contents->kind == Json::Kind::string) {
        hover.contents = contents->scalar;
    } else if (contents->kind == Json::Kind::object) {
        const auto* value = contents->member("value");
        if (!value || value->kind != Json::Kind::string) return std::nullopt;
        hover.contents = value->scalar;
    } else {
        return std::nullopt;
    }
    if (const auto* range = result.member("range")) {
        hover.range = parse_range(*range);
        if (!hover.range) return std::nullopt;
    }
    return hover;
}

std::optional<LspNavigationTarget> parse_location(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* uri = value.member("uri");
    const auto* range = value.member("range");
    if (!uri) uri = value.member("targetUri");
    if (!range) range = value.member("targetSelectionRange");
    if (!uri || uri->kind != Json::Kind::string || !range) {
        return std::nullopt;
    }
    auto parsed_range = parse_range(*range);
    if (!parsed_range) return std::nullopt;
    return LspNavigationTarget{uri->scalar, *parsed_range};
}

std::optional<std::vector<LspNavigationTarget>> parse_locations(
    const Json& result, std::size_t maximum_targets) {
    if (result.kind == Json::Kind::null_value) {
        return std::vector<LspNavigationTarget>{};
    }
    std::vector<LspNavigationTarget> targets;
    if (result.kind == Json::Kind::array) {
        if (result.array.size() > maximum_targets) return std::nullopt;
        targets.reserve(result.array.size());
        for (const auto& value : result.array) {
            auto target = parse_location(value);
            if (!target) return std::nullopt;
            targets.push_back(std::move(*target));
        }
    } else {
        auto target = parse_location(result);
        if (!target || maximum_targets == 0) return std::nullopt;
        targets.push_back(std::move(*target));
    }
    return targets;
}

} // namespace

LspFeatureCommandSet lsp_feature_command_set() { return {}; }

LspFeatureDelta derive_lsp_feature_delta(const LspFeatureViewState& base,
                                         const LspFeatureViewState& target) {
    LspFeatureDelta delta{base.revision, target.revision, std::nullopt};
    if (base != target) delta.state = target;
    return delta;
}

LspFeatureReplayResult replay_lsp_feature_delta(
    const LspFeatureViewState& base, const LspFeatureDelta& delta) {
    if (delta.base_revision != base.revision) {
        return {std::nullopt, LspFeatureReplayError::stale_revision};
    }
    if (delta.revision < delta.base_revision ||
        (delta.state && delta.state->revision != delta.revision) ||
        (!delta.state && delta.revision != delta.base_revision)) {
        return {std::nullopt, LspFeatureReplayError::malformed_delta};
    }
    return {delta.state ? delta.state
                        : std::optional<LspFeatureViewState>{base},
            LspFeatureReplayError::none};
}

LspFeatureController::LspFeatureController(LspSyncClient& client,
                                           LspFeatureConfig config)
    : client_(client), config_(config) {
    if (config.maximum_completion_items == 0 ||
        config.maximum_navigation_targets == 0 ||
        config.maximum_json_depth == 0) {
        throw std::invalid_argument("LSP feature limits must be non-zero");
    }
}

LspFeatureRequestResult LspFeatureController::request_completion(
    std::string uri, Revision revision, ByteOffset position) {
    return request(Kind::completion, std::move(uri), revision, position);
}
LspFeatureRequestResult LspFeatureController::request_hover(
    std::string uri, Revision revision, ByteOffset position) {
    return request(Kind::hover, std::move(uri), revision, position);
}
LspFeatureRequestResult LspFeatureController::request_definition(
    std::string uri, Revision revision, ByteOffset position) {
    return request(Kind::definition, std::move(uri), revision, position);
}
LspFeatureRequestResult LspFeatureController::request_references(
    std::string uri, Revision revision, ByteOffset position) {
    return request(Kind::references, std::move(uri), revision, position);
}

LspFeatureRequestResult LspFeatureController::request(
    Kind kind, std::string uri, Revision revision, ByteOffset position) {
    const auto snapshot = client_.document_snapshot(uri);
    if (!snapshot) {
        return {0, LspFeatureError::unknown_document,
                "LSP feature request targets an unknown document"};
    }
    if (snapshot->revision != revision) {
        return {0, LspFeatureError::stale_revision,
                "LSP feature request carries a stale revision"};
    }
    const auto lsp_position =
        byte_offset_to_lsp_position(snapshot->text, position);
    if (!lsp_position.accepted()) {
        return {0, LspFeatureError::invalid_position,
                "LSP feature request position is invalid"};
    }
    supersede(kind);
    std::string method;
    switch (kind) {
    case Kind::completion: method = "textDocument/completion"; break;
    case Kind::hover: method = "textDocument/hover"; break;
    case Kind::definition: method = "textDocument/definition"; break;
    case Kind::references: method = "textDocument/references"; break;
    }
    auto params =
        "{\"textDocument\":{\"uri\":" + json_escape(uri) +
        "},\"position\":{\"line\":" +
        std::to_string(lsp_position.position.line) + ",\"character\":" +
        std::to_string(lsp_position.position.character) + "}";
    if (kind == Kind::references) {
        params += ",\"context\":{\"includeDeclaration\":true}";
    }
    params += "}";
    const auto sent = client_.request(std::move(method), std::move(params));
    if (!sent.accepted()) {
        return {0, LspFeatureError::sync_error, sent.message};
    }
    const auto generation = ++generation_;
    pending_.emplace(sent.id,
                     Pending{kind, std::move(uri), revision, generation,
                             Disposition::active});
    active_ids_[static_cast<std::size_t>(kind)] = sent.id;
    if (kind == Kind::completion) {
        state_.completion.visible = true;
        state_.completion.loading = true;
    }
    state_.status = "loading";
    changed();
    return {sent.id, LspFeatureError::none, {}};
}

void LspFeatureController::supersede(Kind kind) {
    cancel(kind, Disposition::superseded);
}

void LspFeatureController::cancel(Kind kind, Disposition disposition) {
    auto& id = active_ids_[static_cast<std::size_t>(kind)];
    if (id == 0) return;
    const auto found = pending_.find(id);
    if (found != pending_.end() &&
        found->second.disposition == Disposition::active) {
        found->second.disposition = disposition;
        (void)client_.cancel(id);
    }
    id = 0;
}

LspFeaturePollResult LspFeatureController::poll(Revision current_revision) {
    const auto transport = client_.poll();
    if (!transport.accepted()) {
        return {transport.error, transport.message, {}};
    }
    LspFeaturePollResult result;
    for (auto& response : client_.take_completed_responses()) {
        const auto found = pending_.find(response.id);
        if (found == pending_.end()) continue;
        const auto pending = found->second;
        pending_.erase(found);
        auto& active =
            active_ids_[static_cast<std::size_t>(pending.kind)];
        if (active == response.id) active = 0;

        LspFeaturePublishResult outcome;
        if (pending.disposition == Disposition::superseded) {
            outcome = LspFeaturePublishResult::superseded;
        } else if (pending.disposition == Disposition::cancelled ||
                   response.status == LspCompletedResponseStatus::cancelled) {
            outcome = LspFeaturePublishResult::cancelled;
        } else if (response.status ==
                   LspCompletedResponseStatus::server_error) {
            outcome = LspFeaturePublishResult::server_error;
        } else {
            const auto snapshot = client_.document_snapshot(pending.uri);
            if (current_revision != pending.revision || !snapshot ||
                snapshot->revision != pending.revision) {
                outcome = LspFeaturePublishResult::stale_revision;
            } else {
                const auto root =
                    JsonParser{response.payload_json, config_.maximum_json_depth}
                        .parse();
                const auto* payload = root ? response_result(*root) : nullptr;
                bool accepted = false;
                if (payload && pending.kind == Kind::completion) {
                    auto items =
                        parse_completion(*payload,
                                         config_.maximum_completion_items);
                    if (items) {
                        state_.completion.items = std::move(*items);
                        state_.completion.loading = false;
                        state_.completion.visible =
                            !state_.completion.items.empty();
                        state_.completion.selected_index =
                            state_.completion.items.empty()
                                ? std::nullopt
                                : std::optional<std::size_t>{0};
                        accepted = true;
                    }
                } else if (payload && pending.kind == Kind::hover) {
                    auto hover = parse_hover(*payload);
                    if (hover) {
                        state_.hover =
                            hover->contents.empty()
                                ? std::nullopt
                                : std::optional<LspHover>{std::move(*hover)};
                        accepted = true;
                    }
                } else if (payload) {
                    auto locations = parse_locations(
                        *payload, config_.maximum_navigation_targets);
                    if (locations) {
                        state_.navigation.targets = std::move(*locations);
                        state_.navigation.selected_index =
                            state_.navigation.targets.empty()
                                ? std::nullopt
                                : std::optional<std::size_t>{0};
                        state_.navigation.user_navigation = true;
                        state_.navigation.reveal_primary_caret =
                            !state_.navigation.targets.empty();
                        accepted = true;
                    }
                }
                outcome = accepted
                              ? LspFeaturePublishResult::accepted
                              : LspFeaturePublishResult::malformed_response;
                if (accepted) {
                    state_.status = "ready";
                    changed();
                }
            }
        }
        if (pending.disposition == Disposition::active &&
            outcome != LspFeaturePublishResult::accepted) {
            if (pending.kind == Kind::completion) {
                state_.completion.loading = false;
            }
            switch (outcome) {
            case LspFeaturePublishResult::stale_revision:
                state_.status = "stale LSP response rejected";
                break;
            case LspFeaturePublishResult::malformed_response:
                state_.status = "malformed LSP response rejected";
                break;
            case LspFeaturePublishResult::server_error:
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

void LspFeatureController::select_next_completion() {
    auto& completion = state_.completion;
    if (!completion.selected_index || completion.items.empty()) return;
    completion.selected_index =
        (*completion.selected_index + 1) % completion.items.size();
    changed();
}

void LspFeatureController::select_previous_completion() {
    auto& completion = state_.completion;
    if (!completion.selected_index || completion.items.empty()) return;
    completion.selected_index =
        (*completion.selected_index + completion.items.size() - 1) %
        completion.items.size();
    changed();
}

LspCompletionAcceptance LspFeatureController::accept_completion() {
    const auto selected = state_.completion.selected_index;
    if (!selected || *selected >= state_.completion.items.size()) {
        return {false, {}, std::nullopt, "no completion is selected"};
    }
    const auto item = state_.completion.items[*selected];
    state_.completion = {};
    state_.status = "completion accepted";
    changed();
    return {true, item.insert_text, item.replacement_range, {}};
}

void LspFeatureController::dismiss_completion() {
    cancel(Kind::completion, Disposition::cancelled);
    state_.completion = {};
    state_.status = "completion dismissed";
    changed();
}

void LspFeatureController::dismiss_hover() {
    cancel(Kind::hover, Disposition::cancelled);
    state_.hover.reset();
    state_.status = "hover dismissed";
    changed();
}

void LspFeatureController::changed() {
    state_.revision = Revision{state_.revision.value() + 1};
}

} // namespace ssg
