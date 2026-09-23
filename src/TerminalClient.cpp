#include <ssg/TerminalClient.h>

#include <ssg/Editor.h>
#include <ssg/GridPresenter.h>
#include <ssg/HitTester.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Picker.h>
#include <ssg/PromptEditState.h>
#include <ssg/Selection.h>
#include <ssg/SystemClipboardReader.h>
#include <ssg/Terminal.h>
#include <ssg/TerminalCapabilities.h>
#include <ssg/TerminalInput.h>
#include <ssg/TerminalOutput.h>
#include <ssg/focus.h>
#include <ssg/pointer_routing.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>

namespace ssg {

namespace {

struct GutterDrag {
    HitRegion region;
    int gutterY;
    int travel;
    int grabOffset;
};

struct PointerState {
    bool dragging = false;
    std::optional<DocumentPosition> dragAnchor;
    bool altDrag = false;
    std::optional<GutterDrag> gutterDrag;
    ClickTracker clickTracker;
    int lastColumn = 0;
    int lastRow = 0;
};

const char* environmentVariable(std::string_view name) {
    return std::getenv(std::string{name}.c_str());
}

} // namespace

struct TerminalClient::Impl {
    class PaletteView {
      public:
        explicit PaletteView(Impl& client) : client_{client} {}

        void scroll(std::int64_t delta) {
            if (!open_) return;
            ScrollOffset offset{window_.firstVisible};
            offset.byLines(delta, candidateCount(), window_.paneRows);
            window_.firstVisible = offset.firstVisible();
        }

        void scrollToFraction(std::uint32_t numerator,
                              std::uint32_t denominator) {
            if (!open_) return;
            ScrollOffset offset{window_.firstVisible};
            offset.toFraction(numerator, denominator, candidateCount(),
                              window_.paneRows);
            window_.firstVisible = offset.firstVisible();
        }

        void apply(const ClientOwnedInput& input) {
            switch (input.kind) {
            case ClientOwnedInputKind::TextEdit:
                window_.query = applyPromptTextEdit(window_.query, input.edit);
                if (input.edit.kind == PromptTextEdit::Kind::Insert ||
                    input.edit.kind ==
                        PromptTextEdit::Kind::DeleteBackward ||
                    input.edit.kind == PromptTextEdit::Kind::DeleteForward ||
                    input.edit.kind ==
                        PromptTextEdit::Kind::DeleteWordBackward ||
                    input.edit.kind ==
                        PromptTextEdit::Kind::DeleteWordForward) {
                    window_.selected = 0;
                    revealSelection();
                }
                break;
            case ClientOwnedInputKind::SelectNext:
                ++window_.selected;
                revealSelection();
                break;
            case ClientOwnedInputKind::SelectPrevious:
                if (window_.selected > 0) --window_.selected;
                revealSelection();
                break;
            case ClientOwnedInputKind::Submit:
                submitSelectedCandidate();
                break;
            case ClientOwnedInputKind::SystemClipboardPasteIntoEditor:
            case ClientOwnedInputKind::SystemClipboardPasteIntoText:
                break;
            }
        }

        [[nodiscard]] PaletteReport report() {
            return open_ ? buildPaletteReport(candidates_, window_)
                         : PaletteReport{};
        }

        void adopt(const GridPresentation& snapshot) {
            activation_ = snapshot.paletteView.activePicker;
            mode_ = activation_ ? activation_->mode : SearchMode::Command;
            if (auto const* published =
                    snapshot.paletteView.candidatesFor(mode_)) {
                candidates_ = *published;
            } else {
                candidates_.clear();
            }
            if (snapshot.document) {
                window_.paneRows = static_cast<std::uint32_t>(
                    std::max(snapshot.document->content.height, 1));
            }
            const bool wasOpen = open_;
            open_ = snapshot.prompt.activeKind == PromptKind::Palette;
            if (open_ && !wasOpen) {
                window_.query = PromptEditState{};
                window_.selected = 0;
                window_.firstVisible = 0;
            }
        }

