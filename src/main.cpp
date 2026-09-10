#include <ssg/pointer_routing.h>
#include <ssg/Terminal.h>
#include <ssg/TerminalCapabilities.h>
#include <ssg/TerminalInput.h>
#include <ssg/TerminalOutput.h>

#include <ssg/Editor.h>
#include <ssg/GridPresenter.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/TreeSitterGrammars.h>
#include <ssg/HitTester.h>
#include <ssg/ScriptHost.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Picker.h>
#include <ssg/platform_files.h>
#include <ssg/TextInputCommands.h>
#include <ssg/SystemClipboardReader.h>

#include <ssg/InitScriptWatcher.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ssg {

namespace fs = std::filesystem;

#ifdef SSG_STARTUP_TRACE_ENABLED
void recordStartupMark(char const* phase) {
    static char const* const path = std::getenv("SSG_STARTUP_TRACE");
    if (path == nullptr) return;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long const ns = static_cast<long long>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
    // seam-exempt: append-only diagnostic trace, not file content access
    if (std::FILE* file = std::fopen(path, "a"); file != nullptr) {
        std::fprintf(file, "%s %lld\n", phase, ns);
        std::fclose(file);
    }
}
#else
void recordStartupMark(char const*) {}
#endif

namespace {

constexpr int kEscapeTimeoutMs = 30;
constexpr int kEdgeScrollIntervalMs = 40;
constexpr int kAutosaveTickMs = 1000;
constexpr int kDragFrameIntervalMs = 16;
volatile std::sig_atomic_t gSignalPipeWrite = -1;

struct LaunchTarget {
    fs::path cwd;
    std::optional<std::string> file;
};

LaunchTarget resolveLaunch(const fs::path& argument) {
    if (argument.empty()) return {fs::current_path(), std::nullopt};

    const auto status = ssg::statFile(argument);
    if (status && status->kind == ssg::FileKind::Directory) {
        return {fs::absolute(argument), std::nullopt};
    }
    const auto absolute = fs::absolute(argument);
    const auto parent =
        absolute.has_parent_path() ? absolute.parent_path() : fs::current_path();
    return {parent, absolute.filename().string()};
}

struct FdReadiness {
    bool input = false;
    bool signal = false;
    bool gitDiff = false;
    bool initScript = false;
};

FdReadiness waitReadiness(int timeoutMs, int signalFd, int gitDiffFd, int initScriptFd = -1) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    if (signalFd >= 0) FD_SET(signalFd, &set);
    int maxFd = std::max(STDIN_FILENO, signalFd);
    if (gitDiffFd >= 0) {
        FD_SET(gitDiffFd, &set);
        maxFd = std::max(maxFd, gitDiffFd);
    }
    if (initScriptFd >= 0) {
        FD_SET(initScriptFd, &set);
        maxFd = std::max(maxFd, initScriptFd);
    }
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int const ready = ::select(maxFd + 1, &set, nullptr, nullptr, timeoutMs < 0 ? nullptr : &timeout);
    if (ready <= 0) return {};
    return {FD_ISSET(STDIN_FILENO, &set) != 0, signalFd >= 0 ? FD_ISSET(signalFd, &set) != 0 : false, gitDiffFd >= 0 ? FD_ISSET(gitDiffFd, &set) != 0 : false,
            initScriptFd >= 0 ? FD_ISSET(initScriptFd, &set) != 0 : false};
}

extern "C" void signalTagHandler(int signo) {
    int const fd = gSignalPipeWrite;
    if (fd < 0) return;
    unsigned char const tag = static_cast<unsigned char>(signo);
    ssize_t const written = ::write(fd, &tag, 1);
    (void)written;
}

void installSignalTagHandler(int signo) {
    struct sigaction action{};
    action.sa_handler = signalTagHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(signo, &action, nullptr);
}

void popGrapheme(std::string& text) {
    if (text.empty()) return;
    const auto run = ssg::computeCellRun(text);
    if (!run.spans.empty()) text.resize(run.spans.back().byteOffset);
}

