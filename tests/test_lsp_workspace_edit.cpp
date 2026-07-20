#include "fake_lsp_server.h"
#include "test_helpers.h"

#include <ssg/lsp_workspace_edit.h>

#include <fstream>
#include <iterator>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ssg::ByteOffset;
using ssg::LspDocumentSnapshot;
using ssg::LspRenamePublishResult;
using ssg::LspWorkspaceEditApplier;
using ssg::LspWorkspaceEditController;
using ssg::LspWorkspaceEditDocuments;
using ssg::LspWorkspaceEditRecoveryKind;
using ssg::LspWorkspaceFileNode;
using ssg::LspWorkspaceFileNodeKind;
using ssg::LspWorkspaceFileOperations;
using ssg::LspWorkspaceFileResult;
using ssg::LspWorkspaceDocumentWriteResult;
using ssg::LspSyncClient;
using ssg::Revision;
using ssg::test::FakeLspServer;

std::string fixture(std::string_view name) {
    std::ifstream input(std::string{SSG_TEST_SOURCE_DIR} +
                        "/tests/fixtures/lsp/workspace_edits/" +
                        std::string{name});
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::string withId(std::string payload, std::uint64_t id) {
    const auto marker = std::string{"\"id\":2"};
    payload.replace(payload.find(marker), marker.size(),
                    "\"id\":" + std::to_string(id));
    return payload;
}

std::map<std::string, std::string> documentTexts(
    const std::map<std::string, LspDocumentSnapshot>& documents) {
    std::map<std::string, std::string> texts;
    for (const auto& [uri, snapshot] : documents) {
        texts.emplace(uri, snapshot.text);
    }
    return texts;
}

void ready(LspSyncClient& client, FakeLspServer& server,
           std::string text = "symbol") {
    ASSERT_TRUE(client.initialize("file:///workspace").accepted());
    server.queue_payload(ssg::test::response(1, "{\"capabilities\":{}}"));
    ASSERT_TRUE(client.poll().accepted());
    ASSERT_TRUE(client.openDocument("file:///workspace/main.cpp", "cpp",
                                     Revision{1}, std::move(text))
                    .accepted());
}

class FakeDocuments final : public LspWorkspaceEditDocuments {
public:
    std::optional<LspDocumentSnapshot> snapshot(std::string_view uri) const override {
        const auto found = documents.find(std::string{uri});
        return found == documents.end() ? std::nullopt
                                        : std::optional<LspDocumentSnapshot>{
                                              found->second};
    }

    LspWorkspaceDocumentWriteResult apply(std::string uri, Revision expected_revision,
                                          std::string text) override {
        ++apply_calls;
        const auto found = documents.find(uri);
        if (found == documents.end()) {
            return {Revision{0},
                    ssg::LspWorkspaceDocumentError::UnknownDocument,
                    "unknown document"};
        }
        if (found->second.revision != expected_revision) {
            return {found->second.revision,
                    ssg::LspWorkspaceDocumentError::StaleRevision,
                    "stale revision"};
        }
        if (std::find(failing_calls.begin(), failing_calls.end(), apply_calls) !=
            failing_calls.end()) {
            return {found->second.revision,
                    ssg::LspWorkspaceDocumentError::WriteFailed,
                    "injected write failure"};
        }
        found->second.revision = Revision{found->second.revision.value() + 1};
        ++found->second.version;
        found->second.text = std::move(text);
        return {found->second.revision, ssg::LspWorkspaceDocumentError::None,
                {}};
    }

    std::map<std::string, LspDocumentSnapshot> documents;
    std::vector<int> failing_calls;
    int apply_calls = 0;
};

class FakeFiles final : public LspWorkspaceFileOperations {
public:
    LspWorkspaceFileResult snapshot(std::string_view uri,
                                    LspWorkspaceFileNode& node) const override {
        if (directories.contains(std::string{uri})) {
            node = {LspWorkspaceFileNodeKind::Directory, {}};
            return {};
        }
        const auto found = files.find(std::string{uri});
        if (found == files.end()) {
            node = {LspWorkspaceFileNodeKind::Missing, {}};
            return {};
        }
        node = {LspWorkspaceFileNodeKind::File, found->second};
        return {};
    }

    LspWorkspaceFileResult createFile(std::string uri, bool overwrite) override {
        if (shouldFail("create")) {
            return {ssg::LspWorkspaceFileError::IoError,
                    "injected create failure"};
        }
        if (directories.contains(uri)) {
            return {ssg::LspWorkspaceFileError::InvalidOperation,
                    "cannot create file over directory"};
        }
        const auto found = files.find(uri);
        if (found != files.end() && !overwrite) {
            return {ssg::LspWorkspaceFileError::AlreadyExists,
                    "file already exists"};
        }
        files[std::move(uri)] = {};
        return {};
    }

    LspWorkspaceFileResult writeFile(std::string uri,
                                      std::string content) override {
        if (shouldFail("write")) {
            return {ssg::LspWorkspaceFileError::IoError,
                    "injected write failure"};
        }
        if (directories.contains(uri)) {
            return {ssg::LspWorkspaceFileError::InvalidOperation,
                    "cannot write directory"};
        }
        files[std::move(uri)] = std::move(content);
        return {};
    }

    LspWorkspaceFileResult renamePath(std::string old_uri, std::string new_uri,
                                       bool overwrite) override {
        if (shouldFail("rename")) {
            return {ssg::LspWorkspaceFileError::IoError,
                    "injected rename failure"};
        }
        const auto source_file = files.find(old_uri);
        const bool source_directory = directories.contains(old_uri);
        if (source_file == files.end() && !source_directory) {
            return {ssg::LspWorkspaceFileError::NotFound,
                    "rename source is missing"};
        }
        const bool destination_exists =
            files.contains(new_uri) || directories.contains(new_uri);
        if (destination_exists && !overwrite) {
            return {ssg::LspWorkspaceFileError::AlreadyExists,
                    "rename destination already exists"};
        }
        files.erase(new_uri);
        directories.erase(new_uri);
        if (source_directory) {
            directories.erase(old_uri);
            directories.insert(new_uri);
        } else {
            files[new_uri] = source_file->second;
            files.erase(source_file);
        }
        return {};
    }

    LspWorkspaceFileResult deletePath(std::string uri, bool recursive) override {
        if (shouldFail("delete")) {
            return {ssg::LspWorkspaceFileError::IoError,
                    "injected delete failure"};
        }
        if (directories.contains(uri)) {
            if (!recursive) {
                return {ssg::LspWorkspaceFileError::InvalidOperation,
                        "recursive delete required for directory"};
            }
            directories.erase(uri);
            return {};
        }
        if (!files.erase(uri)) {
            return {ssg::LspWorkspaceFileError::NotFound,
                    "delete target is missing"};
        }
        return {};
    }

    LspWorkspaceFileResult restorePath(std::string uri,
                                        const LspWorkspaceFileNode& node) override {
        if (shouldFail("restore")) {
            return {ssg::LspWorkspaceFileError::IoError,
                    "injected restore failure"};
        }
        files.erase(uri);
        directories.erase(uri);
        if (node.kind == LspWorkspaceFileNodeKind::File) {
            files[std::move(uri)] = node.content;
        } else if (node.kind == LspWorkspaceFileNodeKind::Directory) {
            directories.insert(std::move(uri));
        }
        return {};
    }

    bool shouldFail(std::string_view operation) {
        ++operation_calls;
        if (fail_operation == operation && operation_calls == fail_call) {
            return true;
        }
        return false;
    }

    std::map<std::string, std::string> files;
    std::set<std::string> directories;
    std::string fail_operation;
    int fail_call = -1;
    int operation_calls = 0;
};

TEST(commandSetExportsTheSingleNormativeRenameAction) {
    const auto set = ssg::lspWorkspaceEditCommandSet();
    const auto commands = set.descriptors();
    ASSERT_EQ(commands.size(), std::size_t{1});
    ASSERT_EQ(commands[0].id, std::string_view{"rename.symbol"});
    ASSERT_FALSE(commands[0].user_navigation);
}

TEST(unicodePositionFixtureAppliesExpectedEdit) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{7}, 3,
                            "a\xF0\x9F\x98\x80" "b"});
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};

    const auto result = applier.apply(fixture("unicode_edit.json"));

    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"a\xF0\x9F\x99\x82" "b"});
    ASSERT_TRUE(result.recovery.has_value());
    ASSERT_TRUE(applier.recover(*result.recovery).accepted());
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"a\xF0\x9F\x98\x80" "b"});
}