        [[nodiscard]] std::optional<std::string>
        candidateId(std::size_t rankedIndex) const {
            const auto order = ranked();
            if (rankedIndex >= order.size()) return std::nullopt;
            return candidates_[order[rankedIndex]].id;
        }

        [[nodiscard]] std::optional<PickerActivation> activation() const {
            return activation_;
        }

      private:
        [[nodiscard]] std::vector<std::size_t> ranked() const {
            return rankPaletteCandidates(candidates_, window_.query.text());
        }

        [[nodiscard]] std::uint32_t candidateCount() const {
            return static_cast<std::uint32_t>(ranked().size());
        }

        void revealSelection() {
            const auto order = ranked();
            if (window_.selected >= order.size()) {
                window_.selected = order.empty() ? 0 : order.size() - 1;
            }
            if (order.empty()) return;
            ScrollOffset offset{window_.firstVisible};
            offset.revealSelection(
                static_cast<std::uint32_t>(window_.selected),
                static_cast<std::uint32_t>(order.size()), window_.paneRows);
            window_.firstVisible = offset.firstVisible();
        }

        void submitSelectedCandidate() {
            const auto id = candidateId(window_.selected);
            if (activation_ && id) {
                client_.trace(TerminalClientStage::Dispatch);
                (void)client_.editor.input(
                    PickerPointerInput{*activation_, *id});
            }
        }

        Impl& client_;
        bool open_ = false;
        SearchMode mode_ = SearchMode::Command;
        std::optional<PickerActivation> activation_;
        PaletteWindowState window_;
        std::vector<PaletteCandidate> candidates_;
    };

    Impl(Editor& configuredEditor, GridPresenter& configuredPresenter,
         TerminalSession& configuredTerminal,
         TerminalDimensions configuredDimensions,
         TerminalClientObserver configuredObserver)
        : editor{configuredEditor}, presenter{configuredPresenter},
          terminal{configuredTerminal},
          dimensions{configuredDimensions ? std::move(configuredDimensions)
                                          : TerminalDimensions{terminalSize}},
          observer{std::move(configuredObserver)},
          capabilities{environmentVariable, {},
                       configuredTerminal.supportsTruecolor()},
          encoder{capabilities.colorDepth()}, palette{*this} {}

    void trace(TerminalClientStage stage) const {
        if (observer) observer(stage);
    }

    ClientInputOutcome handleInputResult(ClientInputResult result) {
        if (result.clientOwned &&
            (result.clientOwned->kind ==
                 ClientOwnedInputKind::SystemClipboardPasteIntoEditor ||
             result.clientOwned->kind ==
                 ClientOwnedInputKind::SystemClipboardPasteIntoText)) {
            const auto paste = planSystemClipboardPaste(
                *result.clientOwned, clipboardReader.read());
            if (paste.kind == SystemClipboardPasteKind::CommittedText) {
                (void)routeInput(KeyStroke{}, paste.text);
            } else if (paste.kind ==
                       SystemClipboardPasteKind::InternalRegister) {
                trace(TerminalClientStage::Dispatch);
                (void)editor.dispatch("clipboard.paste");
            }
        } else if (result.clientOwned) {
            palette.apply(*result.clientOwned);
        }
        if (result.outcome != ClientInputOutcome::ViewOwned ||
            !result.command || !result.command->viewAction) {
            return result.outcome;
        }
        if (!displayed) return ClientInputOutcome::Rejected;
        auto applied = presenter.apply(*result.command->viewAction, *displayed);
        if (!applied.accepted()) return ClientInputOutcome::Rejected;
        if (applied.transition) {
            trace(TerminalClientStage::Dispatch);
            auto transition = editor.input(*applied.transition);
            if (transition.outcome == ClientInputOutcome::Rejected) {
                return transition.outcome;
            }
        }
        return result.outcome;
    }

    ClientInputOutcome routeInput(KeyStroke stroke, std::string text) {
        trace(TerminalClientStage::Dispatch);
        return handleInputResult(
            editor.input(ClientKeyInput{stroke, std::move(text)}));
    }

