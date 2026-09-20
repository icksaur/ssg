#include <ssg/GitDiffIngress.h>

#include <ssg/Editor.h>
#include <ssg/Selection.h>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace ssg {
namespace {

std::vector<GitTreeRecord> gitTreeRecordsFromScan(
    const std::vector<GitDiffFile>& files) {
    std::vector<GitTreeRecord> records;
    records.reserve(files.size());
    for (const auto& file : files) {
        records.push_back(
            {.workspacePath = file.path.generic_string(),
             .label = file.path.generic_string(),
             .status = file.status(),
             .commands = {}});
    }
    return records;
}

std::size_t lineStartOffset(std::string_view text, std::size_t line) {
    std::size_t offset = 0;
    for (std::size_t current = 0; current < line && offset < text.size();
         ++current) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) return text.size();
        offset = newline + 1;
    }
    return offset;
}

} // namespace

GitDiffIngress::GitDiffIngress(Editor& editor,
                               const std::filesystem::path& root,
                               bool enableGitDiffWorker,
                               bool enableFilesystemWatcher)
    : editor{editor},
      worker{root, enableGitDiffWorker, enableFilesystemWatcher} {}

bool GitDiffIngress::drainGitDiffWorker() {
    auto batch = worker.drain();
    bool accepted = batch.watcherAvailabilityChanged || !batch.scans.empty() ||
                    !batch.watchEvents.empty() || batch.fullReconcile;
    bool externalAdvanced = false;
    for (auto& scan : batch.scans) {
        (void)applyGitDiffScanLocked(std::move(scan));
    }
    if (!batch.watchEvents.empty()) {
        externalAdvanced = editor.external.ingest(std::vector<WatchEvent>{
            batch.watchEvents.begin(), batch.watchEvents.end()});
        const bool inventoryChanged = std::any_of(
            batch.watchEvents.begin(), batch.watchEvents.end(),
            [](const WatchEvent& event) {
                return event.kind != WatchEventKind::Modify ||
                       event.path.filename() == ".gitignore";
            });
        if (inventoryChanged) {
            editor.refreshTreeForPublication();
        }
    }
    if (batch.fullReconcile) {
        externalAdvanced |=
            editor.external.reconcileAllOpenDocumentsAgainstDisk();
        editor.refreshTreeForPublication();
    }
    if (externalAdvanced) {
        editor.refreshSyntax();
        for (const auto document : editor.workspace.documents()) {
            (void)editor.updateTabsFor(document);
        }
        editor.screen.refreshExternalModificationPresence(
            editor.externalModificationPresent());
    }
    return accepted;
}

void GitDiffIngress::refreshLiveDiffDocuments(const DiffViewState& diffView) {
    for (auto it = editor.liveDiffDocuments.begin();
         it != editor.liveDiffDocuments.end();) {
        const auto id = DiffFileId{it->first};
        auto file = std::find_if(
            diffView.files.begin(), diffView.files.end(),
            [&](const DiffFileView& candidate) { return candidate.id == id; });
        const auto desired =
            file == diffView.files.end() ? std::string{} : file->currentContent;
        const auto document = it->second;
        const auto* opened = editor.workspace.tryDocument(document);
        if (opened == nullptr) {
            it = editor.liveDiffDocuments.erase(it);
            continue;
        }
        if (opened->snapshot().text == desired) {
            ++it;
            continue;
        }
        auto state = editor.workspace.state(document);
        const auto label =
            state ? state->displayLabel : std::string{"LiveDiff"};
        auto replacement = editor.workspace.openVirtualDocument(
            label, desired, DocumentMode::Diff);
        if (!replacement.accepted() || !replacement.document) {
            continue;
        }
        it->second = *replacement.document;
        editor.ensureDocumentRuntimeState(*replacement.document);
        auto removed = editor.workspace.removeDocument(document);
        if (removed.accepted()) {
            editor.documentRuntimeStates.erase(document.value());
        }
        ++it;
    }
}

