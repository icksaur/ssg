#pragma once

#include <ssg/lsp_sync_client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct LspFeatureCommandDescriptor {
    std::string_view id;
    bool userNavigation = false;
    friend bool operator==(const LspFeatureCommandDescriptor&,
                           const LspFeatureCommandDescriptor&) = default;
};

class LspFeatureCommandSet {
public:
    [[nodiscard]] const std::array<LspFeatureCommandDescriptor, 9>&
    descriptors() const noexcept {
        return descriptors_;
    }

private:
    const std::array<LspFeatureCommandDescriptor, 9> descriptors_{{
        {"completion.open", false},
        {"completion.next", false},
        {"completion.previous", false},
        {"completion.accept", false},
        {"completion.dismiss", false},
        {"hover.show", false},
        {"hover.dismiss", false},
        {"goto.definition", true},
        {"goto.reference", true},
    }};
};

[[nodiscard]] LspFeatureCommandSet lspFeatureCommandSet();

struct LspCompletionItem {
    std::string label;
    std::string detail;
    std::string sortText;
    std::string insertText;
    std::optional<LspRange> replacementRange;
    friend bool operator==(const LspCompletionItem&,
                           const LspCompletionItem&) = default;
};

struct LspCompletionViewState {
    bool visible = false;
    bool loading = false;
    std::vector<LspCompletionItem> items;
    std::optional<std::size_t> selectedIndex;
    friend bool operator==(const LspCompletionViewState&,
                           const LspCompletionViewState&) = default;
};

struct LspHover {
    std::string contents;
    std::optional<LspRange> range;
    friend bool operator==(const LspHover&, const LspHover&) = default;
};

struct LspNavigationTarget {
    std::string uri;
    LspRange range;
    friend bool operator==(const LspNavigationTarget&,
                           const LspNavigationTarget&) = default;
};

struct LspNavigationViewState {
    std::vector<LspNavigationTarget> targets;
    std::optional<std::size_t> selectedIndex;
    bool userNavigation = false;
    bool revealPrimaryCaret = false;
    friend bool operator==(const LspNavigationViewState&,
                           const LspNavigationViewState&) = default;
};

struct LspFeatureViewState {
    Revision revision{0};
    LspCompletionViewState completion;
    std::optional<LspHover> hover;
    LspNavigationViewState navigation;
    std::string status;
    friend bool operator==(const LspFeatureViewState&,
                           const LspFeatureViewState&) = default;
};

enum class LspFeatureError : std::uint8_t {
    None,
    SyncError,
    UnknownDocument,
    StaleRevision,
    InvalidPosition,
};

struct LspFeatureRequestResult {
    std::uint64_t requestId = 0;
    LspFeatureError error = LspFeatureError::None;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return requestId != 0 && error == LspFeatureError::None;
    }
};

enum class LspFeaturePublishResult : std::uint8_t {
    Accepted,
    Cancelled,
    Superseded,
    StaleRevision,
    MalformedResponse,
    ServerError,
};

struct LspFeaturePublication {
    std::uint64_t requestId = 0;
    LspFeaturePublishResult result = LspFeaturePublishResult::Accepted;
    std::string message;
    friend bool operator==(const LspFeaturePublication&,
                           const LspFeaturePublication&) = default;
};

struct LspFeaturePollResult {
    LspSyncError error = LspSyncError::None;
    std::string message;
    std::vector<LspFeaturePublication> publications;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspSyncError::None;
    }
};

struct LspCompletionAcceptance {
    bool accepted = false;
    std::string text;
    std::optional<LspRange> range;
    std::string message;
};

struct LspFeatureConfig {
    std::size_t maximumCompletionItems = 1000;
    std::size_t maximumNavigationTargets = 1000;
    std::size_t maximumJsonDepth = 64;
};

class LspFeatureController {
public:
    explicit LspFeatureController(LspSyncClient& client,
                                  LspFeatureConfig config = {});

    [[nodiscard]] LspFeatureRequestResult requestCompletion(
        std::string uri, Revision revision, ByteOffset position);
    [[nodiscard]] LspFeatureRequestResult requestHover(
        std::string uri, Revision revision, ByteOffset position);
    [[nodiscard]] LspFeatureRequestResult requestDefinition(
        std::string uri, Revision revision, ByteOffset position);
    [[nodiscard]] LspFeatureRequestResult requestReferences(
        std::string uri, Revision revision, ByteOffset position);
    [[nodiscard]] LspFeaturePollResult poll(Revision currentRevision);

    void selectNextCompletion();
    void selectPreviousCompletion();
    [[nodiscard]] LspCompletionAcceptance acceptCompletion();
    void dismissCompletion();
    void dismissHover();

    [[nodiscard]] const LspFeatureViewState& viewState() const noexcept {
        return state_;
    }

private:
    enum class Kind : std::uint8_t {
        Completion,
        Hover,
        Definition,
        References,
    };
    enum class Disposition : std::uint8_t { Active, Cancelled, Superseded };
    struct Pending {
        Kind kind = Kind::Completion;
        std::string uri;
        Revision revision{0};
        std::uint64_t generation = 0;
        Disposition disposition = Disposition::Active;
    };

    [[nodiscard]] LspFeatureRequestResult request(
        Kind kind, std::string uri, Revision revision, ByteOffset position);
    void supersede(Kind kind);
    void cancel(Kind kind, Disposition disposition);
    void changed();

    LspSyncClient& client_;
    LspFeatureConfig config_;
    LspFeatureViewState state_;
    std::map<std::uint64_t, Pending> pending_;
    std::array<std::uint64_t, 4> activeIds_{};
    std::uint64_t generation_ = 0;
};

} // namespace ssg