TEST(validationRejectsMalformedRangesBeforeAnyMutation) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{1}, 1,
                            "alpha"});
    documents.documents.emplace(
        "file:///workspace/other.cpp",
        LspDocumentSnapshot{"file:///workspace/other.cpp", Revision{1}, 1,
                            "beta"});
    FakeFiles files;
    files.files["file:///workspace/file.txt"] = "payload";
    LspWorkspaceEditApplier applier{documents, files};
    const auto before_docs = documents.documents;
    const auto before_files = files.files;

    const auto result = applier.apply(fixture("validation_failure.json"));

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::LspWorkspaceEditError::InvalidPosition);
    ASSERT_EQ(documents.documents, before_docs);
    ASSERT_EQ(files.files, before_files);
    ASSERT_EQ(documents.apply_calls, 0);
    ASSERT_EQ(files.operation_calls, 0);
}

TEST(equalPositionInsertionsPreservePayloadOrder) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{1}, 1,
                            "ab"});
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};

    const auto result = applier.apply(
        "{\"changes\":{\"file:///workspace/main.cpp\":[{\"range\":{\"start\":"
        "{\"line\":0,\"character\":1},\"end\":{\"line\":0,\"character\":1}},"
        "\"newText\":\"X\"},{\"range\":{\"start\":{\"line\":0,\"character\":1},"
        "\"end\":{\"line\":0,\"character\":1}},\"newText\":\"Y\"}]}}");

    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"aXYb"});
}

