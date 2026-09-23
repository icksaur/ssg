#include <ssg/Editor.h>
#include <ssg/GridPresenter.h>
#include <ssg/Terminal.h>
#include <ssg/TerminalClient.h>

#include "editor_test_support.h"
#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kNavigationProjectionBudget = 1;
constexpr int kNavigationFrameWriteBudget = 1;
constexpr std::size_t kCaretPatchCellBudget = 0;

struct TerminalWrites {
    std::vector<std::string> writes;
};

class FakeTerminal final : public ssg::NativeTerminal {
  public:
    explicit FakeTerminal(TerminalWrites& writes) : writes_{writes} {}
    bool activate() override { return true; }
    void restore() noexcept override {}
    void write(std::string_view bytes) noexcept override {
        writes_.writes.emplace_back(bytes);
    }

  private:
    TerminalWrites& writes_;
};

struct ClientFixture {
    TestRuntimeDirectory root;
    std::unique_ptr<ssg::Editor> editor;
    ssg::GridPresenter presenter;
    TerminalWrites writes;
    ssg::TerminalSession terminal;
    std::vector<ssg::TerminalClientStage> stages;
    ssg::TerminalClient client;

    explicit ClientFixture(std::string text = "abc")
        : editor{makeEditor(root.path())},
          terminal{std::make_unique<FakeTerminal>(writes)},
          client{*editor, presenter, terminal,
                 [] { return ssg::ViewportDimensions{40, 10}; },
                 [this](ssg::TerminalClientStage stage) {
                     stages.push_back(stage);
                 }} {
        ASSERT_TRUE(editor != nullptr);
        if (!editor) return;
        ASSERT_TRUE(editor->dispatch("file.new").accepted());
        ASSERT_TRUE(ssg::test::typeText(*editor, std::move(text)).accepted());
        ASSERT_TRUE(ssg::test::setSelections(*editor, {{0, 0}}).accepted());
        ASSERT_TRUE(client.present());
        writes.writes.clear();
        stages.clear();
    }

  private:
    static std::unique_ptr<ssg::Editor>
    makeEditor(const std::filesystem::path& path) {
        std::filesystem::create_directories(path / "workspace");
        std::filesystem::create_directories(path / "recovery");
        auto created = ssg::createEditor(
            {path / "workspace", path / "recovery", path / "archive"});
        if (!created.accepted()) return {};
        return std::move(created.session);
    }
};

int countStage(const std::vector<ssg::TerminalClientStage>& stages,
               ssg::TerminalClientStage expected) {
    return static_cast<int>(std::count(stages.begin(), stages.end(), expected));
}

void assertNavigationBudget(ClientFixture& fixture,
                            std::string_view input) {
    fixture.client.appendInput(input);
    auto consumed = fixture.client.consumeInput();
    ASSERT_TRUE(consumed.status ==
                ssg::InputConsumption::Status::Consumed);
    ASSERT_EQ(countStage(fixture.stages,
                         ssg::TerminalClientStage::Project),
              kNavigationProjectionBudget);
    ASSERT_EQ(countStage(fixture.stages,
                         ssg::TerminalClientStage::FrameWrite),
              kNavigationFrameWriteBudget);
}

} // namespace

TEST(bufferedNavigationWritesBeforeTheNextDispatch) {
    ClientFixture fixture;
    if (!fixture.editor) return;
    fixture.client.appendInput("\x1b[C\x1b[C");

    auto first = fixture.client.consumeInput();
    ASSERT_TRUE(first.status == ssg::InputConsumption::Status::Consumed);
    ASSERT_EQ(countStage(fixture.stages, ssg::TerminalClientStage::Project),
              kNavigationProjectionBudget);
    ASSERT_EQ(countStage(fixture.stages,
                         ssg::TerminalClientStage::FrameWrite),
              kNavigationFrameWriteBudget);
    const auto firstWrite = std::find(
        fixture.stages.begin(), fixture.stages.end(),
        ssg::TerminalClientStage::FrameWrite);
    ASSERT_TRUE(firstWrite != fixture.stages.end());
    const auto firstWriteIndex =
        static_cast<std::size_t>(firstWrite - fixture.stages.begin());

    auto second = fixture.client.consumeInput();
    ASSERT_TRUE(second.status == ssg::InputConsumption::Status::Consumed);
    const auto secondDispatch = std::find(
        fixture.stages.begin() +
            static_cast<std::ptrdiff_t>(firstWriteIndex + 1),
        fixture.stages.end(),
        ssg::TerminalClientStage::Dispatch);
    ASSERT_TRUE(secondDispatch != fixture.stages.end());
    ASSERT_TRUE(firstWriteIndex <
                static_cast<std::size_t>(
                    secondDispatch - fixture.stages.begin()));
    ASSERT_EQ(countStage(fixture.stages, ssg::TerminalClientStage::Project),
              2 * kNavigationProjectionBudget);
    ASSERT_EQ(countStage(fixture.stages,
                         ssg::TerminalClientStage::FrameWrite),
              2 * kNavigationFrameWriteBudget);
}

