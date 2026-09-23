#pragma once

#include <ssg/Renderer.h>
#include <ssg/RuntimeTiming.h>
#include <ssg/color.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace ssg {

class Editor;
class GridPresenter;
class TerminalSession;

enum class TerminalClientStage : std::uint8_t {
    Decoded,
    Dispatch,
    Project,
    Render,
    Encode,
    FrameWrite,
    ClipboardWrite,
};

using TerminalClientObserver = std::function<void(TerminalClientStage)>;
using TerminalDimensions = std::function<ViewportDimensions()>;

struct InputConsumption {
    enum class Status : std::uint8_t { Idle, Consumed, NeedMoreInput, Quit };

    Status status = Status::Idle;
    std::optional<std::chrono::milliseconds> deadline;
};

class TerminalClient {
  public:
    TerminalClient(Editor& editor, GridPresenter& presenter,
                   TerminalSession& terminal,
                   TerminalDimensions dimensions = {},
                   TerminalClientObserver observer = {});
    ~TerminalClient();
    TerminalClient(const TerminalClient&) = delete;
    TerminalClient& operator=(const TerminalClient&) = delete;

    [[nodiscard]] std::string beginProbe();
    [[nodiscard]] ColorDepth colorDepth() const noexcept;

    void appendInput(std::string_view bytes);
    // Geometry-dependent input uses displayedFrame(), and each state-changing
    // non-motion event is presented before another such event is consumed.
    [[nodiscard]] InputConsumption consumeInput(bool inputExhausted = false);

    [[nodiscard]] bool present();
    void requestPresentation(bool invalidateTerminal = false) noexcept;
    [[nodiscard]] bool pointerInMotion() const noexcept;
    [[nodiscard]] std::optional<int> dragEdge() const;
    void advanceDragEdge(int direction);

    // This is the frame whose terminal state and geometry the user can observe.
    [[nodiscard]] const std::optional<GridPresentation>&
    displayedFrame() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
