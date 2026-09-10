#include <ssg/Command.h>
#include "test_helpers.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

ssg::CommandResult success() { return {}; }

TEST(addFindAndEnumerateInIdOrder) {
    ssg::Commands commands;
    commands.add("z.last", "Last", success);
    commands.add("a.first", "First", success);

    ASSERT_TRUE(commands.find("a.first") != nullptr);
    ASSERT_TRUE(commands.find("missing") == nullptr);
    std::vector<std::string> ids;
    for (auto const& [id, command] : commands.all()) {
        (void)command;
        ids.push_back(id);
    }
    ASSERT_EQ(ids, (std::vector<std::string>{"a.first", "z.last"}));
}

TEST(dispatchReportsSuccessUnknownFailureThrowAndViewAction) {
    ssg::Commands commands;
    commands.add("ok", "OK", success);
    commands.add("fail", "Fail", [] {
        return ssg::CommandResult{ssg::CommandError::HandlerFailed, "refused", {}};
    });
    commands.add("throw", "Throw", []() -> ssg::CommandResult {
        throw std::runtime_error{"boom"};
    });
    commands.add("view", "View", [] {
        return ssg::CommandResult{
            ssg::CommandError::None, {},
            ssg::ViewAction{ssg::CenterSelection{}}};
    });

    ASSERT_TRUE(commands.dispatch("ok").accepted());
    auto unknown = commands.dispatch("missing");
    ASSERT_EQ(unknown.error, ssg::CommandError::UnknownCommand);
    ASSERT_TRUE(unknown.message.find("missing") != std::string::npos);
    ASSERT_EQ(commands.dispatch("fail").error, ssg::CommandError::HandlerFailed);
    auto thrown = commands.dispatch("throw");
    ASSERT_EQ(thrown.error, ssg::CommandError::HandlerFailed);
    ASSERT_TRUE(thrown.message.find("boom") != std::string::npos);
    auto view = commands.dispatch("view");
    ASSERT_TRUE(view.accepted());
    ASSERT_TRUE(view.viewAction.has_value());
}

TEST(addRejectsDuplicateAndEmptyValuesWithoutMutation) {
    ssg::Commands commands;
    commands.add("present", "Present", success);
    auto rejects = [&commands](std::string id, std::string label,
                                std::function<ssg::CommandResult()> handler) {
        try {
            commands.add(std::move(id), std::move(label), std::move(handler));
        } catch (std::runtime_error const&) {
            return true;
        }
        return false;
    };
    ASSERT_TRUE(rejects("present", "Again", success));
    ASSERT_TRUE(rejects("", "Missing ID", success));
    ASSERT_TRUE(rejects("missing.label", "", success));
    ASSERT_TRUE(rejects("missing.handler", "Missing Handler", {}));
    ASSERT_EQ(commands.all().size(), std::size_t{1});
}

TEST(nestedDispatchIsRefused) {
    ssg::Commands commands;
    ssg::CommandResult nested;
    commands.add("inner", "Inner", success);
    commands.add("outer", "Outer", [&] {
        nested = commands.dispatch("inner");
        return success();
    });

    ASSERT_TRUE(commands.dispatch("outer").accepted());
    ASSERT_EQ(nested.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(nested.message, std::string{ssg::kNestedDispatchRefusal});
}

TEST(replaceIsAtomicAndMayReuseRetiredIds) {
    ssg::Commands commands;
    commands.add("builtin", "Builtin", success);
    commands.add("script.old", "Old", success);
    std::vector<std::string> old{"script.old"};
    ssg::Commands::Replacements replacement;
    replacement.emplace_back("script.old", ssg::Command{"New", success});
    replacement.emplace_back("script.new", ssg::Command{"Newer", success});
    commands.replace(old, std::move(replacement));
    ASSERT_TRUE(commands.find("builtin") != nullptr);
    ASSERT_EQ(commands.find("script.old")->label, std::string{"New"});
    ASSERT_TRUE(commands.find("script.new") != nullptr);

    ssg::Commands::Replacements invalid;
    invalid.emplace_back("builtin", ssg::Command{"Collision", success});
    bool rejected = false;
    try {
        commands.replace(old, std::move(invalid));
    } catch (std::runtime_error const&) {
        rejected = true;
    }
    ASSERT_TRUE(rejected);
    ASSERT_TRUE(commands.find("script.old") != nullptr);
    ASSERT_TRUE(commands.find("script.new") != nullptr);

    ssg::Commands::Replacements duplicates;
    duplicates.emplace_back("script.duplicate",
                            ssg::Command{"First", success});
    duplicates.emplace_back("script.duplicate",
                            ssg::Command{"Second", success});
    rejected = false;
    try {
        commands.replace(old, std::move(duplicates));
    } catch (std::runtime_error const&) {
        rejected = true;
    }
    ASSERT_TRUE(rejected);
    ASSERT_TRUE(commands.find("script.duplicate") == nullptr);
    ASSERT_TRUE(commands.find("script.old") != nullptr);
    ASSERT_TRUE(commands.find("script.new") != nullptr);
}

}  // namespace

SSG_TEST_SUITE(test_command) {
    RUN(addFindAndEnumerateInIdOrder);
    RUN(dispatchReportsSuccessUnknownFailureThrowAndViewAction);
    RUN(addRejectsDuplicateAndEmptyValuesWithoutMutation);
    RUN(nestedDispatchIsRefused);
    RUN(replaceIsAtomicAndMayReuseRetiredIds);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
