#include <ssg/InitScriptWatcher.h>

#include <ssg/ScriptHost.h>
#include <ssg/platform_files.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

namespace ssg {

namespace {

bool isBlank(std::string const& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char byte) {
        return std::isspace(byte) != 0;
    });
}

std::optional<std::string> readInitScriptIfPresent(
    std::filesystem::path const& scriptPath, bool reportDiagnostics) {
    auto result = ssg::readFile(scriptPath);
    if (result.status == ssg::FileIoStatus::NotFound) return std::nullopt;
    if (!result.ok()) {
        if (reportDiagnostics) {
            std::fprintf(stderr, "ssg: could not read %s: %s\n",
                         scriptPath.string().c_str(), result.message.c_str());
        }
        return std::nullopt;
    }
    std::string text{reinterpret_cast<char const*>(result.bytes.data()),
                     result.bytes.size()};
    if (isBlank(text)) return std::nullopt;
    return text;
}

constexpr std::chrono::milliseconds kInitScriptPollInterval{500};

}  // namespace

void evaluateInitScript(ScriptHost& scripts,
                        std::filesystem::path const& scriptPath,
                        std::string const& script) {
    auto const result = scripts.evaluate(script);
    if (!result.accepted()) {
        std::fprintf(stderr, "ssg: %s: %s\n", scriptPath.string().c_str(),
                     result.message.c_str());
    }
}

std::optional<std::filesystem::path> resolveInitScriptPath() {
    try {
        return ssg::userConfigRoot("ssg") / "init.lua";
    } catch (std::exception const& error) {
        std::fprintf(stderr, "ssg: could not resolve config directory: %s\n",
                     error.what());
        return std::nullopt;
    }
}

std::optional<std::string> loadInitScript(ScriptHost& scripts) {
    auto const scriptPath = resolveInitScriptPath();
    if (!scriptPath) return std::nullopt;
    auto script = readInitScriptIfPresent(*scriptPath, true);
    if (!script) return std::nullopt;
    evaluateInitScript(scripts, *scriptPath, *script);
    return script;
}

InitScriptWatcher::InitScriptWatcher(
    std::filesystem::path scriptPath, std::optional<std::string> alreadyApplied)
    : scriptPath_{std::move(scriptPath)},
      lastApplied_{std::move(alreadyApplied).value_or(std::string{})} {
    thread_ = std::thread([this] { run(); });
}

InitScriptWatcher::~InitScriptWatcher() {
    if (thread_.joinable()) {
        {
            std::lock_guard lock{mutex_};
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
}

void InitScriptWatcher::drainAndEvaluate(ScriptHost& scripts) {
    readiness_.consume();
    std::optional<std::string> pending;
    {
        std::lock_guard lock{mutex_};
        pending = std::move(pendingScript_);
        pendingScript_.reset();
    }
    if (pending) {
        evaluateInitScript(scripts, scriptPath_, *pending);
    }
}

void InitScriptWatcher::run() {
    std::optional<std::string> lastRead;
    std::unique_lock lock{mutex_};
    while (!stop_) {
        lock.unlock();
        auto current = readInitScriptIfPresent(scriptPath_, false);
        lock.lock();
        if (current && lastRead && *current == *lastRead &&
            *current != lastApplied_) {
            lastApplied_ = *current;
            bool const wasEmpty = !pendingScript_.has_value();
            pendingScript_ = *current;
            if (wasEmpty) {
                readiness_.notify();
            }
        } else if (!current) {
            lastApplied_.clear();
        }
        lastRead = current;
        wake_.wait_for(lock, kInitScriptPollInterval,
                       [this] { return stop_; });
    }
}

}  // namespace ssg
