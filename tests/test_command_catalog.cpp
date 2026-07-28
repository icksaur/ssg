#include <ssg/CommandCatalog.h>

#include "test_helpers.h"

#include <atomic>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

namespace {

ssg::CommandSpecBuilder minimal(std::string id) {
    return ssg::CommandSpecBuilder{std::move(id)}
        .owner("test-owner")
        .summary("a command")
        .observes()
        .handler([](ssg::CommandContext&) {
            return ssg::CommandHandlerResult::success();
        });
}

struct Payload {
    int value = 0;
};

TEST(addIssuesAHandleThatResolvesBackToItsCommand) {
    ssg::CommandCatalog catalog;
    auto const handle = catalog.add(minimal("first.command"));

    ASSERT_TRUE(handle.valid());
    auto const* entry = catalog.find(handle);
    ASSERT_TRUE(entry != nullptr);
    if (entry) ASSERT_EQ(entry->id, std::string{"first.command"});
    ASSERT_TRUE(catalog.find("first.command") == entry);
    ASSERT_TRUE(catalog.handleFor("first.command") == handle);
}

TEST(addRejectsADuplicateIdAndNamesBothOwners) {
    ssg::CommandCatalog catalog;
    catalog.add(minimal("dup.command").owner("first-owner"));

    bool threw = false;
    std::string message;
    try {
        catalog.add(minimal("dup.command").owner("second-owner"));
    } catch (std::runtime_error const& error) {
        threw = true;
        message = error.what();
    }
    ASSERT_TRUE(threw);
    // A collision is a wiring mistake, so the message must identify both
    // claimants -- knowing only the id leaves the author hunting for the other.
    ASSERT_TRUE(message.find("first-owner") != std::string::npos);
    ASSERT_TRUE(message.find("second-owner") != std::string::npos);
    ASSERT_EQ(catalog.size(), std::size_t{1});
}

// Every required field is checked at add, because a builder passed to add is
// finished by definition.  Each omission is checked separately so the test
// fails for the field actually missing.
TEST(addRejectsEachMissingRequiredField) {
    auto const rejects = [](ssg::CommandSpecBuilder spec) {
        ssg::CommandCatalog catalog;
        try {
            catalog.add(std::move(spec));
        } catch (std::runtime_error const&) {
            return true;
        }
        return false;
    };
    auto const handler = [](ssg::CommandContext&) {
        return ssg::CommandHandlerResult::success();
    };

    ASSERT_TRUE(rejects(ssg::CommandSpecBuilder{""}.owner("o").summary("s")
                            .observes().handler(handler)));
    ASSERT_TRUE(rejects(ssg::CommandSpecBuilder{"no.owner"}.summary("s")
                            .observes().handler(handler)));
    ASSERT_TRUE(rejects(ssg::CommandSpecBuilder{"no.summary"}.owner("o")
                            .observes().handler(handler)));
    // Effect has no default: "nobody decided" must not be shippable.
    ASSERT_TRUE(rejects(ssg::CommandSpecBuilder{"no.effect"}.owner("o")
                            .summary("s").handler(handler)));
    ASSERT_TRUE(rejects(ssg::CommandSpecBuilder{"no.handler"}.owner("o")
                            .summary("s").observes()));
    ASSERT_FALSE(rejects(ssg::CommandSpecBuilder{"complete"}.owner("o")
                             .summary("s").observes().handler(handler)));
}

// The catalog is append-only and its storage is stable, so a reference taken
// early survives every later registration.  This is what lets callers hold a
// CommandEntry const* with no lifetime discipline (R4).
TEST(referencesAndHandlesSurviveLaterRegistrations) {
    ssg::CommandCatalog catalog;
    auto const firstHandle = catalog.add(minimal("early.command"));
    auto const* early = catalog.find("early.command");
    ASSERT_TRUE(early != nullptr);

    for (int index = 0; index < 512; ++index) {
        catalog.add(minimal("filler.command" + std::to_string(index)));
    }

    ASSERT_TRUE(catalog.find("early.command") == early);
    ASSERT_TRUE(catalog.find(firstHandle) == early);
    if (early) ASSERT_EQ(early->id, std::string{"early.command"});
    ASSERT_EQ(early->owner, std::string{"test-owner"});
}

TEST(revisionAdvancesOnEveryRegistration) {
    ssg::CommandCatalog catalog;
    auto const initial = catalog.revision();
    catalog.add(minimal("one.command"));
    auto const afterFirst = catalog.revision();
    catalog.add(minimal("two.command"));

    ASSERT_TRUE(afterFirst > initial);
    ASSERT_TRUE(catalog.revision() > afterFirst);
}

TEST(commandsAreEnumeratedInRegistrationOrderAndGroupedByOwner) {
    ssg::CommandCatalog catalog;
    catalog.add(minimal("a.command").owner("alpha"));
    catalog.add(minimal("b.command").owner("beta"));
    catalog.add(minimal("c.command").owner("alpha"));

    std::vector<std::string> ids;
    for (auto const* entry : catalog.commands()) ids.push_back(entry->id);
    ASSERT_EQ(ids, (std::vector<std::string>{"a.command", "b.command",
                                             "c.command"}));

    std::vector<std::string> alpha;
    for (auto const* entry : catalog.ownedBy("alpha")) alpha.push_back(entry->id);
    ASSERT_EQ(alpha, (std::vector<std::string>{"a.command", "c.command"}));
    ASSERT_TRUE(catalog.ownedBy("gamma").empty());
}

// A typed handler records the argument type it consumes, which is what the
// codec is later derived from -- so the type is written once, at the handler.
//
// This checks the type is RECORDED, not that dispatch unwraps it: invoking a
// handler needs a CommandContext, whose constructor is private to EditorSession
// and cannot honestly be fabricated here.  The unwrap is proven end-to-end
// when a migrated command dispatches through a real session (D3).
TEST(aTypedHandlerRecordsTheArgumentTypeItConsumes) {
    ssg::CommandCatalog catalog;
    catalog.add(ssg::CommandSpecBuilder{"typed.command"}
                    .owner("test-owner")
                    .summary("takes a payload")
                    .mutates()
                    .handler<Payload>([](ssg::CommandContext&,
                                         Payload const& payload) {
                        (void)payload;
                        return ssg::CommandHandlerResult::success();
                    }));

    auto const* entry = catalog.find("typed.command");
    ASSERT_TRUE(entry != nullptr);
    if (!entry) return;
    ASSERT_TRUE(entry->argument.type.has_value());
    ASSERT_TRUE(entry->argument.type == std::type_index{typeid(Payload)});
    ASSERT_TRUE(static_cast<bool>(entry->handler));
}

TEST(aCommandWithNoArgumentsDeclaresNoArgumentType) {
    ssg::CommandCatalog catalog;
    catalog.add(minimal("bare.command"));
    auto const* entry = catalog.find("bare.command");
    ASSERT_TRUE(entry != nullptr);
    if (entry) ASSERT_FALSE(entry->argument.type.has_value());
}

TEST(initScriptImpliesLuaApi) {
    ssg::CommandCatalog catalog;
    catalog.add(minimal("granted.command").initScript());
    auto const* entry = catalog.find("granted.command");
    ASSERT_TRUE(entry != nullptr);
    if (!entry) return;
    ASSERT_TRUE(entry->initScript);
    // The startup grant is the narrower axis; it cannot be held without API
    // eligibility, or a host would be asked to expose a command it may not.
    ASSERT_TRUE(entry->luaApi);
}

TEST(unknownIdsAndHandlesResolveToNothing) {
    ssg::CommandCatalog catalog;
    catalog.add(minimal("known.command"));

    ASSERT_TRUE(catalog.find("absent.command") == nullptr);
    ASSERT_FALSE(catalog.handleFor("absent.command").valid());
    ASSERT_TRUE(catalog.find(ssg::CommandHandle{}) == nullptr);
}

// Registration may happen while the editor is running, and protocol decode
// reads the catalog off the session lock on connection threads.  Run under a
// sanitiser build this is the race check; run plain it still asserts that
// concurrent readers observe only whole registrations.
TEST(concurrentReadsSeeOnlyWholeRegistrations) {
    ssg::CommandCatalog catalog;
    catalog.add(minimal("seed.command"));

    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (auto const* entry : catalog.commands()) {
                    if (entry->id.empty() || entry->owner.empty() ||
                        !entry->handler) {
                        ++torn;
                    }
                }
                if (auto const* seed = catalog.find("seed.command");
                    seed == nullptr) {
                    ++torn;
                }
            }
        });
    }
    for (int index = 0; index < 400; ++index) {
        catalog.add(minimal("late.command" + std::to_string(index)));
    }
    stop.store(true);
    for (auto& reader : readers) reader.join();

    ASSERT_EQ(torn.load(), 0);
    ASSERT_EQ(catalog.size(), std::size_t{401});
}

}  // namespace

int main() {
    RUN(addIssuesAHandleThatResolvesBackToItsCommand);
    RUN(addRejectsADuplicateIdAndNamesBothOwners);
    RUN(addRejectsEachMissingRequiredField);
    RUN(referencesAndHandlesSurviveLaterRegistrations);
    RUN(revisionAdvancesOnEveryRegistration);
    RUN(commandsAreEnumeratedInRegistrationOrderAndGroupedByOwner);
    RUN(aTypedHandlerRecordsTheArgumentTypeItConsumes);
    RUN(aCommandWithNoArgumentsDeclaresNoArgumentType);
    RUN(initScriptImpliesLuaApi);
    RUN(unknownIdsAndHandlesResolveToNothing);
    RUN(concurrentReadsSeeOnlyWholeRegistrations);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
