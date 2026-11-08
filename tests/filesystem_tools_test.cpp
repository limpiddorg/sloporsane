#include <agent-runtime/core/process.hpp>
#include <agent-runtime/core/ripgrep.hpp>
#include <agent-runtime/tools/fs/common.hpp>
#include <agent-runtime/tools/fs/glob.hpp>
#include <agent-runtime/tools/fs/grep.hpp>
#include <agent-runtime/tools/fs/write.hpp>
#include <agent-runtime/tools/fs/edit.hpp>
#include <agent-runtime/tools/fs/apply_patch.hpp>

#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <future>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;

struct Environment {
    std::string name;
    std::optional<std::string> before;
    Environment(std::string name, const std::string& value) : name(std::move(name)) {
        if (const auto* current = std::getenv(this->name.c_str())) before = current;
        setenv(this->name.c_str(), value.c_str(), 1);
    }
    ~Environment() { if (before) setenv(name.c_str(), before->c_str(), 1); else unsetenv(name.c_str()); }
};

class FilesystemToolsTest : public testing::Test {
protected:
    fs::path root;
    ToolContext context;
    std::vector<ToolPermissionRequest> permissions;
    void SetUp() override {
        auto pattern = (fs::temp_directory_path() / "sos-tools-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = fs::canonical(pattern);
        fs::create_directory(root / "workspace");
        context = {root / "workspace", root / "workspace"};
        context.ask = [this](const auto& request) { permissions.push_back(request); };
    }
    void TearDown() override { std::error_code error; fs::remove_all(root, error); }
    void put(const std::string& name, const std::string& text) { fs_tools::writeText(context.directory / name, text); }
    std::string get(const std::string& name) { return fs_tools::readText(context.directory / name); }
    fs::path rg() {
        return core::ensureRipgrep(root / "bin", [](const std::string&) -> std::string {
            throw std::runtime_error("Tests require rg on PATH; network download is disabled");
        });
    }
};

TEST_F(FilesystemToolsTest, WritesParentsOverwritesAndPreservesBom) {
    WriteTool tool;
    tool.execute({{"filePath", "nested/file.txt"}, {"content", "\xef\xbb\xbfhello"}}, context);
    tool.execute({{"filePath", "nested/file.txt"}, {"content", "updated"}}, context);
    EXPECT_EQ(get("nested/file.txt"), "\xef\xbb\xbfupdated");
    ASSERT_EQ(permissions.size(), 2);
    EXPECT_EQ(permissions[1].permission, "edit");
    EXPECT_NE(permissions[1].metadata["diff"].get<std::string>().find("+"), std::string::npos);
}

TEST_F(FilesystemToolsTest, ExplainedEditingToolsRequireTextBeforePermissionsAndCaptureItWithTheMutation) {
    WriteTool write(true); EditTool edit(true); ApplyPatchTool patch(true);
    for (const Tool* tool : std::vector<const Tool*>{&write, &edit, &patch}) {
        const auto schema = tool->getDefinition().parameters;
        EXPECT_NE(std::find(schema["required"].begin(), schema["required"].end(), "explanation"), schema["required"].end());
        for (const auto& invalid : {json::object(), json{{"explanation", "  \n"}}, json{{"explanation", 42}}})
            EXPECT_THROW(tool->execute(invalid, context), std::invalid_argument);
    }
    EXPECT_TRUE(permissions.empty());
    std::vector<std::string> explanations;
    context.mutateFiles = [&](const auto&, const auto& action, const auto& explanation) {
        explanations.push_back(explanation); action();
    };
    write.execute({{"filePath", "file"}, {"content", "first\n"}, {"explanation", "Create the model"}}, context);
    edit.execute({{"filePath", "file"}, {"oldString", "first"}, {"newString", "second"}, {"explanation", "Complete corrected model"}}, context);
    patch.execute({{"patchText", "*** Begin Patch\n*** Update File: file\n@@\n-second\n+third\n*** End Patch"},
        {"explanation", "Complete final model"}}, context);
    EXPECT_EQ(explanations, (std::vector<std::string>{"Create the model", "Complete corrected model", "Complete final model"}));
    EXPECT_EQ(get("file"), "third\n");
    EXPECT_FALSE(WriteTool{}.getDefinition().parameters["properties"].contains("explanation"));
}

TEST_F(FilesystemToolsTest, EditRejectsAmbiguityAndSupportsReplaceAll) {
    put("file", "hello\nhello\n");
    EditTool tool;
    json args{{"filePath", "file"}, {"oldString", "hello"}, {"newString", "bye"}};
    EXPECT_THROW(tool.execute(args, context), std::runtime_error);
    EXPECT_TRUE(permissions.empty());
    EXPECT_EQ(get("file"), "hello\nhello\n");
    args["replaceAll"] = true;
    tool.execute(args, context);
    EXPECT_EQ(get("file"), "bye\nbye\n");
    args["oldString"] = "";
    EXPECT_THROW(tool.execute(args, context), std::invalid_argument);
    args["filePath"] = "new/file";
    tool.execute(args, context);
    EXPECT_EQ(get("new/file"), "bye");
}

TEST_F(FilesystemToolsTest, EditKeepsCrLfBomAndHandlesLineWhitespace) {
    put("file", "\xef\xbb\xbf  first\r\n  second\r\n");
    EditTool{}.execute({{"filePath", "file"}, {"oldString", "first\nsecond"}, {"newString", "changed"}}, context);
    EXPECT_EQ(get("file"), "\xef\xbb\xbf" "changed\r\n");
}

TEST_F(FilesystemToolsTest, EditingToolsShareTargetedPermissionAndResultDiffs) {
    std::string before;
    for (int i = 1; i <= 30; ++i) before += "line " + std::to_string(i) + '\n';
    auto after = before;
    after.replace(after.find("line 15\n"), 8, "changed\n");
    const auto path = (context.directory / "file").string();
    const auto expected = "--- " + path + "\n+++ " + path +
        "\n@@ -12,7 +12,7 @@\n line 12\n line 13\n line 14\n-line 15\n+changed\n line 16\n line 17\n line 18\n";
    WriteTool write;
    EditTool edit;
    ApplyPatchTool patch;
    const std::vector<std::pair<const Tool*, json>> operations{
        {&write, {{"filePath", "file"}, {"content", after}}},
        {&edit, {{"filePath", "file"}, {"oldString", "line 15"}, {"newString", "changed"}}},
        {&patch, {{"patchText", "*** Begin Patch\n*** Update File: file\n@@\n-line 15\n+changed\n*** End Patch"}}},
    };
    context.ask = [&](const ToolPermissionRequest& request) {
        EXPECT_EQ(request.permission, "edit");
        EXPECT_EQ(request.metadata.at("diff"), expected);
        EXPECT_EQ(get("file"), before); // Approval still precedes the write.
        permissions.push_back(request);
    };
    for (const auto& [tool, arguments] : operations) {
        put("file", before);
        permissions.clear();
        const auto result = tool->execute(arguments, context);
        ASSERT_EQ(permissions.size(), 1);
        EXPECT_EQ(get("file"), after);
        EXPECT_EQ(result.metadata.at("diff"), expected);
        if (result.metadata.contains("files")) EXPECT_EQ(result.metadata["files"][0]["patch"], expected);
    }
}

TEST_F(FilesystemToolsTest, PermissionsPreventWritesAndDetectChangesWhileWaiting) {
    put("file", "original");
    context.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    EXPECT_THROW(WriteTool{}.execute({{"filePath", "new"}, {"content", "x"}}, context), std::runtime_error);
    EXPECT_FALSE(fs::exists(context.directory / "new"));
    EXPECT_THROW(EditTool{}.execute({{"filePath", "file"}, {"oldString", "original"}, {"newString", "x"}}, context), std::runtime_error);
    EXPECT_EQ(get("file"), "original");
    context.ask = [this](const auto&) { put("file", "human update"); };
    EXPECT_THROW(WriteTool{}.execute({{"filePath", "file"}, {"content", "agent update"}}, context), std::runtime_error);
    EXPECT_EQ(get("file"), "human update");
}

TEST_F(FilesystemToolsTest, ApprovalWaitDoesNotHoldTheMutationLockAndConcurrentChangesAreRejected) {
    WriteTool write;
    EditTool edit;
    ApplyPatchTool patch;
    const std::vector<std::pair<const Tool*, json>> operations{
        {&write, {{"filePath", "file"}, {"content", "new\n"}}},
        {&edit, {{"filePath", "file"}, {"oldString", "old"}, {"newString", "new"}}},
        {&patch, {{"patchText", "*** Begin Patch\n*** Update File: file\n@@\n-old\n+new\n*** End Patch"}}},
    };
    for (const auto& [tool, arguments] : operations) {
        put("file", "old\n");
        auto independent = context;
        independent.ask = [](const auto&) {};
        std::future<ToolResult> concurrent;
        context.ask = [&](const ToolPermissionRequest& request) {
            if (request.permission != "edit") return;
            concurrent = std::async(std::launch::async, [&] {
                return WriteTool{}.execute({{"filePath", "file"}, {"content", "concurrent\n"}}, independent);
            });
            EXPECT_EQ(concurrent.wait_for(std::chrono::seconds(1)), std::future_status::ready);
        };
        EXPECT_THROW(tool->execute(arguments, context), std::runtime_error);
        if (concurrent.valid()) concurrent.get();
        EXPECT_EQ(get("file"), "concurrent\n");
    }
}

TEST_F(FilesystemToolsTest, PatchAddsUpdatesMovesAndDeletes) {
    put("old.txt", "before\nkeep\n");
    put("delete.txt", "obsolete\n");
    const std::string patch = "*** Begin Patch\n*** Add File: sub/new.txt\n+created\n"
        "*** Update File: old.txt\n*** Move to: moved.txt\n@@\n-before\n+after\n keep\n"
        "*** Delete File: delete.txt\n*** End Patch";
    const auto result = ApplyPatchTool{}.execute({{"patchText", patch}}, context);
    EXPECT_EQ(get("sub/new.txt"), "created\n");
    EXPECT_EQ(get("moved.txt"), "after\nkeep\n");
    EXPECT_FALSE(fs::exists(context.directory / "old.txt"));
    EXPECT_FALSE(fs::exists(context.directory / "delete.txt"));
    EXPECT_EQ(result.metadata["files"].size(), 3);
    EXPECT_EQ(permissions.back().permission, "edit");
}

TEST_F(FilesystemToolsTest, PatchSupportsAnchorsMultipleHunksAndEof) {
    put("file", "header\nfirst\nmiddle\nlast\n");
    ApplyPatchTool{}.execute({{"patchText", "*** Begin Patch\n*** Update File: file\n@@ header\n-first\n+one\n"
        "@@\n-last\n+end\n*** End of File\n*** End Patch"}}, context);
    EXPECT_EQ(get("file"), "header\none\nmiddle\nend\n");
}

TEST_F(FilesystemToolsTest, InvalidPatchDoesNotPartiallyApplyAndDenialDoesNotWrite) {
    put("file", "original\n");
    const auto patch = "*** Begin Patch\n*** Add File: new\n+new\n*** Update File: file\n@@\n-wrong\n+next\n*** End Patch";
    EXPECT_THROW(ApplyPatchTool{}.execute({{"patchText", patch}}, context), std::runtime_error);
    EXPECT_FALSE(fs::exists(context.directory / "new"));
    EXPECT_EQ(get("file"), "original\n");
    context.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    EXPECT_THROW(ApplyPatchTool{}.execute({{"patchText", "*** Begin Patch\n*** Delete File: file\n*** End Patch"}}, context), std::runtime_error);
    EXPECT_TRUE(fs::exists(context.directory / "file"));
}

TEST_F(FilesystemToolsTest, PatchRejectsExistingDestinationsAndRepeatedPaths) {
    put("file", "x\n");
    EXPECT_THROW(ApplyPatchTool{}.execute({{"patchText", "*** Begin Patch\n*** Add File: file\n+x\n*** End Patch"}}, context), std::runtime_error);
    EXPECT_THROW(ApplyPatchTool{}.execute({{"patchText", "*** Begin Patch\n*** Delete File: file\n*** Add File: file\n+x\n*** End Patch"}}, context), std::invalid_argument);
    EXPECT_EQ(get("file"), "x\n");
}

TEST_F(FilesystemToolsTest, ExternalAndSymlinkTargetsRequestPermission) {
    fs_tools::writeText(root / "outside", "x");
    fs::create_symlink(root / "outside", context.directory / "link");
    context.ask = [](const ToolPermissionRequest& request) {
        if (request.permission == "external_directory") throw std::runtime_error("Outside denied");
    };
    EXPECT_THROW(WriteTool{}.execute({{"filePath", "link"}, {"content", "changed"}}, context), std::runtime_error);
    EXPECT_EQ(fs_tools::readText(root / "outside"), "x");
}

TEST_F(FilesystemToolsTest, GlobAndGrepSearchWithFiltersAndLiteralArguments) {
    const auto binary = rg();
    put(".gitignore", "ignored/\n");
    fs::create_directory(context.directory / ".git");
    put("src/a.cpp", "needle\nother\nneedle\n");
    put("src/b.hpp", "needle\n");
    put("ignored/hidden.cpp", "needle\n");
    GlobTool glob(binary);
    GrepTool grep(binary);
    const auto found = glob.execute({{"pattern", "**/*.cpp"}}, context);
    EXPECT_EQ(found.metadata["count"], 1);
    EXPECT_NE(found.output.find("src/a.cpp"), std::string::npos);
    EXPECT_EQ(grep.execute({{"pattern", "needle"}, {"include", "*.cpp"}}, context).metadata["matches"], 2);
    EXPECT_EQ(grep.execute({{"pattern", "needle"}, {"path", "src/b.hpp"}}, context).metadata["matches"], 1);
    EXPECT_EQ(grep.execute({{"pattern", "-literal; touch bad"}}, context).metadata["matches"], 0);
    EXPECT_FALSE(fs::exists(context.directory / "bad"));
    EXPECT_THROW(grep.execute({{"pattern", "["}}, context), std::runtime_error);
    EXPECT_EQ(glob.execute({{"pattern", "**/*.missing"}}, context).output, "No files found");
}

TEST_F(FilesystemToolsTest, SearchCapsResultsAt100) {
    const auto binary = rg();
    for (int i = 0; i < 105; ++i) put("files/" + std::to_string(i), "match\n");
    const auto glob = GlobTool(binary).execute({{"pattern", "files/*"}}, context);
    EXPECT_EQ(glob.metadata["count"], 100);
    EXPECT_EQ(glob.metadata["truncated"], true);
    const auto grep = GrepTool(binary).execute({{"pattern", "match"}}, context);
    EXPECT_EQ(grep.metadata["matches"], 100);
    EXPECT_EQ(grep.metadata["truncated"], true);
}

TEST_F(FilesystemToolsTest, ProcessUsesArgvAndPropagatesCancellation) {
    const auto result = core::runProcess("/usr/bin/printf", {"%s", "$(touch bad);quoted path"}, context.directory);
    EXPECT_EQ(result.output, "$(touch bad);quoted path");
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_FALSE(fs::exists(context.directory / "bad"));
    int checks = 0;
    EXPECT_THROW(core::runProcess("/bin/sleep", {"10"}, context.directory, [&] {
        if (++checks > 1) throw std::runtime_error("Cancelled");
    }), std::runtime_error);
}

TEST_F(FilesystemToolsTest, RipgrepPrefersPathThenCachedBinary) {
    const auto path = root / "path";
    const auto bin = root / "bin";
    fs::create_directory(path);
    fs::create_directory(bin);
    fs_tools::writeText(path / "rg", "#!/bin/sh\nexit 0\n");
    fs_tools::writeText(bin / "rg", "#!/bin/sh\nexit 0\n");
    fs::permissions(path / "rg", fs::perms::owner_all);
    fs::permissions(bin / "rg", fs::perms::owner_all);
    Environment env("PATH", path.string());
    EXPECT_EQ(core::ensureRipgrep(bin), path / "rg");
    fs::remove(path / "rg");
    EXPECT_EQ(core::ensureRipgrep(bin), bin / "rg");
}

TEST_F(FilesystemToolsTest, RipgrepInstallsArchiveOnceWithoutNetwork) {
    const auto search = root / "path";
    fs::create_directory(search);
    // GNU tar may invoke gzip via PATH; keep that available but deliberately omit rg.
    if (fs::exists("/usr/bin/gzip")) fs::create_symlink("/usr/bin/gzip", search / "gzip");
    Environment env("PATH", search.string());
    int downloads = 0;
    const auto download = [&](const std::string& url) {
        ++downloads;
        EXPECT_NE(url.find("https://github.com/BurntSushi/ripgrep/releases/download/15.1.0/"), std::string::npos);
        const auto name = url.substr(url.find_last_of('/') + 1);
        const auto folder = name.substr(0, name.size() - std::string(".tar.gz").size());
        const auto source = root / "archive-source";
        fs::create_directories(source / folder);
        fs_tools::writeText(source / folder / "rg", "#!/bin/sh\nexit 0\n");
        const auto archive = root / "fixture.tar.gz";
        const auto packed = core::runProcess("/usr/bin/tar", {"-czf", archive.string(), "-C", source.string(), folder}, root);
        if (packed.exitCode != 0) throw std::runtime_error(packed.error);
        std::ifstream file(archive, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto installed = core::ensureRipgrep(root / "installed", download);
    EXPECT_EQ(installed, root / "installed/rg");
    EXPECT_EQ(core::runProcess(installed, {"--version"}, root).exitCode, 0);
    EXPECT_EQ(core::ensureRipgrep(root / "installed", download), installed);
    EXPECT_EQ(downloads, 1);
    EXPECT_EQ(std::distance(fs::directory_iterator(root / "installed"), fs::directory_iterator{}), 1);
    EXPECT_THROW(core::ensureRipgrep(root / "failed", [](const std::string&) { return "invalid archive"; }), std::runtime_error);
    EXPECT_FALSE(fs::exists(root / "failed/rg"));
}
}