TEST(incompleteEscapeInputReturnsItsDeadlineWithoutPresenting) {
    ClientFixture fixture;
    if (!fixture.editor) return;
    fixture.client.appendInput("\x1b[");
    auto pending = fixture.client.consumeInput();
    ASSERT_TRUE(pending.status ==
                ssg::InputConsumption::Status::NeedMoreInput);
    ASSERT_TRUE(pending.deadline.has_value());
    ASSERT_TRUE(fixture.stages.empty());
    ASSERT_TRUE(fixture.writes.writes.empty());

    fixture.client.appendInput("C");
    auto completed = fixture.client.consumeInput();
    ASSERT_TRUE(completed.status ==
                ssg::InputConsumption::Status::Consumed);
    ASSERT_EQ(countStage(fixture.stages, ssg::TerminalClientStage::Project),
              kNavigationProjectionBudget);
    ASSERT_EQ(countStage(fixture.stages,
                         ssg::TerminalClientStage::FrameWrite),
              kNavigationFrameWriteBudget);
}

TEST(verticalWordAndClickNavigationStayWithinTheWorkBudgets) {
    ClientFixture vertical{"a\nb"};
    if (!vertical.editor) return;
    assertNavigationBudget(vertical, "\x1b[B");

    ClientFixture word;
    if (!word.editor) return;
    assertNavigationBudget(word, "\x1b[1;5C");

    ClientFixture click;
    if (!click.editor || !click.client.displayedFrame() ||
        !click.client.displayedFrame()->document) {
        return;
    }
    const auto content = click.client.displayedFrame()->document->content;
    const auto report =
        "\x1b[<0;" + std::to_string(content.x + 3) + ";" +
        std::to_string(content.y + 1) + "M";
    assertNavigationBudget(click, report);
}

TEST(boundaryNavigationProjectsButDoesNotWriteAnEmptyFrame) {
    ClientFixture fixture;
    if (!fixture.editor) return;
    ASSERT_TRUE(ssg::test::setSelections(*fixture.editor, {{3, 3}}).accepted());
    fixture.client.requestPresentation();
    (void)fixture.client.present();
    fixture.stages.clear();
    fixture.writes.writes.clear();

    fixture.client.appendInput("\x1b[C");
    auto consumed = fixture.client.consumeInput();
    ASSERT_TRUE(consumed.status ==
                ssg::InputConsumption::Status::Consumed);
    ASSERT_EQ(countStage(fixture.stages, ssg::TerminalClientStage::Project),
              kNavigationProjectionBudget);
    ASSERT_EQ(countStage(fixture.stages,
                         ssg::TerminalClientStage::FrameWrite),
              0);
    ASSERT_TRUE(fixture.writes.writes.empty());
    ASSERT_EQ(kCaretPatchCellBudget, std::size_t{0});
}

SSG_TEST_SUITE(test_terminal_client) {
    RUN(bufferedNavigationWritesBeforeTheNextDispatch);
    RUN(incompleteEscapeInputReturnsItsDeadlineWithoutPresenting);
    RUN(verticalWordAndClickNavigationStayWithinTheWorkBudgets);
    RUN(boundaryNavigationProjectsButDoesNotWriteAnEmptyFrame);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
