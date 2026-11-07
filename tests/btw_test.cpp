#include <daemon/session/btw_conversation.hpp>
#include <daemon/session/session_journal.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <set>

namespace {
using nlohmann::json;
class BtwTest : public testing::Test {
protected:
    std::filesystem::path directory;
    void SetUp() override {
        std::string path = "/tmp/sos-btw-XXXXXX";
        ASSERT_NE(mkdtemp(path.data()), nullptr);
        directory = path;
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
};

class BtwProvider : public Provider {
public:
    std::vector<json> inputs;
    std::vector<ToolDefinition> definitions;
    std::string selectedModel;
    std::string effort;
    json calls = json::array();
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    json generate(const json& input, const std::string& model, const std::vector<ToolDefinition>& tools,
        const std::string& reasoning) override {
        inputs.push_back(input);
        definitions = tools;
        selectedModel = model;
        effort = reasoning;
        if (inputs.size() == 1 && !calls.empty()) return {{"id", "tools"}, {"status", "completed"}, {"output", calls}};
        return json::parse(R"({"id":"answer","status":"completed","output":[{"type":"message","id":"text","role":"assistant","content":[{"type":"output_text","text":"Informative answer","annotations":[]}]}]})");
    }
};

TEST_F(BtwTest, FrozenContextAndMessagesPersistSeparatelyFromThePrincipal) {
    BtwConversation branch;
    branch.anchor = "principal-user";
    branch.snapshot = {{"system", MessageRole::SYSTEM, {TextContent{"Original instructions"}}},
        {branch.anchor, MessageRole::USER, {TextContent{"Principal request"}}}};
    branch.messages = {{"btw-user", MessageRole::USER, {TextContent{"Why this approach?"}}}};
    branch.model = "model";
    branch.reasoning = "high";
    branch.provider = "openai";
    {
        SessionJournal journal(directory);
        journal.append(branch.snapshot, 1);
        branch = journal.createBtw(branch);
        EXPECT_EQ(branch.id, "btw-1");
        branch.snapshot.clear(); // Saving an exchange must never overwrite the frozen column.
        branch.messages.push_back({"answer", MessageRole::ASSISTANT, {TextContent{"Because…"}}});
        journal.saveBtw(branch);
        EXPECT_EQ(journal.count(), 2);
        auto second = journal.createBtw(branch);
        EXPECT_EQ(second.id, "btw-2");
    }
    SessionJournal restored(directory);
    const auto saved = restored.readBtw("btw-1");
    EXPECT_EQ(saved.snapshot.size(), 2);
    EXPECT_EQ(saved.messages.size(), 2);
    EXPECT_EQ(saved.anchor, "principal-user");
    EXPECT_EQ(saved.model, "model");
    EXPECT_EQ(restored.listBtw().size(), 2);
    EXPECT_TRUE(restored.listBtw()[0].snapshot.empty()); // Listing does not read frozen contexts.
    EXPECT_THROW(restored.readBtw("btw-999"), std::invalid_argument);
}

TEST_F(BtwTest, SnapshotBalancesOnlyUnfinishedPrincipalToolCalls) {
    std::vector<Message> messages{{"user", MessageRole::USER, {TextContent{"Do work"}}},
        {"call", MessageRole::ASSISTANT, {ToolCallContent{"pending", "bash", {{"command", "dangerous"}}}}}};
    const auto frozen = freezeBtwContext(messages);
    ASSERT_EQ(frozen.size(), 3);
    const auto& result = std::get<ToolResultContent>(frozen.back().content.front());
    EXPECT_EQ(result.callId, "pending");
    EXPECT_TRUE(result.isError);
    EXPECT_NE(result.output.get<std::string>().find("unknown"), std::string::npos);
    EXPECT_EQ(freezeBtwContext(frozen).size(), 3);
    EXPECT_EQ(messages.size(), 2);
}

TEST_F(BtwTest, StepReviewSummaryAndIdentitySurviveResumeWithoutEnteringPrincipalContext) {
    BtwConversation branch;
    branch.review = true; branch.reviewStep = 1;
    branch.reviewSummary = "state.cpp: explicit decision, failed baseline test, no accepted correction";
    SessionJournal journal(directory);
    branch = journal.createBtw(branch);
    auto restored = SessionJournal(directory).readBtw(branch.id);
    EXPECT_EQ(restored.reviewStep, 1);
    EXPECT_EQ(restored.reviewSummary, branch.reviewSummary);
    EXPECT_EQ(journal.count(), 0);
    restored.revisionPlanning = true;
    journal.saveBtw(restored);
    EXPECT_TRUE(SessionJournal(directory).readBtw(branch.id).revisionPlanning);
}

TEST_F(BtwTest, ReviewActiveContextResumesWithoutReinjectingDiscardedConstructionOrLosingHistory) {
    BtwConversation branch;
    branch.review = true;
    branch.snapshot = {{"system", MessageRole::SYSTEM, {TextContent{"Original instructions"}}},
        {"old", MessageRole::USER, {TextContent{"DISCARDED_CONSTRUCTION"}}}};
    branch.messages = {{"original", MessageRole::USER, {TextContent{"ORIGINAL_REVIEW"}}}};
    branch.activeContext = {{"system", MessageRole::SYSTEM, {TextContent{"Review is read-only"}}},
        {"summary", MessageRole::ASSISTANT, {TextContent{"ACTIVE_CHECKPOINT"}}}};
    branch.contextMessageCount = 1;
    SessionJournal journal(directory);
    branch = journal.createBtw(branch);
    branch = SessionJournal(directory).readBtw(branch.id);
    branch.messages.push_back({"new", MessageRole::USER, {TextContent{"LATEST_QUESTION"}}});
    journal.saveBtw(branch);
    auto provider = std::make_unique<BtwProvider>();
    auto* observed = provider.get();
    auto agent = makeBtwAgent(branch, std::move(provider), {"openai", "review-model", "high"}, directory, {}, directory);
    agent->run().get();
    const auto input = observed->inputs.back().dump();
    EXPECT_NE(input.find("ACTIVE_CHECKPOINT"), std::string::npos);
    EXPECT_NE(input.find("LATEST_QUESTION"), std::string::npos);
    EXPECT_EQ(input.find("DISCARDED_CONSTRUCTION"), std::string::npos);
    EXPECT_NE(json(SessionJournal(directory).readBtw(branch.id).messages).dump().find("ORIGINAL_REVIEW"), std::string::npos);
}

TEST_F(BtwTest, AcceptedReviewSegmentIsVisibleButNotReinjectedOrSummarizedAgain) {
    BtwConversation branch;
    branch.review = true;
    branch.snapshot = {{"construction", MessageRole::SYSTEM, {TextContent{"CORRECTED_BUILD_WITH_ACCEPTED_DISCUSSION"}}}};
    branch.messages = {{"old", MessageRole::USER, {TextContent{"ALREADY_INCORPORATED_REVIEW"}}},
        {"review-integrated-old", MessageRole::SYSTEM, {TextContent{"Accepted"}}}};
    branch.reviewSegmentStart = branch.contextMessageCount = branch.messages.size();
    SessionJournal journal(directory);
    branch = journal.createBtw(branch);
    branch = SessionJournal(directory).readBtw(branch.id);
    EXPECT_EQ(branch.reviewSegmentStart, 2);
    EXPECT_TRUE(currentReviewMessages(branch).empty());
    branch.messages.push_back({"new", MessageRole::USER, {TextContent{"NEW_REVIEW_QUESTION"}}});
    const auto summarySource = json(currentReviewMessages(branch)).dump();
    EXPECT_EQ(summarySource.find("ALREADY_INCORPORATED_REVIEW"), std::string::npos);
    EXPECT_NE(summarySource.find("NEW_REVIEW_QUESTION"), std::string::npos);
    auto provider = std::make_unique<BtwProvider>(); auto* observed = provider.get();
    auto agent = makeBtwAgent(branch, std::move(provider), {"openai", "review-model", "high"}, directory, {}, directory);
    agent->run().get();
    const auto input = observed->inputs.back().dump();
    EXPECT_NE(input.find("CORRECTED_BUILD_WITH_ACCEPTED_DISCUSSION"), std::string::npos);
    EXPECT_NE(input.find("NEW_REVIEW_QUESTION"), std::string::npos);
    EXPECT_EQ(input.find("ALREADY_INCORPORATED_REVIEW"), std::string::npos);
    EXPECT_NE(json(branch.messages).dump().find("ALREADY_INCORPORATED_REVIEW"), std::string::npos);
    auto invalid = json(branch); invalid["review_segment_start"] = 100;
    EXPECT_THROW(invalid.get<BtwConversation>(), std::invalid_argument);
}

TEST_F(BtwTest, HistoricalReadsMapOriginalPathsAndCannotEscapeToTheLiveCheckout) {
    const auto live = directory / "live", frozen = directory / "frozen";
    std::filesystem::create_directories(live);
    std::ofstream(live / "state.txt") << "FUTURE_CONTENT";
    BtwConversation branch; branch.review = true;
    branch.messages = {{"question", MessageRole::USER, {TextContent{"Read state.txt"}}}};
    auto provider = std::make_unique<BtwProvider>(); auto* fake = provider.get();
    fake->calls = json::array({
        {{"type", "function_call"}, {"id", "read"}, {"call_id", "read"}, {"name", "read"},
            {"arguments", json{{"filePath", (live / "state.txt").string()}}.dump()}},
        {{"type", "function_call"}, {"id", "external"}, {"call_id", "external"}, {"name", "read"},
            {"arguments", json{{"filePath", (directory / "secret.txt").string()}}.dump()}}});
    std::ofstream(directory / "secret.txt") << "SECRET_CONTENT";
    bool prepared = false;
    auto agent = makeBtwAgent(branch, std::move(provider), {"openai", "model", "high"}, frozen,
        [](const auto&) -> Image { throw std::runtime_error("No image"); }, frozen, false, {live},
        [&](const auto&) {
            if (prepared) return;
            std::filesystem::create_directory(frozen);
            std::ofstream(frozen / "state.txt") << "HISTORICAL_CONTENT";
            prepared = true;
        });
    EXPECT_FALSE(prepared);
    agent->run().get();
    ASSERT_EQ(fake->inputs.size(), 2);
    EXPECT_NE(fake->inputs.back().dump().find("HISTORICAL_CONTENT"), std::string::npos);
    EXPECT_EQ(fake->inputs.back().dump().find("FUTURE_CONTENT"), std::string::npos);
    EXPECT_EQ(fake->inputs.back().dump().find("SECRET_CONTENT"), std::string::npos);
}

TEST_F(BtwTest, StandalonePlacementPersistsAndIncompletePlacementIsRejected) {
    BtwConversation branch;
    branch.standalone = true;
    branch.anchor = "last-activity";
    branch.pageAnchor = "last-user";
    {
        SessionJournal journal(directory);
        branch = journal.createBtw(branch);
    }
    SessionJournal reopened(directory);
    const auto restored = reopened.readBtw(branch.id);
    EXPECT_TRUE(restored.standalone);
    EXPECT_EQ(restored.anchor, branch.anchor);
    EXPECT_EQ(restored.pageAnchor, branch.pageAnchor);
    EXPECT_TRUE(reopened.listBtw().front().standalone);
    for (const auto* field : {"standalone", "pageAnchor", "review", "revision_planning", "review_summary",
        "review_segment_start", "active_context", "context_construction_key", "context_message_count"}) {
        auto incomplete = json(branch);
        incomplete.erase(field);
        EXPECT_THROW(incomplete.get<BtwConversation>(), json::out_of_range) << field;
    }
}

TEST_F(BtwTest, IdleBranchInheritsContextWithoutClaimingPrincipalExecution) {
    BtwConversation branch;
    branch.standalone = true;
    branch.snapshot = {{"principal", MessageRole::USER, {TextContent{"Original request"}}},
        {"answer", MessageRole::ASSISTANT, {TextContent{"Finished answer"}}}};
    branch.messages = {{"question", MessageRole::USER, {TextContent{"Explain"}}}};
    auto provider = std::make_unique<BtwProvider>();
    auto* fake = provider.get();
    auto agent = makeBtwAgent(branch, std::move(provider), {"openai", "model", "high"}, directory,
        [](const auto&) -> Image { throw std::runtime_error("No image"); }, directory);
    agent->run().get();
    const auto input = fake->inputs.front().dump();
    EXPECT_NE(input.find("Original request"), std::string::npos);
    EXPECT_NE(input.find("Finished answer"), std::string::npos);
    EXPECT_NE(input.find("independent, informative BTW"), std::string::npos);
    EXPECT_EQ(input.find("was being processed"), std::string::npos);
}

TEST_F(BtwTest, AgentExposesOnlyReadToolsAndRefusesWritesAndCommandsAtRuntime) {
    std::ofstream(directory / "readable.txt") << "Workspace contents";
    BtwConversation branch;
    branch.snapshot = {{"principal", MessageRole::USER, {TextContent{"Original task"}}}};
    branch.messages = {{"question", MessageRole::USER, {TextContent{"Explain"}}}};
    auto provider = std::make_unique<BtwProvider>();
    auto* fake = provider.get();
    for (const auto& [name, args] : std::vector<std::pair<std::string, json>>{
        {"read", {{"filePath", (directory / "readable.txt").string()}}},
        {"bash", {{"command", "touch forbidden"}}},
        {"write", {{"filePath", (directory / "forbidden").string()}, {"content", "bad"}}},
        {"apply_patch", {{"patchText", "*** Begin Patch\n*** Add File: forbidden\n+bad\n*** End Patch"}}}}) {
        fake->calls.push_back({{"type", "function_call"}, {"id", name}, {"call_id", name}, {"name", name}, {"arguments", args.dump()}});
    }
    auto agent = makeBtwAgent(branch, std::move(provider), {"openai", "chosen-model", "high"}, directory,
        [](const auto&) -> Image { throw std::runtime_error("No image"); }, directory);
    agent->run().get();
    std::set<std::string> names;
    for (const auto& tool : fake->definitions) names.insert(tool.id);
    EXPECT_EQ(names, (std::set<std::string>{"read", "glob", "grep", "webfetch", "view_image"}));
    EXPECT_EQ(fake->selectedModel, "chosen-model");
    EXPECT_EQ(fake->effort, "high");
    ASSERT_EQ(fake->inputs.size(), 2);
    EXPECT_NE(fake->inputs[0].dump().find("Original task"), std::string::npos);
    EXPECT_NE(fake->inputs[0].dump().find("frozen snapshot"), std::string::npos);
    EXPECT_NE(fake->inputs[1].dump().find("Workspace contents"), std::string::npos);
    EXPECT_NE(fake->inputs[1].dump().find("This tool is not allowed"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(directory / "forbidden"));
}

TEST_F(BtwTest, PreviousSchemaIsRejectedWithoutReplacingThePrincipalJournal) {
    {
        SessionJournal journal(directory);
        journal.append({{"user", MessageRole::USER, {TextContent{"Preserve me"}}}}, 1);
    }
    {
        SessionDatabase db(directory / "session.sqlite");
        db.execute("DROP TABLE btw; PRAGMA user_version=4;");
    }
    EXPECT_THROW(SessionJournal{directory}, std::runtime_error);
    SessionDatabase db(directory / "session.sqlite");
    SessionDatabase::Statement read(db, "SELECT id FROM messages");
    ASSERT_TRUE(read.step());
    EXPECT_EQ(read.text(0), "user");
}

TEST_F(BtwTest, ReadOnlyBranchCannotReadOutsideTheIntegrationRoot) {
    std::ofstream(directory / "private.txt") << "Do not expose these contents";
    std::filesystem::create_directory(directory / "authorized");
    BtwConversation branch;
    branch.messages = {{"question", MessageRole::USER, {TextContent{"Read"}}}};
    auto provider = std::make_unique<BtwProvider>();
    auto* fake = provider.get();
    fake->calls.push_back({{"type", "function_call"}, {"id", "read"}, {"call_id", "read"}, {"name", "read"},
        {"arguments", json({{"filePath", (directory / "private.txt").string()}}).dump()}});
    auto agent = makeBtwAgent(branch, std::move(provider), {"openai", "model", "medium"}, directory,
        [](const auto&) -> Image { throw std::runtime_error("No image"); }, directory / "authorized");
    agent->run().get();
    ASSERT_EQ(fake->inputs.size(), 2);
    EXPECT_NE(fake->inputs[1].dump().find("external access is not approved"), std::string::npos);
    EXPECT_EQ(fake->inputs[1].dump().find("Do not expose these contents"), std::string::npos);
}

TEST_F(BtwTest, FrozenImagePixelsSurviveReopeningTheDatabase) {
    const auto image = validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"});
    BtwConversation branch;
    Message user{"original", MessageRole::USER, {TextContent{"An image"}}};
    user.images = {image};
    branch.snapshot = {user};
    {
        SessionJournal journal(directory);
        branch = journal.createBtw(branch);
    }
    SessionJournal reopened(directory);
    const auto restored = reopened.readBtw(branch.id);
    ASSERT_EQ(restored.snapshot.size(), 1);
    ASSERT_EQ(restored.snapshot[0].images.size(), 1);
    EXPECT_EQ(restored.snapshot[0].images[0].data, image.data);
    EXPECT_EQ(restored.snapshot[0].images[0].id, image.id);
}
}