DiffIngressResult GitDiffIngress::applyGitDiffScanLocked(GitDiffScan scan) {
    if (scan.revision == 0) {
        currentGitBranch = scan.currentBranch;
        return {};
    }
    if (scan.revision <= lastGitScanRevision) {
        return {DiffIngressError::DiffRejected};
    }
    currentGitBranch = scan.currentBranch;
    auto gitRecords = gitTreeRecordsFromScan(scan.files);
    auto stagedDiff = editor.diff;
    auto stagedFollow = editor.follow;
    std::vector<FollowDiffChange> followChanges;
    followChanges.reserve(scan.files.size() +
                          stagedDiff.viewState().files.size());
    bool mutated = false;
    std::vector<DiffFileId> statusOnlyIds;

    std::uint64_t nextRevision = stagedDiff.viewState().revision + 1;
    const auto nextMutationRevision = [&nextRevision]() {
        const auto current = nextRevision;
        ++nextRevision;
        return current;
    };
    const auto removeDetailedFile =
        [&](const DiffFileId& id) -> DiffIngressResult {
        const auto prior = stagedDiff.file(id);
        if (!prior || !stagedDiff.isGitFile(id)) {
            return {};
        }
        auto removedFile = prior->get();
        auto priorHunks = removedFile.hunks;
        const auto revision = nextMutationRevision();
        const auto removed = stagedDiff.removeFile(id, revision);
        if (!removed.accepted()) {
            return {DiffIngressError::DiffRejected};
        }
        mutated = true;
        removedFile.deleted = true;
        removedFile.currentContent.clear();
        removedFile.hunks.clear();
        removedFile.changedLines.clear();
        followChanges.push_back(
            {std::move(removedFile), std::move(priorHunks), revision});
        return {};
    };

    std::vector<DiffFileId> scannedIds;
    scannedIds.reserve(scan.files.size());
    for (auto& file : scan.files) {
        scannedIds.push_back(file.id);
        std::vector<DiffHunk> priorHunks;
        if (const auto prior = stagedDiff.file(file.id)) {
            priorHunks = prior->get().hunks;
        }
        const auto revision = nextMutationRevision();
        const auto applied = stagedDiff.updateGitFile(
            {.id = file.id,
             .path = file.path,
             .previousPath = file.previousPath,
             .baselineContent = std::move(file.baselineContent),
             .workingContent = std::move(file.workingContent)},
            scan.baselineIdentity, revision);
        if (!applied.accepted()) {
            if (applied.error != DiffError::WorkLimitExceeded) {
                return {DiffIngressError::DiffRejected};
            }
            statusOnlyIds.push_back(file.id);
            if (auto removed = removeDetailedFile(file.id);
                !removed.accepted()) {
                return removed;
            }
            continue;
        }
        const auto changedFile = stagedDiff.file(file.id);
        if (!changedFile) {
            return {DiffIngressError::DiffRejected};
        }
        mutated = true;
        followChanges.push_back(
            {changedFile->get(), std::move(priorHunks), revision});
    }

    const auto stagedView = stagedDiff.viewState();
    for (const auto& file : stagedView.files) {
        if (std::find(scannedIds.begin(), scannedIds.end(), file.id) !=
            scannedIds.end()) {
            continue;
        }
        if (!stagedDiff.isGitFile(file.id)) {
            continue;
        }
        if (auto removed = removeDetailedFile(file.id); !removed.accepted()) {
            return removed;
        }
    }

    if (mutated) {
        const auto followed =
            stagedFollow.acceptExternalChanges(std::move(followChanges));
        if (!followed.accepted()) {
            return {DiffIngressError::FollowRejected};
        }

        const auto previousTarget = editor.follow.viewState().activeTarget;
        editor.diff = std::move(stagedDiff);
        editor.follow = std::move(stagedFollow);
        for (const auto& id : statusOnlyIds) {
            std::optional<TabId> liveTab;
            for (const auto& tab : editor.tabs.viewState().tabs) {
                if (tab.kind == TabKind::LiveDiff &&
                    tab.contentIdentity == id.value()) {
                    liveTab = tab.id;
                    break;
                }
            }
            if (liveTab) {
                const auto tabs = editor.tabs.viewState();
                const auto found = std::find_if(
                    tabs.tabs.begin(), tabs.tabs.end(),
                    [&](const TabState& tab) { return tab.id == *liveTab; });
                if (found != tabs.tabs.end()) {
                    auto outcome = editor.closeTab(*found);
                    (void)editor.tabs.close(*liveTab, std::move(outcome));
                }
            }
        }
        refreshLiveDiffDocuments(editor.diff.viewState());
        const auto next = editor.follow.viewState();
        if (next.mode == FollowMode::Following && next.activeTarget &&
            next.activeTarget != previousTarget) {
            (void)editor.openOrRevealFollowTargetProgrammatic(
                *next.activeTarget);
        }
    }
    editor.tree.replaceProvider(TreeProviderSnapshot::fromGit(
        TreeProviderId{"git"}, std::move(gitRecords)));
    lastGitScanRevision = scan.revision;
    return {};
}

bool GitDiffIngress::revealCurrentDiffTarget(
    const FollowTarget& target, NavigationClass classification) {
    (void)classification;
    const auto& text = editor.activeText();
    const auto offset = lineStartOffset(text, target.newestHunkLine);
    const auto position = resolveSelectionPosition(text, ByteOffset{offset});
    if (!position) {
        return false;
    }
    editor.selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*position, *position}}};
    editor.screen.focusEditor();
    return true;
}

} // namespace ssg