TEST(multiDocumentWriteFailureRollsBackAtomically) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/a.cpp",
        LspDocumentSnapshot{"file:///workspace/a.cpp", Revision{3}, 1,
                            "one"});
    documents.documents.emplace(
        "file:///workspace/b.cpp",
        LspDocumentSnapshot{"file:///workspace/b.cpp", Revision{4}, 1,
                            "two"});
    documents.failing_calls = {2};
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};
    const auto before = documentTexts(documents.documents);

    const auto result = applier.apply(fixture("two_document_edit.json"));

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::LspWorkspaceEditError::ApplyFailed);
    ASSERT_EQ(documentTexts(documents.documents), before);
    ASSERT_FALSE(result.recovery.has_value());
}

TEST(repeatedDocumentEditsRecoverInReverseRevisionOrder) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{1}, 1,
                            "123456"});
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};

    const auto result = applier.apply(
        "{\"documentChanges\":["
        "{\"textDocument\":{\"uri\":\"file:///workspace/main.cpp\","
        "\"version\":null},\"edits\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":3}},"
        "\"newText\":\"AAA\"}]},"
        "{\"textDocument\":{\"uri\":\"file:///workspace/main.cpp\","
        "\"version\":null},\"edits\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":3},\"end\":{\"line\":0,\"character\":6}},"
        "\"newText\":\"BBB\"}]}]}");

    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"AAABBB"});
    ASSERT_TRUE(result.recovery.has_value());
    ASSERT_TRUE(applier.recover(*result.recovery).accepted());
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"123456"});
}

TEST(documentChangesTakePrecedenceOverChanges) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{1}, 1,
                            "old"});
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};

    const auto result = applier.apply(
        "{\"changes\":{\"file:///workspace/main.cpp\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,"
        "\"character\":3}},\"newText\":\"wrong\"}]},"
        "\"documentChanges\":[{\"textDocument\":{\"uri\":"
        "\"file:///workspace/main.cpp\",\"version\":1},\"edits\":[{"
        "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
        "\"line\":0,\"character\":3}},\"newText\":\"right\"}]}]}");

    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"right"});
}

TEST(rollbackFailureSurfacesRecoveryRecordForRetry) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/a.cpp",
        LspDocumentSnapshot{"file:///workspace/a.cpp", Revision{3}, 1,
                            "one"});
    documents.documents.emplace(
        "file:///workspace/b.cpp",
        LspDocumentSnapshot{"file:///workspace/b.cpp", Revision{4}, 1,
                            "two"});
    documents.failing_calls = {2, 3};
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};

    const auto result = applier.apply(fixture("two_document_edit.json"));

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::LspWorkspaceEditError::RollbackFailed);
    ASSERT_TRUE(result.recovery.has_value());
    ASSERT_EQ(result.recovery->operations.size(), std::size_t{1});
    ASSERT_EQ(result.recovery->operations[0].kind,
              LspWorkspaceEditRecoveryKind::DocumentText);

    documents.failing_calls.clear();
    ASSERT_TRUE(applier.recover(*result.recovery).accepted());
    ASSERT_EQ(documents.documents["file:///workspace/a.cpp"].text,
              std::string{"one"});
}

TEST(fileOperationFailureRollsBackDocumentsAndPaths) {
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{1}, 1,
                            "hello"});
    FakeFiles files;
    files.files["file:///workspace/old.txt"] = "old";
    files.files["file:///workspace/existing.txt"] = "existing";
    files.fail_operation = "rename";
    files.fail_call = 2;
    LspWorkspaceEditApplier applier{documents, files};
    const auto before_docs = documentTexts(documents.documents);
    const auto before_files = files.files;

    const auto result = applier.apply(fixture("file_ops_with_text_edit.json"));

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::LspWorkspaceEditError::ApplyFailed);
    ASSERT_EQ(documentTexts(documents.documents), before_docs);
    ASSERT_EQ(files.files, before_files);
}