void popWord(std::string& text) {
    while (!text.empty() && !ssg::isWordByte(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    while (!text.empty() && ssg::isWordByte(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
}

} // namespace

ViewActionResult applyScriptViewAction(Editor& runtime, GridPresenter& presenter, const ViewAction& request) {
    auto frame = presenter.project(runtime, {terminalSize(), {}});
    if (!frame) {
        return {ViewActionStatus::Rejected, std::nullopt, "view action has no current grid frame"};
    }
    return presenter.apply(request, *frame);
}

const char* environmentVariable(std::string_view name) { return std::getenv(std::string{name}.c_str()); }

class PaletteView;

struct SsgContext {
    Editor& runtime;
    GridPresenter& presenter;
    ScriptHost& scripts;
    TerminalSession& terminal;
    InitScriptWatcher* initScript;
    int gitDiffWakeFd;
    int signalReadFd;
    std::unique_ptr<PaletteView> palette;
    std::optional<GridPresentation> activeSnapshot;
    FocusTarget focus = FocusTarget::Editor;
    SystemClipboardWriter clipboardWriter;
    SystemClipboardReader clipboardReader;
    TerminalCapabilities capabilities{environmentVariable};
};

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

void drainSignals(SsgContext& context);
std::optional<GridPresentation> projectFrame(SsgContext& context);
ClientInputOutcome handleInputResult(SsgContext& context, ClientInputResult result);
ClientInputOutcome routeInput(SsgContext& context, KeyStroke stroke, std::string text);
void handleTerminalReply(SsgContext& context, const Decoded& decoded);
void handlePaste(SsgContext& context, const Decoded& decoded);
void handleScroll(SsgContext& context, const Decoded& decoded);
void handleKey(SsgContext& context, const Decoded& decoded, bool& quit);
void handlePointer(SsgContext& context, const Decoded& decoded, PointerState& pointer);
void dispatchBufferedInput(SsgContext& context, std::string& buffer, PointerState& pointer, bool& quit);

class PaletteView {
  public:
    explicit PaletteView(SsgContext& context) : context_{context} {}

    void scroll(std::int64_t delta) {
        if (!open_) return;
        auto offset = scrollOffset();
        offset.byLines(delta, candidateCount(), window_.paneRows);
        window_.firstVisible = offset.firstVisible();
    }

    void scrollToFraction(std::uint32_t numerator, std::uint32_t denominator) {
        if (!open_) return;
        auto offset = scrollOffset();
        offset.toFraction(numerator, denominator, candidateCount(), window_.paneRows);
        window_.firstVisible = offset.firstVisible();
    }

    void apply(const ClientOwnedInput& input) {
        switch (input.kind) {
        case ClientOwnedInputKind::AppendText:
            window_.query += input.text;
            window_.selected = 0;
            revealSelection();
            break;
        case ClientOwnedInputKind::DeleteGraphemeBackward:
            popGrapheme(window_.query);
            window_.selected = 0;
            revealSelection();
            break;
        case ClientOwnedInputKind::DeleteWordBackward:
            popWord(window_.query);
            window_.selected = 0;
            revealSelection();
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

    [[nodiscard]] PaletteReport report() { return open_ ? buildPaletteReport(candidates_, window_) : PaletteReport{}; }

    void adopt(const GridPresentation& snapshot) {
        activation_ = snapshot.paletteView.activePicker;
        mode_ = activation_ ? activation_->mode : SearchMode::Command;
        if (auto const* published = snapshot.paletteView.candidatesFor(mode_)) {
            candidates_ = *published;
        } else {
            candidates_.clear();
        }
        if (snapshot.document) {
            window_.paneRows = static_cast<std::uint32_t>(std::max(snapshot.document->content.height, 1));
        }
        const bool wasOpen = open_;
        open_ = snapshot.promptStatus.activeKind == PromptKind::Palette;
        if (open_ && !wasOpen) {
            window_.query.clear();
            window_.selected = 0;
            window_.firstVisible = 0;
        }
    }

    [[nodiscard]] std::optional<std::string> candidateId(std::size_t rankedIndex) const {
        const auto order = ranked();
        if (rankedIndex >= order.size()) return std::nullopt;
        return candidates_[order[rankedIndex]].id;
    }

    [[nodiscard]] std::optional<PickerActivation> activation() const { return activation_; }

  private:
    [[nodiscard]] std::vector<std::size_t> ranked() const { return rankPaletteCandidates(candidates_, window_.query); }

    [[nodiscard]] std::uint32_t candidateCount() const { return static_cast<std::uint32_t>(ranked().size()); }

    [[nodiscard]] ScrollOffset scrollOffset() const { return ScrollOffset{window_.firstVisible}; }

    void revealSelection() {
        const auto order = ranked();
        if (window_.selected >= order.size()) {
            window_.selected = order.empty() ? 0 : order.size() - 1;
        }
        if (order.empty()) return;
        auto offset = scrollOffset();
        offset.revealSelection(static_cast<std::uint32_t>(window_.selected), static_cast<std::uint32_t>(order.size()), window_.paneRows);
        window_.firstVisible = offset.firstVisible();
    }

    void submitSelectedCandidate() {
        const auto id = candidateId(window_.selected);
        if (activation_ && id) {
            (void)context_.runtime.input(PickerPointerInput{*activation_, *id});
        }
    }

    SsgContext& context_;
    bool open_ = false;
    SearchMode mode_ = SearchMode::Command;
    std::optional<PickerActivation> activation_;
    PaletteWindowState window_;
    std::vector<PaletteCandidate> candidates_;
};

void drainSignals(SsgContext& context) {
    char scratch[64];
    std::string tags;
    for (;;) {
        auto const n = ::read(context.signalReadFd, scratch, sizeof scratch);
        if (n <= 0) break;
        tags.append(scratch, static_cast<std::size_t>(n));
    }
    auto const events = classifySignalTags(tags);
    if (events.terminate) {
        int const signo = *events.terminate;
        context.terminal.restore();
        ::signal(signo, SIG_DFL);
        ::raise(signo);
    }
}

std::optional<GridPresentation> projectFrame(SsgContext& context) {
    auto snapshot = context.presenter.project(context.runtime, {terminalSize(), context.palette->report()});
    if (!snapshot) return std::nullopt;

    context.focus = effectiveUiFocus(snapshot->uiTree);
    context.palette->adopt(*snapshot);
    if (auto const bytes = context.clipboardWriter.bytesFor(snapshot->clipboardWrite, context.capabilities.has(Capability::ClipboardWrite))) {
        writeAll(*bytes);
    }
    return snapshot;
}

ClientInputOutcome handleInputResult(SsgContext& context, ClientInputResult result) {
    if (result.command) context.activeSnapshot.reset();
    if (result.clientOwned &&
        (result.clientOwned->kind ==
             ClientOwnedInputKind::SystemClipboardPasteIntoEditor ||
         result.clientOwned->kind ==
             ClientOwnedInputKind::SystemClipboardPasteIntoText)) {
        const auto paste = planSystemClipboardPaste(
            *result.clientOwned, context.clipboardReader.read());
        if (paste.kind == SystemClipboardPasteKind::CommittedText) {
            (void)routeInput(context, KeyStroke{}, paste.text);
        } else if (paste.kind == SystemClipboardPasteKind::InternalRegister) {
            (void)context.runtime.dispatch("clipboard.paste");
            context.activeSnapshot.reset();
        }
    } else if (result.clientOwned) {
        context.palette->apply(*result.clientOwned);
    }
    if (result.outcome != ClientInputOutcome::ViewOwned || !result.command || !result.command->viewAction) {
        return result.outcome;
    }
    if (!context.activeSnapshot) context.activeSnapshot = projectFrame(context);
    if (!context.activeSnapshot) return ClientInputOutcome::Rejected;
    auto applied = context.presenter.apply(*result.command->viewAction, *context.activeSnapshot);
    if (!applied.accepted()) return ClientInputOutcome::Rejected;
    if (applied.transition) {
        auto transition = context.runtime.input(*applied.transition);
        if (transition.outcome == ClientInputOutcome::Rejected) {
            return transition.outcome;
        }
    }
    context.activeSnapshot.reset();
    return result.outcome;
}

ClientInputOutcome routeInput(SsgContext& context, KeyStroke stroke, std::string text) { return handleInputResult(context, context.runtime.input(ClientKeyInput{stroke, std::move(text)})); }

void handleTerminalReply(SsgContext& context, const Decoded& decoded) {
    context.capabilities.observeReply(decoded.reply);
    if (context.capabilities.has(Capability::KeyboardProtocol)) {
        context.terminal.enableKeyboardProtocol();
    }
}

void handlePaste(SsgContext& context, const Decoded& decoded) {
    if (decoded.text.empty()) return;
    (void)routeInput(context, KeyStroke{}, decoded.text);
}

void handleScroll(SsgContext& context, const Decoded& decoded) {
    HitRegion region = HitRegion::None;
    if (context.activeSnapshot) {
        region = HitTester{*context.activeSnapshot}.at(decoded.pointer.column, decoded.pointer.row).region;
    }
    switch (route_wheel(region)) {
    case WheelTarget::editor:
        (void)handleInputResult(context, context.runtime.input(ScrollLinesInput{{ScrollTarget::Document, decoded.scroll}}));
        break;
    case WheelTarget::tree:
        (void)handleInputResult(context, context.runtime.input(ScrollLinesInput{{ScrollTarget::Tree, decoded.scroll}}));
        break;
    case WheelTarget::palette:
        context.palette->scroll(decoded.scroll);
        break;
    case WheelTarget::none:
        break;
    }
}

void handleKey(SsgContext& context, const Decoded& decoded, bool& quit) {
    if (applicationQuitRequested(decoded.stroke)) {
        quit = true;
        return;
    }
    (void)routeInput(context, decoded.stroke, decoded.text);
}

void handlePointer(SsgContext& context, const Decoded& decoded, PointerState& pointer) {
    pointer.lastColumn = decoded.pointer.column;
    pointer.lastRow = decoded.pointer.row;
    RegionHit hit;
    PointerTargets targets;
    std::optional<DocumentPosition> doubleClickPosition;

    if (context.activeSnapshot) {
        HitTester tester{*context.activeSnapshot};
        if (pointer.gutterDrag && decoded.pointer.kind == PointerKind::drag) {
            hit.region = pointer.gutterDrag->region;
            int const relativeRow = decoded.pointer.row - pointer.gutterDrag->gutterY;
            auto const fraction = gutter_fraction(relativeRow, pointer.gutterDrag->grabOffset, pointer.gutterDrag->travel);
            hit.scrollNumerator = fraction.numerator;
            hit.scrollDenominator = fraction.denominator;
        } else {
            hit = tester.at(decoded.pointer.column, decoded.pointer.row);
            const bool leftPress = decoded.pointer.kind == PointerKind::press && decoded.pointer.button == PointerButton::left;
            if (leftPress && is_scrollbar_region(hit.region)) {
                if (auto const thumb = tester.gutterThumb(hit.region)) {
                    int const relativeRow = decoded.pointer.row - thumb->gutterY;
                    int const travel = static_cast<int>(thumb->viewportRows) - static_cast<int>(thumb->thumbSize);
                    int const grabOffset = scrollbar_grab_offset(relativeRow, static_cast<int>(thumb->thumbStart), static_cast<int>(thumb->thumbSize));
                    pointer.gutterDrag = GutterDrag{hit.region, thumb->gutterY, travel, grabOffset};
                    auto const fraction = gutter_fraction(relativeRow, grabOffset, travel);
                    hit.scrollNumerator = fraction.numerator;
                    hit.scrollDenominator = fraction.denominator;
                }
            } else if (leftPress) {
                pointer.gutterDrag.reset();
            }
        }

        if (hit.region == HitRegion::Editor) {
            targets.document_position = resolveSelectionPosition(context.activeSnapshot->documentText, ByteOffset{hit.byteOffset});
        } else if (hit.region == HitRegion::Tab) {
            auto const& tabs = context.activeSnapshot->tabs.tabs;
            if (hit.tabIndex < tabs.size()) {
                targets.tab_id = tabs[hit.tabIndex].id;
            }
        } else if (hit.region == HitRegion::Palette) {
            targets.picker_candidate_id = context.palette->candidateId(hit.itemIndex);
            targets.picker_activation = context.palette->activation();
        } else if (hit.region == HitRegion::HeaderField || hit.region == HitRegion::FooterField) {
            if (hit.fieldId) targets.ui_node_id = UiNodeId{*hit.fieldId};
        } else if (hit.region == HitRegion::NoticeAction) {
            targets.notice_action_id = hit.fieldId;
        } else if (hit.region == HitRegion::ExternalAction && hit.externalFileId && hit.commandId) {
            const auto fileId = DiffFileId{*hit.externalFileId};
            for (auto const& file : context.activeSnapshot->externalModification.files) {
                if (file.id != fileId) continue;
                for (auto const& action : file.actions) {
                    if (action.command == *hit.commandId) {
                        targets.external_invocation = ExternalActionInvocation{file.id, action.action};
                        break;
                    }
                }
            }
        }

        static constexpr auto doubleClickWindow = std::chrono::milliseconds{400};
        const bool leftEditorPress = decoded.pointer.kind == PointerKind::press && decoded.pointer.button == PointerButton::left && hit.region == HitRegion::Editor;
        if (leftEditorPress && targets.document_position &&
            register_click_is_double(pointer.clickTracker, std::chrono::steady_clock::now(), context.activeSnapshot->tabs.active ? context.activeSnapshot->tabs.active->value() : 0,
                                     decoded.pointer.row, decoded.pointer.column, doubleClickWindow)) {
            doubleClickPosition = targets.document_position;
        }
    }

    bool const effectiveAlt = decoded.pointer.kind == PointerKind::press ? decoded.pointer.alt : pointer.altDrag;
    auto plan = doubleClickPosition ? double_click_dispatch(*doubleClickPosition)
                                    : route_pointer(hit, decoded.pointer.button, decoded.pointer.kind, effectiveAlt, pointer.dragging, pointer.dragAnchor, targets);
    if (plan.semantic_input) {
        (void)handleInputResult(context, context.runtime.input(*plan.semantic_input));
    }
    if (plan.client_scroll && plan.client_scroll->target == WheelTarget::palette) {
        context.palette->scrollToFraction(plan.client_scroll->numerator, plan.client_scroll->denominator);
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

void dispatchBufferedInput(SsgContext& context, std::string& buffer, PointerState& pointer, bool& quit) {
    char bytes[4096];
    while (!buffer.empty() && !quit) {
        std::size_t consumed = 0;
        auto decoded = decodeInput(buffer, false, consumed);
        if (decoded.status == DecodeStatus::incomplete) {
            auto const ready = waitReadiness(kEscapeTimeoutMs, context.signalReadFd, -1);
            if (ready.signal) drainSignals(context);
            if (ready.input) {
                auto more = ::read(STDIN_FILENO, bytes, sizeof bytes);
                if (more > 0) {
                    buffer.append(bytes, static_cast<std::size_t>(more));
                    continue;
                }
            }
            decoded = decodeInput(buffer, true, consumed);
            if (decoded.status == DecodeStatus::incomplete) return;
        }
        buffer.erase(0, consumed);

        if (decoded.status == DecodeStatus::reply) {
            handleTerminalReply(context, decoded);
            continue;
        }
        if (decoded.status == DecodeStatus::pointer && decoded.pointer.kind == PointerKind::drag) {
            std::size_t peekConsumed = 0;
            const auto next = decodeInput(buffer, false, peekConsumed);
            if (next.status == DecodeStatus::pointer && next.pointer.kind == PointerKind::drag) {
                continue;
            }
        }

        context.activeSnapshot = projectFrame(context);
        switch (decoded.status) {
        case DecodeStatus::pointer:
            handlePointer(context, decoded, pointer);
            break;
        case DecodeStatus::paste:
            handlePaste(context, decoded);
            break;
        case DecodeStatus::scroll:
            handleScroll(context, decoded);
            break;
        case DecodeStatus::key:
            handleKey(context, decoded, quit);
            break;
        case DecodeStatus::reply:
        case DecodeStatus::none:
        case DecodeStatus::incomplete:
            break;
        }
    }
}

} // namespace ssg

int main(int argc, char** argv) {
    using namespace ssg;

    recordStartupMark("main_entry");
    const int firstOperand = argc > 1 && std::string_view{argv[1]} == "--" ? 2 : 1;
    const fs::path argument = argc > firstOperand ? argv[firstOperand] : fs::path{};
    auto target = resolveLaunch(argument);

    fs::path stateBase;
    if (const char* stateOverride = std::getenv("SSG_STATE_DIR"); stateOverride != nullptr && *stateOverride != '\0' && fs::path{stateOverride}.is_absolute()) {
        stateBase = stateOverride;
    } else {
        stateBase = ssg::userStateRoot("ssg");
    }
    (void)ssg::createDirectoriesDurably(stateBase / "scratch");
    (void)ssg::createDirectoriesDurably(stateBase / "archive");
    for (const auto& dir : {stateBase, stateBase / "scratch", stateBase / "archive"}) {
        const auto status = ssg::statFile(dir);
        if (status && status->kind == ssg::FileKind::Directory) {
            try {
                ssg::setOwnerOnlyPermissions(dir);
            } catch (const std::exception&) {
            }
        }
    }

    auto recoveryBase = fs::temp_directory_path() / ("ssg-" + std::to_string(::getpid()));
    (void)ssg::createDirectoriesDurably(recoveryBase / "recovery");

    ssg::EditorConfig config;
    config.cwd = target.cwd;
    config.scratchRoot = stateBase / "scratch";
    config.recoveryRoot = recoveryBase / "recovery";
    config.archiveRoot = stateBase / "archive";
    config.deferEnrichment = true;
    config.syntaxParser = ssg::TreeSitterParserFactory::createDefault();
    auto created = ssg::createEditor(config);
    if (!created.accepted()) {
        std::fprintf(stderr, "ssg: %s\n", created.message.c_str());
        return 1;
    }
    auto runtimeOwner = std::move(created.session);
    auto& runtime = *runtimeOwner;
    const int gitDiffWakeFd = runtime.gitDiffWakeDescriptor();
    recordStartupMark("post_create");

    ssg::GridPresenter presenter;
    recordStartupMark("post_presenter_init");
    ssg::ScriptHost scripts{runtime, std::bind_front(applyScriptViewAction, std::ref(runtime), std::ref(presenter))};
    auto const appliedInitScript = loadInitScript(scripts, runtime);
    std::optional<InitScriptWatcher> initScriptWatcher;
    if (auto scriptPath = resolveInitScriptPath()) {
        initScriptWatcher.emplace(*scriptPath, appliedInitScript);
    }

    bool startsWithAnEditableDocument = false;
    bool openedNamedFile = false;
    if (target.file) {
        // A named file that exists is opened; a named file that does not is
        // created as an unsaved buffer claiming that name, so the user can type
        // and save without naming it again.
        auto const openResult = ssg::statFile(target.cwd / *target.file)
                                    ? applyFilePathCompletion(
                                          runtime, PromptCompletion::FileOpen,
                                          *target.file)
                                    : createFileByPath(runtime, *target.file);
        if (!openResult.accepted) {
            std::fprintf(stderr, "ssg: %s\n", openResult.message.c_str());
        }
        startsWithAnEditableDocument = openResult.accepted;
        openedNamedFile = openResult.accepted;
    }
    if (!startsWithAnEditableDocument) {
        startsWithAnEditableDocument = runtime.dispatch("file.new").accepted();
    }
    recordStartupMark("post_open");

    TerminalSession terminal;
    if (!terminal.active()) {
        std::fprintf(stderr, "ssg: stdin/stdout is not an interactive terminal\n");
        return 1;
    }

    int signalPipe[2] = {-1, -1};
    if (::pipe(signalPipe) != 0) {
        std::fprintf(stderr, "ssg: failed to create signal pipe\n");
        return 1;
    }
    ::fcntl(signalPipe[0], F_SETFL, ::fcntl(signalPipe[0], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(signalPipe[1], F_SETFL, ::fcntl(signalPipe[1], F_GETFL, 0) | O_NONBLOCK);
    gSignalPipeWrite = signalPipe[1];
    installSignalTagHandler(SIGWINCH);
    installSignalTagHandler(SIGTERM);
    installSignalTagHandler(SIGHUP);

    SsgContext context{runtime, presenter, scripts, terminal, initScriptWatcher ? &*initScriptWatcher : nullptr, gitDiffWakeFd, signalPipe[0]};
    context.palette = std::make_unique<PaletteView>(context);
    auto& mode = context.terminal;
    ssg::ColorDepth const colorDepth = context.capabilities.colorDepth();
    writeAll(context.capabilities.beginProbe());

    std::string buffer;
    bool quit = false;
    PointerState pointer;

    // Once inside main, exception unwinding does not portably restore the terminal.
    bool firstFrameMarked = false;
    auto lastFrameAt = std::chrono::steady_clock::time_point{};
    ssg::LineLayoutCache renderLineCache;
    try {
        while (!quit) {
            // Motion floods can otherwise render every event and pin a core.
            if (pointer.gutterDrag.has_value() || pointer.dragging) {
                const auto sinceFrame = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastFrameAt).count();
                if (sinceFrame < kDragFrameIntervalMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{kDragFrameIntervalMs - sinceFrame});
                }
            }
            lastFrameAt = std::chrono::steady_clock::now();
            context.activeSnapshot = projectFrame(context);
            auto& snapshot = context.activeSnapshot;
            if (snapshot) {
                auto grid = ssg::renderFrame(*snapshot, renderLineCache);
                std::string frame =
                    ssg::encodeFrame(grid, colorDepth,
                                     !pointer.gutterDrag.has_value());
                if (!firstFrameMarked) {
                    recordStartupMark("first_content_frame");
                    firstFrameMarked = true;
                    writeAll(frame);
                    runtime.primeDeferred();
                    // The file provider does not exist until deferred enrichment.
                    // A named file should not start obscured by the sidebar.
                    if (!openedNamedFile) {
                        if (auto const panelResult = runtime.dispatch("panel.show_files"); !panelResult.accepted()) {
                            std::fprintf(stderr, "ssg: could not open Files sidebar: %s\n", panelResult.message.c_str());
                        }
                    }
                    // Showing the sidebar focuses it, so restore the requested file.
                    if (startsWithAnEditableDocument) {
                        runtime.focusEditor();
                    }
                    continue;
                }
                writeAll(frame);
            }

            // One read should contain enough of a motion burst to coalesce it.
            char bytes[4096];
            std::optional<int> dragEdge;
            if (pointer.dragging && snapshot && snapshot->document) {
                dragEdge = ssg::edge_scroll(pointer.dragging, pointer.lastRow, snapshot->document->content);
            }
            if (dragEdge) {
                auto const ready = waitReadiness(kEdgeScrollIntervalMs, context.signalReadFd, -1);
                if (ready.signal) {
                    // A held drag must not defer resize or termination.
                    drainSignals(context);
                    continue;
                }
                if (!ready.input) {
                    (void)handleInputResult(context, runtime.input(ssg::DocumentPointerInput{std::nullopt, false, false, ssg::InputPointerButton::Primary, ssg::InputPointerPhase::Move,
                                                                                             *dragEdge < 0 ? ssg::DocumentPointerEdge::Before : ssg::DocumentPointerEdge::After}));
                    continue;
                }
            }
            // The self-pipe makes resize and termination reliable across read(2).
            // An autosave timeout repaints only when a draft was written.
            FdReadiness wait;
            while (true) {
                const auto timeout =
                    runtime.workspaceSearchPending() ? 0 : kAutosaveTickMs;
                wait = waitReadiness(timeout, context.signalReadFd, context.gitDiffWakeFd, initScriptWatcher ? initScriptWatcher->wakeDescriptor() : -1);
                if (wait.input || wait.signal || wait.gitDiff || wait.initScript) {
                    break;
                }
                if (runtime.workspaceSearchPending()) break;
                if (runtime.flushDueAutosaveDrafts() > 0) break;
            }
            if (wait.signal) {
                drainSignals(context);
                if (!wait.input && !runtime.workspaceSearchPending()) continue;
            }
            if (wait.initScript) {
                // ScriptHost belongs to the main thread.
                initScriptWatcher->drainAndEvaluate(scripts, runtime);
                if (!wait.input && !runtime.workspaceSearchPending()) continue;
            }
            if (wait.gitDiff) {
                (void)runtime.pump();
                if (!wait.input && !runtime.workspaceSearchPending()) continue;
            }
            if (wait.input) {
                auto readBytes = ::read(STDIN_FILENO, bytes, sizeof bytes);
                if (readBytes <= 0) break;
                buffer.append(bytes, static_cast<std::size_t>(readBytes));
                dispatchBufferedInput(context, buffer, pointer, quit);
            }
            runtime.advanceWorkspaceSearch();
        }
    } catch (std::exception const& error) {
        mode.restore();
        std::fprintf(stderr, "ssg: %s\n", error.what());
        return 1;
    } catch (...) {
        mode.restore();
        std::fprintf(stderr, "ssg: terminated by an unknown error\n");
        return 1;
    }

    // Crashes rely on the last debounced draft; only clean exit reaches this flush.
    runtime.flushAllAutosaveDrafts();

    return 0;
}