    void handlePointer(const Decoded& decoded) {
        pointer.lastColumn = decoded.pointer.column;
        pointer.lastRow = decoded.pointer.row;
        RegionHit hit;
        PointerTargets targets;
        std::optional<DocumentPosition> doubleClickPosition;

        if (displayed) {
            HitTester tester{*displayed};
            if (pointer.gutterDrag &&
                decoded.pointer.kind == PointerKind::drag) {
                hit.region = pointer.gutterDrag->region;
                int const relativeRow =
                    decoded.pointer.row - pointer.gutterDrag->gutterY;
                auto const fraction = gutter_fraction(
                    relativeRow, pointer.gutterDrag->grabOffset,
                    pointer.gutterDrag->travel);
                hit.scrollNumerator = fraction.numerator;
                hit.scrollDenominator = fraction.denominator;
            } else {
                hit = tester.at(decoded.pointer.column, decoded.pointer.row);
                const bool leftPress =
                    decoded.pointer.kind == PointerKind::press &&
                    decoded.pointer.button == PointerButton::left;
                if (leftPress && is_scrollbar_region(hit.region)) {
                    if (auto const thumb = tester.gutterThumb(hit.region)) {
                        int const relativeRow =
                            decoded.pointer.row - thumb->gutterY;
                        int const travel =
                            static_cast<int>(thumb->viewportRows) -
                            static_cast<int>(thumb->thumbSize);
                        int const grabOffset = scrollbar_grab_offset(
                            relativeRow, static_cast<int>(thumb->thumbStart),
                            static_cast<int>(thumb->thumbSize));
                        pointer.gutterDrag = GutterDrag{
                            hit.region, thumb->gutterY, travel, grabOffset};
                        auto const fraction =
                            gutter_fraction(relativeRow, grabOffset, travel);
                        hit.scrollNumerator = fraction.numerator;
                        hit.scrollDenominator = fraction.denominator;
                    }
                } else if (leftPress) {
                    pointer.gutterDrag.reset();
                }
            }

            if (hit.region == HitRegion::Editor) {
                targets.document_position = resolveSelectionPosition(
                    displayed->documentText, ByteOffset{hit.byteOffset});
            } else if (hit.region == HitRegion::Tab) {
                auto const& tabs = displayed->tabs.tabs;
                if (hit.tabIndex < tabs.size()) {
                    targets.tab_id = tabs[hit.tabIndex].id;
                }
            } else if (hit.region == HitRegion::Palette) {
                targets.picker_candidate_id =
                    palette.candidateId(hit.itemIndex);
                targets.picker_activation = palette.activation();
            } else if (hit.region == HitRegion::HeaderField ||
                       hit.region == HitRegion::FooterField) {
                if (hit.fieldId) targets.ui_node_id = UiNodeId{*hit.fieldId};
            } else if (hit.region == HitRegion::ExternalAction &&
                       hit.externalFileId && hit.commandId) {
                const auto fileId = DiffFileId{*hit.externalFileId};
                for (auto const& file :
                     displayed->externalModification.files) {
                    if (file.id != fileId) continue;
                    for (auto const& action : file.actions) {
                        if (action.command == *hit.commandId) {
                            targets.external_invocation =
                                ExternalActionInvocation{file.id,
                                                         action.action};
                            break;
                        }
                    }
                }
            }

            static constexpr auto doubleClickWindow =
                std::chrono::milliseconds{400};
            const bool leftEditorPress =
                decoded.pointer.kind == PointerKind::press &&
                decoded.pointer.button == PointerButton::left &&
                hit.region == HitRegion::Editor;
            if (leftEditorPress && targets.document_position &&
                register_click_is_double(
                    pointer.clickTracker, std::chrono::steady_clock::now(),
                    displayed->tabs.active
                        ? displayed->tabs.active->value()
                        : 0,
                    decoded.pointer.row, decoded.pointer.column,
                    doubleClickWindow)) {
                doubleClickPosition = targets.document_position;
            }
        }

        bool const effectiveAlt =
            decoded.pointer.kind == PointerKind::press ? decoded.pointer.alt
                                                       : pointer.altDrag;
        auto plan =
            doubleClickPosition
                ? double_click_dispatch(*doubleClickPosition)
                : route_pointer(hit, decoded.pointer.button,
                                decoded.pointer.kind, effectiveAlt,
                                pointer.dragging, pointer.dragAnchor, targets);
        if (plan.semantic_input) {
            trace(TerminalClientStage::Dispatch);
            (void)handleInputResult(editor.input(*plan.semantic_input));
        }
        if (plan.client_scroll &&
            plan.client_scroll->target == WheelTarget::palette) {
            palette.scrollToFraction(plan.client_scroll->numerator,
                                     plan.client_scroll->denominator);
        }
        if (plan.begins_drag) {
            pointer.dragging = true;
            pointer.dragAnchor = targets.document_position;
            pointer.altDrag = effectiveAlt;
        }
        if (decoded.pointer.kind == PointerKind::release) {
            pointer.gutterDrag.reset();
        }
        if (plan.ends_drag) {
            pointer.dragging = false;
            pointer.dragAnchor.reset();
            pointer.altDrag = false;
        }
    }

