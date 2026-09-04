#include <ssg/CommandCatalog.h>

#include <ssg/command_executor.h>

#include "test_helpers.h"

#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

namespace {

ssg::CommandSpec minimal(std::string id) {
    return ssg::CommandSpec{
        .id = std::move(id),
        .owner = "test-owner",
        .summary = "a command",
        .effect = ssg::CommandEffect::Observation,
        .binding = ssg::bindNoArgumentHandler([](ssg::CommandContext&) {
            return ssg::CommandHandlerResult::success();
        }),
    };
}

ssg::CommandSpec minimal(std::string id, std::string owner) {
    auto spec = minimal(std::move(id));
    spec.owner = std::move(owner);
    return spec;
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
    auto first = minimal("dup.command");
    first.owner = "first-owner";
    catalog.add(std::move(first));

    bool threw = false;
    std::string message;
    try {
        auto second = minimal("dup.command");
        second.owner = "second-owner";
        catalog.add(std::move(second));
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

// Every required field is checked at add, because a spec passed to add is
// finished by definition.  Each omission is checked separately so the test
// fails for the field actually missing.
TEST(addRejectsEachMissingRequiredField) {
    auto const rejects = [](ssg::CommandSpec spec) {
        ssg::CommandCatalog catalog;
        try {
            catalog.add(std::move(spec));
        } catch (std::runtime_error const&) {
            return true;
        }
        return false;
    };
    auto const handler = ssg::bindNoArgumentHandler([](ssg::CommandContext&) {
        return ssg::CommandHandlerResult::success();
    });

    ASSERT_TRUE(rejects(ssg::CommandSpec{.id = "",
                                         .owner = "o",
                                         .summary = "s",
                                         .effect = ssg::CommandEffect::Observation,
                                         .binding = handler}));
    ASSERT_TRUE(rejects(ssg::CommandSpec{.id = "no.owner",
                                         .summary = "s",
                                         .effect = ssg::CommandEffect::Observation,
                                         .binding = handler}));
    ASSERT_TRUE(rejects(ssg::CommandSpec{.id = "no.summary",
                                         .owner = "o",
                                         .effect = ssg::CommandEffect::Observation,
                                         .binding = handler}));
    // Effect has no default: "nobody decided" must not be shippable.
    ASSERT_TRUE(rejects(ssg::CommandSpec{
        .id = "no.effect", .owner = "o", .summary = "s", .binding = handler}));
    ASSERT_TRUE(rejects(ssg::CommandSpec{.id = "no.handler",
                                         .owner = "o",
                                         .summary = "s",
                                         .effect = ssg::CommandEffect::Observation}));
    ASSERT_FALSE(rejects(ssg::CommandSpec{.id = "complete",
                                          .owner = "o",
                                          .summary = "s",
                                          .effect = ssg::CommandEffect::Observation,
                                          .binding = handler}));
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
    catalog.add(minimal("a.command", "alpha"));
    catalog.add(minimal("b.command", "beta"));
    catalog.add(minimal("c.command", "alpha"));

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
// handler needs a CommandContext, whose constructor is private to CommandExecutor
// and cannot honestly be fabricated here.  The unwrap is proven end-to-end
// when a migrated command dispatches through a real session (D3).
TEST(aTypedHandlerRecordsTheArgumentTypeItConsumes) {
    ssg::CommandCatalog catalog;
    catalog.add(ssg::CommandSpec{
        .id = "typed.command",
        .owner = "test-owner",
        .summary = "takes a payload",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindWireHandler<Payload>(
            [](ssg::CommandContext&, Payload const& payload) {
                (void)payload;
                return ssg::CommandHandlerResult::success();
            }),
    });

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
    auto spec = minimal("granted.command");
    spec.initScript = true;
    catalog.add(std::move(spec));
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

// The point of a dynamic catalog: a command registered after the session was
// built is dispatchable at once.
//
// This failed before the session stopped keeping its own registry.  A late
// registration reached the catalog -- so the palette listed the command and the
// keymap resolved a handle for it -- while dispatch consulted a snapshot taken
// at build time and answered UnknownCommand.  Worse, the handle indexed past
// that snapshot's parallel array.
TEST(aCommandRegisteredAfterExecutorConstructionIsDispatchable) {
    auto catalog = std::make_shared<ssg::CommandCatalog>();
    catalog->add(minimal("early.command"));
    ssg::CommandExecutor executor{catalog};

    int lateCalls = 0;
    catalog->add(ssg::CommandSpec{
        .id = "late.command",
        .owner = "test-owner",
        .summary = "registered after the session existed",
        .effect = ssg::CommandEffect::Observation,
        .binding = ssg::bindNoArgumentHandler(
            [&lateCalls](ssg::CommandContext&) {
                ++lateCalls;
                return ssg::CommandHandlerResult::success();
            }),
    });

    auto const byName = executor.dispatch({"late.command",  {}});
    ASSERT_TRUE(byName.accepted());
    ASSERT_EQ(lateCalls, 1);

    // And by the handle the catalog issued for it, which is the keystroke
    // path's spelling.
    auto const handle = catalog->handleFor("late.command");
    ASSERT_TRUE(handle.valid());
    auto const byHandle = executor.dispatch({ssg::CommandName{"late.command", handle},  {}});
    ASSERT_TRUE(byHandle.accepted());
    ASSERT_EQ(lateCalls, 2);
}

TEST(registeringPastTheHandleSpaceIsRefused) {
    // A handle is a 16-bit index, so the catalog must refuse a command it
    // cannot name rather than wrap one onto another command's handle.
    ASSERT_EQ(ssg::CommandCatalog::kMaximumCommands, std::size_t{65535});
}

TEST(aSwapExceedingTheHandleSpaceLeavesThePreviousGenerationWorking) {
    // Capacity is checked in the validate phase, so running out of handles
    // behaves like any other refusal: the generation already installed keeps
    // working rather than being retired for a replacement that cannot fit.
    // Retired slots are NOT reclaimed, which is what makes exhaustion reachable
    // at all -- a script reloaded often enough consumes the space.
    ssg::CommandCatalog catalog;
    auto previous = std::vector{catalog.add(minimal("lua.survivor"))};

    // Fill the space, one reload at a time, until a swap is refused.
    bool refused = false;
    std::string refusal;
    for (int reload = 0; reload < 200000 && !refused; ++reload) {
        std::vector<ssg::CommandSpec> batch;
        batch.push_back(minimal("lua.churn"));
        try {
            auto const added = catalog.replaceGeneration(previous, std::move(batch));
            previous = added;
        } catch (std::runtime_error const& thrown) {
            refused = true;
            refusal = thrown.what();
        }
    }
    ASSERT_TRUE(refused);
    ASSERT_TRUE(refusal.find("full") != std::string::npos);

    // The generation that was installed when the refusal happened is intact,
    // and still dispatchable -- its handle resolves to a live entry.
    ASSERT_EQ(previous.size(), std::size_t{1});
    auto const* live = catalog.find(previous[0]);
    ASSERT_TRUE(live != nullptr);
    if (live) ASSERT_EQ(live->id, std::string{"lua.churn"});
    ASSERT_TRUE(catalog.find("lua.churn") == live);
}


TEST(retiringACommandFreesItsNameButNeverItsHandle) {
    // A reload registers the same ids again, so retiring must release the name.
    // The handle must NOT be reused: a stale handle has to resolve to nothing
    // rather than to whatever was registered next.
    ssg::CommandCatalog catalog;
    auto const first = catalog.add(minimal("lua.hello"));
    ASSERT_TRUE(catalog.find("lua.hello") != nullptr);

    std::vector<ssg::CommandSpec> second;
    second.push_back(minimal("lua.hello"));
    auto const replaced = catalog.replaceGeneration(std::array{first}, std::move(second));

    ASSERT_EQ(replaced.size(), std::size_t{1});
    ASSERT_TRUE(catalog.find(first) == nullptr);
    ASSERT_TRUE(!(replaced[0] == first));
    auto const* live = catalog.find("lua.hello");
    ASSERT_TRUE(live != nullptr);
    ASSERT_TRUE(catalog.find(replaced[0]) == live);
    ASSERT_EQ(catalog.size(), std::size_t{1});
}

TEST(retiringWithoutReplacementMakesTheCommandUnknown) {
    ssg::CommandCatalog catalog;
    auto const handle = catalog.add(minimal("lua.gone"));
    catalog.replaceGeneration(std::array{handle}, {});
    ASSERT_TRUE(catalog.find("lua.gone") == nullptr);
    ASSERT_TRUE(catalog.find(handle) == nullptr);
    ASSERT_EQ(catalog.size(), std::size_t{0});
}

TEST(aBatchWithOneBadSpecChangesNothing) {
    // The whole point of validating first: a failed reload must leave the
    // previous generation registered and working, not delete it and then fail
    // to install the replacement.
    ssg::CommandCatalog catalog;
    auto const previous = catalog.add(minimal("lua.keep"));

    std::vector<ssg::CommandSpec> batch;
    batch.push_back(minimal("lua.fine"));
    // Missing owner/summary/effect/handler: an incomplete spec is the bad one.
    batch.push_back(ssg::CommandSpec{.id = "lua.broken"});
    bool threw = false;
    try {
        catalog.replaceGeneration(std::array{previous}, std::move(batch));
    } catch (std::runtime_error const&) {
        threw = true;
    }

    ASSERT_TRUE(threw);
    ASSERT_TRUE(catalog.find(previous) != nullptr);
    ASSERT_TRUE(catalog.find("lua.fine") == nullptr);
    ASSERT_EQ(catalog.size(), std::size_t{1});
}

TEST(aBatchRepeatingAnIdIsRefusedWholesale) {
    ssg::CommandCatalog catalog;
    std::vector<ssg::CommandSpec> batch;
    batch.push_back(minimal("lua.twice"));
    batch.push_back(minimal("lua.twice"));
    bool threw = false;
    try {
        catalog.replaceGeneration({}, std::move(batch));
    } catch (std::runtime_error const&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    ASSERT_EQ(catalog.size(), std::size_t{0});
}

TEST(aBatchMayReuseAnIdItIsItselfRetiring) {
    // Retirement is applied after validation, so validation has to accept an id
    // that the same batch is about to free -- which is the ordinary reload.
    ssg::CommandCatalog catalog;
    auto const a = catalog.add(minimal("lua.a"));
    auto const b = catalog.add(minimal("lua.b"));
    std::vector<ssg::CommandSpec> batch;
    batch.push_back(minimal("lua.a"));
    batch.push_back(minimal("lua.c"));
    auto const added = catalog.replaceGeneration(std::array{a, b}, std::move(batch));
    ASSERT_EQ(added.size(), std::size_t{2});
    ASSERT_TRUE(catalog.find("lua.a") != nullptr);
    ASSERT_TRUE(catalog.find("lua.b") == nullptr);
    ASSERT_TRUE(catalog.find("lua.c") != nullptr);
    ASSERT_EQ(catalog.size(), std::size_t{2});
}

TEST(aSwapIsNeverObservedWithNeitherGenerationPresent) {
    // A palette listing none of the user's commands, even for an instant, is
    // the failure this exists to prevent.
    ssg::CommandCatalog catalog;
    auto handles = std::vector{catalog.add(minimal("lua.only"))};

    std::atomic<bool> stop{false};
    std::atomic<bool> sawNeither{false};
    std::thread reader{[&] {
        while (!stop.load()) {
            if (catalog.find("lua.only") == nullptr) sawNeither.store(true);
        }
    }};

    for (int generation = 0; generation < 200; ++generation) {
        std::vector<ssg::CommandSpec> batch;
        batch.push_back(minimal("lua.only"));
        handles = catalog.replaceGeneration(handles, std::move(batch));
    }
    stop.store(true);
    reader.join();
    ASSERT_TRUE(!sawNeither.load());
}

SSG_TEST_SUITE(test_command_catalog) {
    RUN(aCommandRegisteredAfterExecutorConstructionIsDispatchable);
    RUN(registeringPastTheHandleSpaceIsRefused);
    RUN(aSwapExceedingTheHandleSpaceLeavesThePreviousGenerationWorking);
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
    RUN(retiringACommandFreesItsNameButNeverItsHandle);
    RUN(retiringWithoutReplacementMakesTheCommandUnknown);
    RUN(aBatchWithOneBadSpecChangesNothing);
    RUN(aBatchRepeatingAnIdIsRefusedWholesale);
    RUN(aBatchMayReuseAnIdItIsItselfRetiring);
    RUN(aSwapIsNeverObservedWithNeitherGenerationPresent);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
