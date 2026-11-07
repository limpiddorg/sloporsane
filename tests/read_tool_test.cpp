#include <agent-runtime/tools/fs/read.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <thread>
#include <agent-runtime/tools/registry.hpp>
#include <agent-runtime/agent.hpp>

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;

class ReadToolTest : public testing::Test {
protected:
    fs::path root;
    ToolContext context;
    ReadTool tool;
    std::vector<ToolPermissionRequest> permissions;

    void SetUp() override {
        static std::atomic<unsigned> sequence = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("sos-read-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
        ASSERT_TRUE(fs::create_directory(root));
        fs::create_directory(root / "workspace");
        context.directory = fs::canonical(root / "workspace");
        context.worktree = context.directory;
        context.ask = [this](const auto& request) { permissions.push_back(request); };
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    void write(const std::string& name, const std::string& content) {
        const auto path = context.directory / name;
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        ASSERT_TRUE(file.good());
    }
};

TEST_F(ReadToolTest, ReadsNumberedLinesAndUnterminatedLastLine) {
    write("file.txt", "first\r\nsecond\nlast");
    const auto result = tool.execute({{"filePath", "file.txt"}}, context);
    EXPECT_NE(result.output.find("1: first\n2: second\n3: last"), std::string::npos);
    EXPECT_EQ(result.title, "file.txt");
    EXPECT_EQ(result.metadata["display"]["totalLines"], 3);
    EXPECT_EQ(result.metadata["truncated"], false);
    ASSERT_EQ(permissions.size(), 1);
    EXPECT_EQ(permissions[0].permission, "read");
    EXPECT_EQ(permissions[0].patterns, (std::vector<std::string>{"file.txt"}));
}

TEST_F(ReadToolTest, SupportsPaginationAndAbsolutePaths) {
    write("file.txt", "a\nb\nc\nd\n");
    const auto result = tool.execute({{"filePath", (context.directory / "file.txt").string()}, {"offset", 2}, {"limit", 2}}, context);
    EXPECT_EQ(result.metadata["display"]["text"], "b\nc");
    EXPECT_EQ(result.metadata["display"]["totalLines"], 4);
    EXPECT_EQ(result.metadata["truncated"], true);
    EXPECT_NE(result.output.find("offset=4"), std::string::npos);
    EXPECT_THROW(tool.execute({{"filePath", "file.txt"}, {"offset", 5}}, context), std::out_of_range);
}

TEST_F(ReadToolTest, HandlesEmptyFilesAndZeroRangesLikeOpenCode) {
    write("empty", "");
    EXPECT_EQ(tool.execute({{"filePath", "empty"}}, context).metadata["display"]["totalLines"], 0);
    write("file.txt", "a\nb\n");
    EXPECT_EQ(tool.execute({{"filePath", "file.txt"}, {"offset", 0}}, context).metadata["display"]["lineStart"], 1);
    const auto zero = tool.execute({{"filePath", "file.txt"}, {"limit", 0}}, context);
    EXPECT_EQ(zero.metadata["display"]["text"], "");
    EXPECT_EQ(zero.metadata["truncated"], true);
}

TEST_F(ReadToolTest, ListsSortedDirectoryEntriesWithPagination) {
    write("folder/z.txt", "z");
    write("folder/a.txt", "a");
    fs::create_directory(context.directory / "folder/sub");
    const auto result = tool.execute({{"filePath", "folder"}, {"offset", 2}, {"limit", 1}}, context);
    EXPECT_EQ(result.metadata["display"]["entries"], json::array({"sub/"}));
    EXPECT_EQ(result.metadata["display"]["totalEntries"], 3);
    EXPECT_EQ(result.metadata["truncated"], true);
}

TEST_F(ReadToolTest, CapsLinesAndOutputBytes) {
    std::string content;
    for (int i = 0; i < 2001; ++i) content += "x\n";
    write("lines.txt", content);
    const auto lines = tool.execute({{"filePath", "lines.txt"}}, context);
    EXPECT_EQ(lines.metadata["display"]["lineEnd"], 2000);
    EXPECT_EQ(lines.metadata["truncated"], true);
    content.clear();
    for (int i = 0; i < 100; ++i) content += std::string(2000, 'x') + '\n';
    write("bytes.txt", content);
    const auto bytes = tool.execute({{"filePath", "bytes.txt"}}, context);
    EXPECT_EQ(bytes.metadata["display"]["lineEnd"], 25);
    EXPECT_NE(bytes.output.find("Output capped at 50 KB"), std::string::npos);
}

TEST_F(ReadToolTest, TruncatesLongUtf8LinesWithoutSplittingCharacters) {
    std::string text;
    for (int i = 0; i < 2001; ++i) text += "é";
    write("utf8.txt", text);
    const auto result = tool.execute({{"filePath", "utf8.txt"}}, context);
    EXPECT_EQ(result.metadata["display"]["text"], text.substr(0, 4000) + "... (line truncated to 2000 chars)");
    EXPECT_NO_THROW(result.metadata.dump());
}

TEST_F(ReadToolTest, RejectsInvalidArgumentsAndBinaryFiles) {
    for (const json& value : {json(-1), json(1.5), json("2"), json(nullptr)}) {
        EXPECT_THROW(tool.execute({{"filePath", "file"}, {"offset", value}}, context), std::invalid_argument);
    }
    EXPECT_THROW(tool.execute(json::object(), context), json::out_of_range);
    write("binary", std::string("a\0b", 3));
    EXPECT_THROW(tool.execute({{"filePath", "binary"}}, context), std::runtime_error);
    write("object.o", "printable");
    EXPECT_THROW(tool.execute({{"filePath", "object.o"}}, context), std::runtime_error);
}

TEST_F(ReadToolTest, ReportsMissingFilesWithSuggestions) {
    write("agent.hpp", "class Agent;");
    try {
        tool.execute({{"filePath", "agent"}}, context);
        FAIL() << "Expected missing file";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("agent.hpp"), std::string::npos);
    }
}

TEST_F(ReadToolTest, ReturnsPdfAndImageAsBase64Attachments) {
    write("document.pdf", "%PDF-1.7\n");
    const auto pdf = tool.execute({{"filePath", "document.pdf"}}, context);
    ASSERT_EQ(pdf.attachments.size(), 1);
    EXPECT_EQ(pdf.attachments[0].mime, "application/pdf");
    EXPECT_EQ(pdf.attachments[0].url, "data:application/pdf;base64,JVBERi0xLjcK");
    write("image.gif", "GIF89a");
    const auto image = tool.execute({{"filePath", "image.gif"}}, context);
    ASSERT_EQ(image.attachments.size(), 1);
    EXPECT_EQ(image.attachments[0].url, "data:image/gif;base64,R0lGODlh");
}

TEST_F(ReadToolTest, RequestsExternalDirectoryPermissionForSymlinks) {
    write("../outside.txt", "outside");
    fs::create_symlink(root / "outside.txt", context.directory / "link");
    const auto result = tool.execute({{"filePath", "link"}}, context);
    ASSERT_EQ(permissions.size(), 2);
    EXPECT_EQ(permissions[0].permission, "external_directory");
    EXPECT_EQ(permissions[1].permission, "read");
    EXPECT_EQ(result.metadata["display"]["text"], "outside");
}

TEST_F(ReadToolTest, HonorsDenialAndCancellation) {
    write("file.txt", "secret");
    context.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    EXPECT_THROW(tool.execute({{"filePath", "file.txt"}}, context), std::runtime_error);
    context.ask = {};
    EXPECT_THROW(tool.execute({{"filePath", "file.txt"}}, context), std::invalid_argument);
    context.checkCancelled = [] { throw std::runtime_error("Cancelled"); };
    EXPECT_THROW(tool.execute({{"filePath", "file.txt"}}, context), std::runtime_error);
}

class ScriptedProvider : public Provider {
public:
    std::vector<json> responses;
    std::vector<json> inputs;
    std::vector<std::vector<ToolDefinition>> definitions;

    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    std::optional<std::uint64_t> getContextLimit(const std::string&) override { return 10000; }
    ModelInfo getModelInfo(const std::string& model) override {
        return {model, 10000, {}, {}, {}, ImageSupport::SUPPORTED};
    }
    std::optional<std::uint64_t> getUsedContext(const json&) const override { return inputs.size() * 100; }
    json generate(const json& input, const std::string&, const std::vector<ToolDefinition>& tools,
        const std::string&) override {
        inputs.push_back(input);
        definitions.push_back(tools);
        return responses.at(inputs.size() - 1);
    }
};

json toolCall(const std::string& id, const json& arguments, const std::string& name = "read") {
    return {{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments.dump()}};
}

json textItem(const std::string& text) {
    return {{"type", "message"}, {"id", "msg_final"}, {"role", "assistant"},
        {"content", json::array({{{"type", "output_text"}, {"text", text}}})}};
}

json response(const std::string& id, json output) {
    return {{"id", id}, {"status", "completed"}, {"output", std::move(output)}};
}

TEST_F(ReadToolTest, AgentLoopsOverMultipleReadsEvenWhenResponseAlsoContainsText) {
    write("first.txt", "first content");
    write("second.txt", "second content");
    auto provider = std::make_unique<ScriptedProvider>();
    auto* script = provider.get();
    script->responses = {
        response("r1", json::array({textItem("Reading"), toolCall("c1", {{"filePath", "first.txt"}}),
            toolCall("c2", {{"filePath", "second.txt"}})})),
        response("r2", json::array({toolCall("c3", {{"filePath", "first.txt"}, {"limit", 1}})})),
        response("r3", json::array({textItem("Done")}))
    };
    ToolRegistry registry;
    registry.add(std::make_unique<ReadTool>());
    Agent agent(std::move(provider), "test-model", std::move(registry), context);
    std::vector<AgentEvent> events;
    agent.setEventHandler([&](AgentEvent event) { events.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Read the files"}}});
    agent.run().get();
    ASSERT_EQ(script->inputs.size(), 3);
    ASSERT_EQ(script->definitions[0].size(), 1);
    EXPECT_EQ(script->definitions[0][0].id, "read");
    EXPECT_EQ(script->definitions[2][0].id, "read");
    EXPECT_EQ(script->inputs[1][4]["call_id"], "c1");
    EXPECT_NE(script->inputs[1][4]["output"].get<std::string>().find("first content"), std::string::npos);
    EXPECT_EQ(script->inputs[1][5]["call_id"], "c2");
    EXPECT_NE(script->inputs[1][5]["output"].get<std::string>().find("second content"), std::string::npos);
    const auto messages = agent.getMessages();
    ASSERT_EQ(messages.size(), 7);
    EXPECT_EQ(messages[2].role, MessageRole::TOOL);
    EXPECT_FALSE(std::get<ToolResultContent>(messages[2].content[0]).isError);
    EXPECT_EQ(std::get<ToolResultContent>(messages[2].content[0]).metadata["display"]["text"], "first content");
    EXPECT_FALSE(script->inputs[1][4].contains("metadata"));
    EXPECT_EQ(script->inputs[1][4]["output"].get<std::string>().find("\"display\""), std::string::npos);
    EXPECT_EQ(std::get<TextContent>(messages.back().content[0]).text, "Done");
    EXPECT_FALSE(agent.isRunning());
    EXPECT_FALSE(messages[1].metadata); // Intermediate provider turn with tool calls.
    ASSERT_TRUE(messages.back().metadata);
    EXPECT_EQ(messages.back().metadata->model, "test-model");
    ASSERT_EQ(events.size(), 8);
    EXPECT_FALSE(events[0].context.lastTurnUsedContext);
    const std::vector<std::uint64_t> expected{100, 100, 100, 200, 200, 300, 300};
    for (std::size_t i = 1; i < events.size(); ++i) {
        EXPECT_EQ(events[i].context.lastTurnUsedContext, expected[i - 1]);
        EXPECT_EQ(events[i].context.totalContext, 10000);
    }
}

TEST_F(ReadToolTest, CancelsRunningShellSkipsRemainingCallsAndResumesSameConversation) {
    auto provider = std::make_unique<ScriptedProvider>();
    auto* script = provider.get();
    script->responses = {
        response("first", json::array({toolCall("running", {{"command", "touch started; sleep 10"}, {"mainCommand", "touch"}}, "bash"),
            toolCall("pending", {{"command", "touch should-not-run"}, {"mainCommand", "touch"}}, "bash")})),
        response("resumed", json::array({textItem("Resumed with the interruption history")}))
    };
    ToolRegistry tools;
    tools.add(std::make_unique<BashTool>());
    Agent agent(std::move(provider), "model", std::move(tools), context);
    std::vector<AgentEvent> events;
    agent.setEventHandler([&](AgentEvent event) { events.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run two commands"}}});
    const auto running = agent.run();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!fs::exists(context.directory / "started") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(fs::exists(context.directory / "started"));
    agent.cancel();
    EXPECT_EQ(running.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    running.get();
    EXPECT_EQ(events.back().type, AgentEventType::EXECUTION_CANCELLED);
    EXPECT_FALSE(fs::exists(context.directory / "should-not-run"));
    const auto messages = agent.getMessages();
    ASSERT_EQ(messages.size(), 5);
    const auto& interrupted = std::get<ToolResultContent>(messages[2].content[0]);
    const auto& skipped = std::get<ToolResultContent>(messages[3].content[0]);
    EXPECT_TRUE(interrupted.isError);
    EXPECT_EQ(interrupted.metadata["cancelled"], true);
    EXPECT_EQ(interrupted.metadata["started"], true);
    EXPECT_EQ(skipped.metadata["started"], false);
    EXPECT_EQ(messages[4].role, MessageRole::SYSTEM);
    agent.append(Message{"next", MessageRole::USER, {TextContent{"Continue differently"}}});
    agent.run().get();
    EXPECT_EQ(script->inputs.size(), 2);
    EXPECT_NE(script->inputs.back().dump().find("previous run was interrupted"), std::string::npos);
    EXPECT_EQ(events.back().type, AgentEventType::EXECUTION_COMPLETED);
    EXPECT_FALSE(fs::exists(context.directory / "should-not-run"));
}

TEST_F(ReadToolTest, AgentReturnsToolErrorsAndPermissionDenialsToModel) {
    write("file.txt", "private data");
    context.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    auto provider = std::make_unique<ScriptedProvider>();
    auto* script = provider.get();
    script->responses = {
        response("r1", json::array({toolCall("c1", {{"filePath", "file.txt"}}),
            toolCall("c2", json::object(), "unknown"), toolCall("c3", {{"filePath", "file.txt"}, {"limit", -1}})})),
        response("r2", json::array({textItem("Could not read")}))
    };
    ToolRegistry registry;
    registry.add(std::make_unique<ReadTool>());
    Agent agent(std::move(provider), "test-model", std::move(registry), context);
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Read"}}});
    agent.run().get();
    ASSERT_EQ(script->inputs.size(), 2);
    const auto messages = agent.getMessages();
    for (std::size_t i = 2; i <= 4; ++i)
        EXPECT_TRUE(std::get<ToolResultContent>(messages[i].content[0]).isError);
    EXPECT_EQ(json::parse(script->inputs[1][4]["output"].get<std::string>())["error"], "Denied");
    EXPECT_EQ(script->inputs[1].dump().find("private data"), std::string::npos);
}

TEST_F(ReadToolTest, AgentForwardsImageAndPdfAttachmentsInToolResults) {
    write("image.gif", "GIF89a");
    write("document.pdf", "%PDF-1.7\n");
    auto provider = std::make_unique<ScriptedProvider>();
    auto* script = provider.get();
    script->responses = {
        response("r1", json::array({toolCall("c1", {{"filePath", "image.gif"}}),
            toolCall("c2", {{"filePath", "document.pdf"}})})),
        response("r2", json::array({textItem("Done")}))
    };
    ToolRegistry registry;
    registry.add(std::make_unique<ReadTool>());
    Agent agent(std::move(provider), "test-model", std::move(registry), context);
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Read"}}});
    agent.run().get();
    ASSERT_EQ(script->inputs.size(), 2);
    const auto image = script->inputs[1][3]["output"];
    EXPECT_EQ(image[0]["type"], "input_text");
    EXPECT_EQ(image[1]["type"], "input_image");
    EXPECT_EQ(image[1]["image_url"], "data:image/gif;base64,R0lGODlh");
    const auto pdf = script->inputs[1][4]["output"];
    EXPECT_EQ(pdf[1]["type"], "input_file");
    EXPECT_EQ(pdf[1]["file_data"], "data:application/pdf;base64,JVBERi0xLjcK");
    EXPECT_EQ(std::get<ToolResultContent>(agent.getMessages()[2].content[0]).attachments.size(), 1);
}

TEST_F(ReadToolTest, RegistryOwnsToolsAndExposesDefinitions) {
    ToolRegistry registry;
    registry.add(std::make_unique<ReadTool>());
    ASSERT_EQ(registry.getDefinitions().size(), 1);
    const auto definition = registry.getDefinitions()[0];
    EXPECT_EQ(definition.id, "read");
    EXPECT_EQ(definition.parameters["required"], json::array({"filePath"}));
    EXPECT_THROW(registry.add(std::make_unique<ReadTool>()), std::invalid_argument);
    EXPECT_THROW(registry.add(nullptr), std::invalid_argument);
    EXPECT_THROW(registry.get("write"), std::out_of_range);
    write("file.txt", "Hello");
    EXPECT_EQ(registry.get("read").execute({{"filePath", "file.txt"}}, context).metadata["preview"], "Hello");
}
}