TEST(directoryResourceOperationsApplyAndRecover) {
    FakeDocuments documents;
    FakeFiles files;
    files.directories.insert("file:///workspace/dirA");
    files.directories.insert("file:///workspace/dirC");
    LspWorkspaceEditApplier applier{documents, files};

    const auto result = applier.apply(
        "{\"documentChanges\":[{\"kind\":\"rename\",\"oldUri\":"
        "\"file:///workspace/dirA\",\"newUri\":\"file:///workspace/dirB\","
        "\"options\":{\"overwrite\":false}},{\"kind\":\"delete\",\"uri\":"
        "\"file:///workspace/dirC\",\"options\":{\"recursive\":true}}]}");

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(files.directories.contains("file:///workspace/dirB"));
    ASSERT_FALSE(files.directories.contains("file:///workspace/dirA"));
    ASSERT_FALSE(files.directories.contains("file:///workspace/dirC"));
    ASSERT_TRUE(result.recovery.has_value());
    ASSERT_TRUE(applier.recover(*result.recovery).accepted());
    ASSERT_TRUE(files.directories.contains("file:///workspace/dirA"));
    ASSERT_TRUE(files.directories.contains("file:///workspace/dirC"));
    ASSERT_FALSE(files.directories.contains("file:///workspace/dirB"));
}

TEST(renameRequestsUseSyncedUtf16PositionsAndApplyWorkspaceEdits) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server, "a\xF0\x9F\x98\x80" "b");
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{1}, 1,
                            "a\xF0\x9F\x98\x80" "b"});
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};
    LspWorkspaceEditController controller{client, applier};

    const auto request = controller.requestRename(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{5}, "renamed");
    ASSERT_TRUE(request.accepted());
    const auto& payload = server.received_payloads().back();
    ASSERT_TRUE(payload.find("\"method\":\"textDocument/rename\"") !=
                std::string::npos);
    ASSERT_TRUE(payload.find("\"line\":0,\"character\":3") !=
                std::string::npos);
    ASSERT_TRUE(payload.find("\"newName\":\"renamed\"") !=
                std::string::npos);

    server.queue_payload(withId(fixture("rename_response.json"),
                                 request.request_id));
    const auto published = controller.poll(Revision{1});

    ASSERT_TRUE(published.accepted());
    ASSERT_EQ(published.publications.size(), std::size_t{1});
    ASSERT_EQ(published.publications[0].result,
              LspRenamePublishResult::Accepted);
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"renamed"});
}

TEST(supersededRenameResponseCannotReplaceNewerResult) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server, "symbol");
    FakeDocuments documents;
    documents.documents.emplace(
        "file:///workspace/main.cpp",
        LspDocumentSnapshot{"file:///workspace/main.cpp", Revision{1}, 1,
                            "symbol"});
    FakeFiles files;
    LspWorkspaceEditApplier applier{documents, files};
    LspWorkspaceEditController controller{client, applier};

    const auto first = controller.requestRename(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0}, "old");
    const auto second = controller.requestRename(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0}, "new");
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());

    server.queue_payload(withId(fixture("rename_response.json"),
                                 first.request_id));
    const auto stale = controller.poll(Revision{1});
    ASSERT_EQ(stale.publications[0].result,
              LspRenamePublishResult::Superseded);
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"symbol"});

    server.queue_payload(withId(fixture("rename_response_second.json"),
                                 second.request_id));
    const auto accepted = controller.poll(Revision{1});
    ASSERT_EQ(accepted.publications[0].result,
              LspRenamePublishResult::Accepted);
    ASSERT_EQ(documents.documents["file:///workspace/main.cpp"].text,
              std::string{"new"});
}

} // namespace

int main() {
    RUN(commandSetExportsTheSingleNormativeRenameAction);
    RUN(unicodePositionFixtureAppliesExpectedEdit);
    RUN(validationRejectsMalformedRangesBeforeAnyMutation);
    RUN(equalPositionInsertionsPreservePayloadOrder);
    RUN(multiDocumentWriteFailureRollsBackAtomically);
    RUN(rollbackFailureSurfacesRecoveryRecordForRetry);
    RUN(fileOperationFailureRollsBackDocumentsAndPaths);
    RUN(directoryResourceOperationsApplyAndRecover);
    RUN(renameRequestsUseSyncedUtf16PositionsAndApplyWorkspaceEdits);
    RUN(supersededRenameResponseCannotReplaceNewerResult);
    return failed == 0 ? 0 : 1;
}