    bool present() {
        if (!presentationRequested) return false;
        presentationRequested = false;
        trace(TerminalClientStage::Project);
        auto snapshot =
            presenter.project(editor, {dimensions(), palette.report()});
        if (!snapshot) return false;
        focus = effectiveUiFocus(snapshot->uiTree);
        palette.adopt(*snapshot);
        if (auto const bytes = clipboardWriter.bytesFor(
                snapshot->clipboardWrite,
                capabilities.has(Capability::ClipboardWrite))) {
            trace(TerminalClientStage::ClipboardWrite);
            terminal.write(*bytes);
        }
        trace(TerminalClientStage::Render);
        auto grid = renderFrame(*snapshot, lineCache);
        trace(TerminalClientStage::Encode);
        const bool showCursor = !pointer.gutterDrag.has_value();
        auto encoded = encoder.encode(grid, showCursor);
        if (!encoded.bytes.empty()) {
            trace(TerminalClientStage::FrameWrite);
            terminal.write(encoded.bytes);
        }
        encoder.commit(grid, showCursor);
        displayed = std::move(snapshot);
        return !encoded.bytes.empty();
    }

    Editor& editor;
    GridPresenter& presenter;
    TerminalSession& terminal;
    TerminalDimensions dimensions;
    TerminalClientObserver observer;
    TerminalCapabilities capabilities;
    RetainedTerminalEncoder encoder;
    LineLayoutCache lineCache;
    PaletteView palette;
    PointerState pointer;
    SystemClipboardWriter clipboardWriter;
    SystemClipboardReader clipboardReader;
    std::optional<GridPresentation> displayed;
    std::string input;
    FocusTarget focus = FocusTarget::Editor;
    bool presentationRequested = true;
};

TerminalClient::TerminalClient(Editor& editor, GridPresenter& presenter,
                               TerminalSession& terminal,
                               TerminalDimensions dimensions,
                               TerminalClientObserver observer)
    : impl_{std::make_unique<Impl>(
          editor, presenter, terminal, std::move(dimensions),
          std::move(observer))} {}

TerminalClient::~TerminalClient() = default;

std::string TerminalClient::beginProbe() {
    return impl_->capabilities.beginProbe();
}

ColorDepth TerminalClient::colorDepth() const noexcept {
    return impl_->capabilities.colorDepth();
}

void TerminalClient::appendInput(std::string_view bytes) {
    impl_->input.append(bytes);
}

InputConsumption TerminalClient::consumeInput(bool inputExhausted) {
    while (!impl_->input.empty()) {
        std::size_t consumed = 0;
        auto decoded = decodeInput(impl_->input, inputExhausted, consumed);
        if (decoded.status == DecodeStatus::incomplete) {
            return {InputConsumption::Status::NeedMoreInput,
                    selectRuntimeWaitTimeout(
                        {.escapeSequencePending = true})};
        }
        impl_->input.erase(0, consumed);
        impl_->trace(TerminalClientStage::Decoded);

        if (decoded.status == DecodeStatus::reply) {
            impl_->capabilities.observeReply(decoded.reply);
            if (impl_->capabilities.has(Capability::KeyboardProtocol)) {
                impl_->terminal.enableKeyboardProtocol();
            }
            continue;
        }
        if (decoded.status == DecodeStatus::pointer &&
            decoded.pointer.kind == PointerKind::drag) {
            std::size_t peekConsumed = 0;
            const auto next =
                decodeInput(impl_->input, false, peekConsumed);
            if (next.status == DecodeStatus::pointer &&
                next.pointer.kind == PointerKind::drag) {
                continue;
            }
        }

        switch (decoded.status) {
        case DecodeStatus::pointer:
            impl_->handlePointer(decoded);
            break;
        case DecodeStatus::paste:
            if (!decoded.text.empty()) {
                (void)impl_->routeInput(KeyStroke{}, decoded.text);
            }
            break;
        case DecodeStatus::scroll: {
            HitRegion region = HitRegion::None;
            if (impl_->displayed) {
                region = HitTester{*impl_->displayed}
                             .at(decoded.pointer.column,
                                 decoded.pointer.row)
                             .region;
            }
            switch (route_wheel(region)) {
            case WheelTarget::editor:
                impl_->trace(TerminalClientStage::Dispatch);
                (void)impl_->handleInputResult(impl_->editor.input(
                    ScrollLinesInput{{ScrollTarget::Document,
                                      decoded.scroll}}));
                break;
            case WheelTarget::tree:
                impl_->trace(TerminalClientStage::Dispatch);
                (void)impl_->handleInputResult(impl_->editor.input(
                    ScrollLinesInput{{ScrollTarget::Tree,
                                      decoded.scroll}}));
                break;
            case WheelTarget::palette:
                impl_->palette.scroll(decoded.scroll);
                break;
            case WheelTarget::none:
                break;
            }
            break;
        }
        case DecodeStatus::key:
            if (applicationQuitRequested(decoded.stroke)) {
                return {InputConsumption::Status::Quit, std::nullopt};
            }
            (void)impl_->routeInput(decoded.stroke, decoded.text);
            break;
        case DecodeStatus::none:
            continue;
        case DecodeStatus::reply:
        case DecodeStatus::incomplete:
            break;
        }
        impl_->presentationRequested = true;
        (void)impl_->present();
        return {InputConsumption::Status::Consumed, std::nullopt};
    }
    return {InputConsumption::Status::Idle, std::nullopt};
}

bool TerminalClient::present() { return impl_->present(); }

void TerminalClient::requestPresentation(bool invalidateTerminal) noexcept {
    impl_->presentationRequested = true;
    if (invalidateTerminal) impl_->encoder.invalidate();
}

bool TerminalClient::pointerInMotion() const noexcept {
    return impl_->pointer.dragging || impl_->pointer.gutterDrag.has_value();
}

std::optional<int> TerminalClient::dragEdge() const {
    if (!impl_->pointer.dragging || !impl_->displayed ||
        !impl_->displayed->document) {
        return std::nullopt;
    }
    return edge_scroll(impl_->pointer.dragging, impl_->pointer.lastRow,
                       impl_->displayed->document->content);
}

void TerminalClient::advanceDragEdge(int direction) {
    impl_->trace(TerminalClientStage::Dispatch);
    (void)impl_->handleInputResult(impl_->editor.input(DocumentPointerInput{
        std::nullopt, false, false, InputPointerButton::Primary,
        InputPointerPhase::Move,
        direction < 0 ? DocumentPointerEdge::Before
                      : DocumentPointerEdge::After}));
    impl_->presentationRequested = true;
    (void)impl_->present();
}

const std::optional<GridPresentation>&
TerminalClient::displayedFrame() const noexcept {
    return impl_->displayed;
}

} // namespace ssg
