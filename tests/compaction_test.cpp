#include <daemon/session/compaction_service.hpp>
#include <daemon/prompts.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <gtest/gtest.h>
#include <limits>

namespace {
using nlohmann::json;

class SummaryProvider : public Provider {
public:
    json input;
    std::vector<json> inputs;
    std::vector<std::string> answers;
    json response;
    std::vector<ToolDefinition> tools;
    std::string model, reasoning;
    SummaryProvider() {
        response = {{"id", "summary_response"}, {"status", "completed"}, {"output", json::array({{
            {"type", "message"}, {"id", "summary_item"}, {"role", "assistant"},
            {"content", json::array({{{"type", "output_text"}, {"text", "## Goal\n- Continue the work."}, {"annotations", json::array()}}})}
        }})}};
    }
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    json generate(const json& input, const std::string& model, const std::vector<ToolDefinition>& tools,
        const std::string& reasoning) override {
        this->input = input;
        inputs.push_back(input);
        this->model = model;
        this->reasoning = reasoning;
        this->tools = tools;
        if (!answers.empty()) response["output"][0]["content"][0]["text"] = answers[std::min(inputs.size() - 1, answers.size() - 1)];
        return response;
    }
};

Conversation history() {
    Conversation result(ProviderFormat::OPENAI_RESPONSES);
    result.append(Message{"system", MessageRole::SYSTEM, {TextContent{"Original instructions"}}});
    result.append(Message{"a", MessageRole::USER, {TextContent{"Old request: preserve my constraints"}}});
    result.append(Message{"b", MessageRole::ASSISTANT, {ToolCallContent{"call", "read", {{"filePath", "old.cpp"}}}}});
    result.append(Message{"c", MessageRole::TOOL, {ToolResultContent{"call", std::string(4000, 'x') + "OLD_END", false}}});
    result.append(Message{"d", MessageRole::ASSISTANT, {TextContent{"Old result"}}});
    result.append(Message{"e", MessageRole::USER, {TextContent{"Recent request"}}});
    result.append(Message{"f", MessageRole::ASSISTANT, {ToolCallContent{"recent_call", "read", {{"filePath", "recent.cpp"}}}}});
    result.append(Message{"g", MessageRole::TOOL, {ToolResultContent{"recent_call", "Recent result", false}}});
    result.append(Message{"h", MessageRole::ASSISTANT, {TextContent{"Recent answer"}}});
    return result;
}

TEST(CompactionTest, ImagesBecomeStableReferencesWithoutSendingPixelsToSummarizer) {
    auto image = validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"});
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    const auto original = history();
    for (auto message : original.getMessages()) {
        if (message.id == "a" || message.id == "e") message.images = {image};
        conversation.append(message);
    }
    SummaryProvider provider;
    auto result = CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 30000);
    EXPECT_EQ(provider.input.dump().find(image.data), std::string::npos);
    EXPECT_NE(provider.input.dump().find(image.id), std::string::npos);
    ASSERT_EQ(result.summary.compaction->imageReferences.size(), 1);
    EXPECT_TRUE(result.summary.compaction->imageReferences[0].data.empty());
    EXPECT_NE(std::get<TextContent>(result.summary.content[0]).text.find(image.id), std::string::npos);
    bool retained = false;
    for (const auto& message : result.conversation.getMessages())
        if (message.id == "e") { retained = true; EXPECT_EQ(message.images[0].data, image.data); }
    EXPECT_TRUE(retained);
    EXPECT_EQ(conversation.getMessages()[1].images[0].data, image.data);
    auto second = CompactionService{}.compact(result.conversation, provider, SessionInfo{}, "next", {}, 0);
    EXPECT_EQ(second.summary.compaction->imageReferences[0].id, image.id);
    EXPECT_EQ(provider.input.dump().find(image.data), std::string::npos);
}

TEST(CompactionTest, RecentBudgetUsesQuarterOfContextWithoutFloorAndCapsAtThirtyThousand) {
    EXPECT_EQ(CompactionService::recentBudget(3), 0);
    EXPECT_EQ(CompactionService::recentBudget(2000), 500);
    EXPECT_EQ(CompactionService::recentBudget(64000), 16000);
    EXPECT_EQ(CompactionService::recentBudget(120000), 30000);
    EXPECT_EQ(CompactionService::recentBudget(1000000), 30000);
    EXPECT_EQ(CompactionService::recentBudget(std::nullopt), CompactionService::DEFAULT_RECENT_TOKENS);
    EXPECT_EQ(CompactionService::recentBudget(0), CompactionService::DEFAULT_RECENT_TOKENS);
}

TEST(CompactionTest, ProgressCountsCorrectionsAndResetsPerCompaction) {
    SummaryProvider provider;
    provider.answers = {std::string(9000, 'x'), "Short summary"};
    std::vector<std::size_t> attempts;
    const auto progress = [&](std::size_t attempt) {
        EXPECT_EQ(provider.inputs.size() + 1, attempt);
        attempts.push_back(attempt);
    };
    CompactionService{}.compact(history(), provider, SessionInfo{}, "checkpoint", {}, 30000, false, progress);
    EXPECT_EQ(attempts, (std::vector<std::size_t>{1, 2}));
    attempts.clear();
    provider.inputs.clear();
    provider.answers = {"Short summary"};
    CompactionService{}.compact(history(), provider, SessionInfo{}, "next", {}, 30000, false, progress);
    EXPECT_EQ(attempts, (std::vector<std::size_t>{1}));
}

TEST(CompactionTest, ReviewSummaryPreservesConcreteEvidenceUsesSelectedModelAndRejectsEmptyOrCancelledResults) {
    SummaryProvider provider;
    const ProviderSelection selection{"openai", "review-model", "high"};
    const std::vector<Message> messages{
        {"question", MessageRole::USER, {TextContent{"Keep state.cpp unchanged; why did test X fail?"}}},
        {"answer", MessageRole::ASSISTANT, {TextContent{"test X fails on baseline; no correction accepted."}}}};
    EXPECT_FALSE(CompactionService::summarizeReview(messages, provider, selection, {}).empty());
    EXPECT_EQ(provider.model, "review-model");
    EXPECT_EQ(provider.reasoning, "high");
    EXPECT_TRUE(provider.tools.empty());
    EXPECT_NE(provider.input.dump().find("state.cpp unchanged"), std::string::npos);
    EXPECT_NE(provider.input.dump().find("no correction accepted"), std::string::npos);
    EXPECT_NE(provider.input.dump().find("Distinguish proposals from accepted decisions"), std::string::npos);
    EXPECT_NE(provider.input.dump().find("summary in English, even when the user writes in another language"), std::string::npos);
    EXPECT_THROW(CompactionService::summarizeReview(messages, provider, selection, [] { return true; }), core::Cancelled);
    provider.response["output"][0]["content"][0]["text"] = "   ";
    EXPECT_THROW(CompactionService::summarizeReview(messages, provider, selection, {}), std::runtime_error);
}

TEST(CompactionTest, ReviewSummaryUsesOriginalExchangesAndNeverCompactionOrSelectionContext) {
    SummaryProvider provider;
    Message checkpoint{"checkpoint", MessageRole::ASSISTANT, {TextContent{"CONSTRUCTION_DUPLICATE"}}};
    checkpoint.compaction = CompactionMetadata{{}, "sane"};
    const std::vector<Message> messages{
        {"review-selection-0", MessageRole::SYSTEM, {TextContent{"CONSTRUCTION_SELECTION"}}},
        {"before", MessageRole::USER, {TextContent{"ORIGINAL_BEFORE"}}},
        checkpoint,
        {"compaction-status-1", MessageRole::SYSTEM, {TextContent{"started"}}},
        {"after", MessageRole::ASSISTANT, {TextContent{"ORIGINAL_AFTER"}}},
        {"tool", MessageRole::TOOL, {ToolResultContent{"call", std::string(3000, 'x') + "TOOL_END", false}}}};
    CompactionService::summarizeReview(messages, provider, {}, {});
    const auto input = provider.input.dump();
    EXPECT_NE(input.find("ORIGINAL_BEFORE"), std::string::npos);
    EXPECT_NE(input.find("ORIGINAL_AFTER"), std::string::npos);
    EXPECT_NE(input.find("TOOL_END"), std::string::npos);
    EXPECT_EQ(input.find("CONSTRUCTION_DUPLICATE"), std::string::npos);
    EXPECT_EQ(input.find("CONSTRUCTION_SELECTION"), std::string::npos);
}

TEST(CompactionTest, ReviewSummaryReducesOversizedHistoryInBoundedRequestsAndMergesIt) {
    SummaryProvider provider;
    std::vector<Message> messages;
    for (unsigned i = 0; i < 12; ++i) messages.push_back(Message{"m" + std::to_string(i), MessageRole::USER,
        {TextContent{"CHUNK_" + std::to_string(i) + " " + std::string(8000, 'a')}}});
    ProviderSelection selection{"openai", "selected-model", "high", 32768};
    auto result = CompactionService::summarizeReview(messages, provider, selection, {});
    EXPECT_LE(result.size(), 4096);
    EXPECT_GT(provider.inputs.size(), 2);
    for (const auto& input : provider.inputs) EXPECT_LT(input.dump().size(), 17000);
    EXPECT_NE(provider.inputs.back().dump().find("Continue the work"), std::string::npos);
    EXPECT_EQ(provider.model, selection.model);
    EXPECT_TRUE(provider.tools.empty());
}

TEST(CompactionTest, OversizedSummaryRetriesSmallestCandidateThenEmergencyAndTruncates) {
    SummaryProvider provider;
    provider.answers = {std::string(6000, 'a'), std::string(5000, 'b'), std::string(7000, 'c'), std::string(8000, 'd')};
    const auto result = CompactionService::summarizeReview(
        {Message{"q", MessageRole::USER, {TextContent{"Question"}}}}, provider, {}, {});
    ASSERT_EQ(provider.inputs.size(), 4);
    EXPECT_LE(result.size(), 4096);
    EXPECT_TRUE(result.starts_with(std::string(100, 'b')));
    EXPECT_NE(result.find("Summary truncated"), std::string::npos);
    const auto first = provider.inputs[1].dump();
    EXPECT_NE(first.find("6001 UTF-8 bytes"), std::string::npos);
    EXPECT_NE(first.find("maximum of 4096 UTF-8 bytes"), std::string::npos);
    EXPECT_NE(first.find("at least 32%"), std::string::npos);
    EXPECT_NE(first.find("Aim for a little more reduction to be sure"), std::string::npos);
    EXPECT_NE(provider.inputs[2].dump().find("5001 UTF-8 bytes"), std::string::npos);
    EXPECT_NE(provider.inputs[3].dump().find("emergency summary"), std::string::npos);
    EXPECT_NE(provider.inputs[3].dump().find(std::string(100, 'b')), std::string::npos);
    EXPECT_EQ(provider.inputs[3].dump().find(std::string(100, 'c')), std::string::npos);
}

TEST(CompactionTest, SummaryStopsAsSoonAsCorrectionFits) {
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        SummaryProvider provider;
        provider.answers = std::vector<std::string>(attempt, std::string(6000, 'x'));
        provider.answers.push_back("Short summary");
        const auto result = CompactionService::summarizeReview(
            {Message{"q", MessageRole::USER, {TextContent{"Question"}}}}, provider, {}, {});
        EXPECT_EQ(result, "Short summary\n");
        EXPECT_EQ(provider.inputs.size(), attempt + 1);
    }
}

TEST(CompactionTest, SummaryFallbackKeepsUtf8ValidAndDoesNotHideProviderFailures) {
    SummaryProvider provider;
    std::string text;
    for (unsigned i = 0; i < 3000; ++i) text += "界";
    provider.answers = {text};
    const auto result = CompactionService::summarizeReview(
        {Message{"q", MessageRole::USER, {TextContent{"Question"}}}}, provider, {}, {});
    EXPECT_LE(result.size(), 4096);
    EXPECT_NO_THROW(json(result).dump());
    provider.answers = {std::string(6000, 'x'), " "};
    provider.inputs.clear();
    EXPECT_THROW(CompactionService::summarizeReview(
        {Message{"q", MessageRole::USER, {TextContent{"Question"}}}}, provider, {}, {}), std::runtime_error);
    EXPECT_EQ(provider.inputs.size(), 2);
}

TEST(CompactionTest, NonReducingModelStillCompactsLargeInputsInBoundedRequests) {
    SummaryProvider provider;
    provider.answers = {std::string(9000, 'x')};
    const auto result = CompactionService::summarizeReview(
        {Message{"q", MessageRole::USER, {TextContent{std::string(50000, 'x')}}}}, provider, {}, {});
    EXPECT_LE(result.size(), 4096);
    EXPECT_LT(provider.inputs.size(), 30);
}

TEST(CompactionTest, OrdinaryCompactionAlsoCorrectsOversizedSummaries) {
    SummaryProvider provider;
    provider.answers = {std::string(9000, 'x'), "Short summary"};
    auto result = CompactionService{}.compact(history(), provider, SessionInfo{}, "checkpoint", {}, 30000);
    EXPECT_EQ(provider.inputs.size(), 2);
    EXPECT_EQ(std::get<TextContent>(result.summary.content[0]).text, "Short summary\n");
}

TEST(CompactionTest, CompactionRequestsOnePercentWithTwoThousandTokenFloorWithoutAdvertisingTolerance) {
    class SizedProvider : public SummaryProvider {
    public:
        ModelInfo limits{"summary", 400000, {}, {}};
        ModelInfo getModelInfo(const std::string&) override { return limits; }
    } provider;
    for (const auto context : {32000, 128000, 200000, 400000}) {
        SessionInfo info;
        info.contextLimit = context;
        const auto tokens = std::max(2000, context / 20);
        provider.inputs.clear();
        provider.answers = {std::string(tokens * 4 - 1, 'x')};
        auto result = CompactionService{}.compact(history(), provider, info, "checkpoint", {}, 30000);
        ASSERT_EQ(provider.inputs.size(), 1);
        const auto prompt = provider.inputs.front().dump();
        EXPECT_NE(prompt.find("MAXIMUM " + std::to_string(std::max(2000, context / 100)) + " tokens"), std::string::npos);
        EXPECT_NE(prompt.find("ceiling, not a target"), std::string::npos);
        EXPECT_NE(prompt.find("Do not pad"), std::string::npos);
        EXPECT_EQ(prompt.find("5%"), std::string::npos);
        EXPECT_EQ(prompt.find("UTF-8 bytes"), std::string::npos);
        EXPECT_EQ(std::get<TextContent>(result.summary.content.front()).text.size(), tokens * 4);
    }
    provider.inputs.clear();
    provider.answers = {"Short summary"};
    CompactionService{}.compact(history(), provider, SessionInfo{}, "provider-context", {}, 30000);
    EXPECT_NE(provider.input.dump().find("MAXIMUM 4000 tokens"), std::string::npos);
    provider.limits.outputLimit = 1000;
    SessionInfo info;
    info.contextLimit = 128000;
    CompactionService{}.compact(history(), provider, info, "capped", {}, 30000);
    EXPECT_NE(provider.input.dump().find("MAXIMUM 1000 tokens"), std::string::npos);
}

TEST(CompactionTest, OrdinaryCompactionFitsLargerTranscriptsInOneCall) {
    auto conversation = history();
    conversation.append(Message{"large", MessageRole::USER, {TextContent{std::string(50000, 'a')}}});
    SummaryProvider provider;
    CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 0);
    EXPECT_EQ(provider.inputs.size(), 1);
}

TEST(CompactionTest, OrdinaryCompactionChunksLargeInputsAndLimitsEachSynthesisToThreeAttempts) {
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append(Message{"system", MessageRole::SYSTEM, {TextContent{"Initial instructions"}}});
    conversation.append(Message{"old", MessageRole::USER, {TextContent{std::string(250000, 'a')}}});
    conversation.append(Message{"answer", MessageRole::ASSISTANT, {TextContent{"Earlier answer"}}});
    SummaryProvider provider;
    provider.answers = {std::string(9000, 'x')}; // Never cooperates with any reduction request.
    std::vector<std::size_t> attempts;
    auto result = CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 0, false,
        [&](std::size_t attempt) { attempts.push_back(attempt); });
    const auto& text = std::get<TextContent>(result.summary.content.front()).text;
    EXPECT_LE(text.size(), 2000 * 4);
    EXPECT_NE(text.find("Summary truncated"), std::string::npos);
    ASSERT_GT(provider.inputs.size(), 3); // Multiple blocks and their final merge.
    ASSERT_EQ(provider.inputs.size() % 3, 0);
    ASSERT_EQ(attempts.size(), provider.inputs.size());
    for (std::size_t i = 0; i < provider.inputs.size(); ++i) {
        EXPECT_EQ(attempts[i], i % 3 + 1);
        if (i % 3) {
            EXPECT_NE(provider.inputs[i].dump().find("Rewrite it much more concisely"), std::string::npos);
            EXPECT_EQ(provider.inputs[i].dump().find("UTF-8 bytes"), std::string::npos);
        }
    }
    for (const auto& input : provider.inputs) EXPECT_LT(input.dump().size(), 100000);
    EXPECT_TRUE(provider.tools.empty());
    EXPECT_EQ(conversation.getMessages().size(), 3); // Source history is never pruned.
}

TEST(CompactionTest, NaturalChunkAndMergeCallsAlwaysReportFirstAttempt) {
    auto conversation = history();
    conversation.append(Message{"large", MessageRole::USER, {TextContent{std::string(250000, 'a')}}});
    SummaryProvider provider;
    std::vector<std::size_t> attempts;
    CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 0, false,
        [&](std::size_t attempt) { attempts.push_back(attempt); });
    ASSERT_GT(attempts.size(), 1);
    EXPECT_EQ(attempts.size(), provider.inputs.size());
    for (const auto attempt : attempts) EXPECT_EQ(attempt, 1);
}

TEST(CompactionTest, SyntheticSkillTurnKeepsItsPairAndUsesRewrittenUserTextInSummary) {
    auto conversation = history();
    Message user{"skill-user", MessageRole::USER, {TextContent{"//demo value"}}};
    user.providerText = "Call the skill \"demo\" with arguments (JSON data): {}";
    Message call{"skill-call", MessageRole::ASSISTANT,
        {ToolCallContent{"skill-call", "skill", {{"name", "demo"}, {"arguments", json::object()}}}}};
    call.userSkill = "demo";
    conversation.append(user);
    conversation.append(call);
    conversation.append({"skill-result", MessageRole::TOOL, {ToolResultContent{"skill-call", "SKILL_CONTENT", false}}});
    SummaryProvider provider;
    const auto retained = CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 30000);
    const auto wire = retained.conversation.getProviderFormat().dump();
    EXPECT_NE(wire.find("SKILL_CONTENT"), std::string::npos);
    EXPECT_NE(wire.find("function_call_output"), std::string::npos);
    EXPECT_NE(wire.find("Call the skill"), std::string::npos);
    EXPECT_EQ(wire.find("//demo"), std::string::npos);
    CompactionService{}.compact(conversation, provider, SessionInfo{}, "all", {}, 0);
    EXPECT_NE(provider.input.dump().find("Call the skill"), std::string::npos);
    EXPECT_EQ(provider.input.dump().find("//demo"), std::string::npos);
}

TEST(CompactionTest, RetainsLatestExactProjectInstructionsAndCatalogAcrossRepeatedCompaction) {
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append({"system", MessageRole::SYSTEM, {TextContent{"Harness"}}});
    conversation.append({"user", MessageRole::USER, {TextContent{"Old request"}}});
    conversation.append({"project_instructions_abcd_1", MessageRole::SYSTEM, {TextContent{"Old rules"}}});
    conversation.append({"project_instructions_abcd_2", MessageRole::SYSTEM, {TextContent{"Current exact rules"}}});
    conversation.append({"skill_catalog_3", MessageRole::SYSTEM, {TextContent{"Exact catalog"}}});
    conversation.append({"answer", MessageRole::ASSISTANT, {TextContent{"Done"}}});
    conversation.append({"recent", MessageRole::USER, {TextContent{"Continue"}}});
    conversation.append({"recent_answer", MessageRole::ASSISTANT, {TextContent{"Continued"}}});
    SummaryProvider provider;
    auto result = CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 0);
    EXPECT_EQ(result.summary.compaction->retainedMessageIds,
        (std::vector<std::string>{"project_instructions_abcd_2", "skill_catalog_3"}));
    result.conversation.append({"next", MessageRole::USER, {TextContent{"Next"}}});
    result.conversation.append({"next_answer", MessageRole::ASSISTANT, {TextContent{"Done"}}});
    auto second = CompactionService{}.compact(result.conversation, provider, SessionInfo{}, "second", {}, 0);
    EXPECT_EQ(second.summary.compaction->retainedMessageIds, result.summary.compaction->retainedMessageIds);
    EXPECT_NE(second.conversation.getProviderFormat().dump().find("Current exact rules"), std::string::npos);
    EXPECT_EQ(second.conversation.getProviderFormat().dump().find("Old rules"), std::string::npos);
}

TEST(CompactionTest, ExecutionPhaseSurvivesCompactionBeforeTheCurrentRequestAndResume) {
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append({"system", MessageRole::SYSTEM, {TextContent{"Harness"}}});
    conversation.append({"old", MessageRole::USER, {TextContent{"Plan the work"}}});
    const Message phase{"implementation_phase_build_1", MessageRole::SYSTEM,
        {TextContent{prompts::implementationPhaseAnnouncement(ImplementationPhase::EXECUTION)}}};
    conversation.append(phase);
    conversation.append({"current", MessageRole::USER, {TextContent{"Build and validate this step"}}});
    SummaryProvider provider;
    auto result = CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 0, true);
    EXPECT_EQ(result.conversation.getMessages().back().id, phase.id);
    EXPECT_EQ(result.summary.compaction->retainedMessageIds, (std::vector<std::string>{phase.id, "current"}));
    auto restored = CompactionService::restore(conversation.getMessages(), result.summary);
    EXPECT_EQ(json(restored.getMessages().back()), json(phase));
    EXPECT_NE(std::get<TextContent>(restored.getMessages()[restored.getMessages().size() - 2].content[0]).text.find("Current execution mode: sane"), std::string::npos);
    restored.append({"next", MessageRole::USER, {TextContent{"Continue validation"}}});
    const auto repeated = CompactionService{}.compact(restored, provider, SessionInfo{}, "again", {}, 0, true);
    EXPECT_EQ(json(repeated.conversation.getMessages().back()), json(phase));
}

TEST(CompactionTest, LatestReviewOrPlanningPhaseOverridesHistoricalExecutionAfterCompaction) {
    for (const auto reviewId : {"btw-instructions", "implementation_phase_review_2"}) {
        Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
        conversation.append({"system", MessageRole::SYSTEM, {TextContent{"Harness"}}});
        conversation.append({"old", MessageRole::USER, {TextContent{"Build"}}});
        conversation.append({"implementation_phase_build_1", MessageRole::SYSTEM,
            {TextContent{prompts::implementationPhaseAnnouncement(ImplementationPhase::EXECUTION)}}});
        const Message phase{reviewId, MessageRole::SYSTEM,
            {TextContent{prompts::implementationPhaseAnnouncement(ImplementationPhase::REVIEW)}}};
        conversation.append(phase);
        conversation.append({"question", MessageRole::USER, {TextContent{"Inspect the result"}}});
        SummaryProvider provider;
        auto result = CompactionService{}.compact(conversation, provider, SessionInfo{}, "checkpoint", {}, 0, true);
        EXPECT_EQ(json(result.conversation.getMessages().back()), json(phase));
        for (const auto& message : result.conversation.getMessages()) EXPECT_NE(message.id, "implementation_phase_build_1");
        result.conversation.append({"implementation_phase_plan_3", MessageRole::SYSTEM,
            {TextContent{prompts::implementationPhaseAnnouncement(ImplementationPhase::PLANNING)}}});
        result.conversation.append({"plan", MessageRole::USER, {TextContent{"Discuss the plan"}}});
        result = CompactionService{}.compact(result.conversation, provider, SessionInfo{}, "next", {}, 0, true);
        EXPECT_EQ(result.conversation.getMessages().back().id, "implementation_phase_plan_3");
    }
}

TEST(CompactionTest, TriggerUsesTenPercentWithMinimumAndReservesOutputForSharedWindows) {
    EXPECT_EQ(CompactionService::triggerBudget({"model", 128000, {}, {}}), 111616);
    EXPECT_EQ(CompactionService::triggerBudget({"model", 200000, {}, {}}), 180000);
    EXPECT_EQ(CompactionService::triggerBudget({"model", 200000, {}, 32000}), 168000);
    EXPECT_EQ(CompactionService::triggerBudget({"model", 200000, 180000, 32000}), 162000);
    EXPECT_EQ(CompactionService::triggerBudget({"model", 200001, {}, {}}), 180000);
    EXPECT_FALSE(CompactionService::triggerBudget({"unknown", {}, {}, {}}));
    EXPECT_FALSE(CompactionService::triggerBudget({"zero", 0, {}, {}}));
    EXPECT_FALSE(CompactionService::triggerBudget({"small", 16384, {}, {}}));
    EXPECT_FALSE(CompactionService::triggerBudget({"output", 200000, {}, 200000}));
}

TEST(CompactionTest, EstimatesPendingMessagesWithoutMutationAndReusesMeasuredPrefix) {
    auto conversation = history();
    const auto original = json(conversation.getMessages());
    const ModelInfo model{"model", 200000, {}, {}};
    CompactionContext context{conversation, model, {}, 179999, conversation.getMessages().size()};
    CompactionService service;
    EXPECT_FALSE(service.needCompaction(context));
    const std::vector<Message> pending{{"pending", MessageRole::USER, {TextContent{"More work"}}}};
    EXPECT_TRUE(service.needCompaction(context, pending));
    const auto withPending = service.estimateTokens(context, pending);
    EXPECT_EQ(json(conversation.getMessages()), original);
    conversation.append(pending.front());
    EXPECT_EQ(service.estimateTokens(context), withPending);
    context.measuredTokens = 180000;
    context.measuredMessages = conversation.getMessages().size();
    EXPECT_TRUE(service.needCompaction(context));
    context.measuredTokens = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(service.estimateTokens(context, pending), std::numeric_limits<std::uint64_t>::max());
}

TEST(CompactionTest, LocalEstimateIncludesSystemToolsAndIgnoresDisplayMetadata) {
    auto conversation = history();
    const ModelInfo model{"model", 200000, {}, {}};
    const auto base = CompactionService::estimateTokens({conversation, model});
    const std::vector<ToolDefinition> tools{{"read", std::string(4000, 'x'), json::object()}};
    EXPECT_GE(CompactionService::estimateTokens({conversation, model, tools}), base + 1000);
    Conversation decorated(ProviderFormat::OPENAI_RESPONSES);
    for (auto message : conversation.getMessages()) {
        if (message.role == MessageRole::ASSISTANT) message.metadata = GenerationMetadata{"model", "high", 3000};
        if (message.role == MessageRole::TOOL)
            std::get<ToolResultContent>(message.content[0]).metadata = {{"display", std::string(10000, 'x')}};
        decorated.append(message);
    }
    EXPECT_EQ(CompactionService::estimateTokens({decorated, model}), base);
    conversation.append(Message{"extra", MessageRole::SYSTEM, {TextContent{std::string(4000, 's')}}});
    EXPECT_GE(CompactionService::estimateTokens({conversation, model}), base + 1000);
}

TEST(CompactionTest, AutomaticCompactionPreservesCurrentTurnEvenWithZeroRetentionBudget) {
    SummaryProvider provider;
    auto result = CompactionService{}.compact(history(), provider, SessionInfo{}, "auto", {}, 0, true);
    EXPECT_EQ(result.summary.compaction->retainedMessageIds, (std::vector<std::string>{"e", "f", "g", "h"}));
    EXPECT_EQ(provider.input.dump().find("Recent request"), std::string::npos);
    EXPECT_NE(result.conversation.getProviderFormat().dump().find("recent_call"), std::string::npos);
    Conversation first(ProviderFormat::OPENAI_RESPONSES);
    first.append(Message{"s", MessageRole::SYSTEM, {TextContent{"System"}}});
    first.append(Message{"u", MessageRole::USER, {TextContent{"Only turn"}}});
    EXPECT_THROW(CompactionService{}.compact(first, provider, SessionInfo{}, "auto", {}, 0, true), std::runtime_error);
}

TEST(CompactionTest, SummarizesOnlyTheHeadWithoutToolsAndKeepsCompleteRecentTurns) {
    auto conversation = history();
    const json original = conversation.getMessages();
    SummaryProvider provider;
    SessionInfo info;
    info.model = "summary-model";
    info.reasoning = "high";
    info.mode = ExecutionMode::SLOP;
    auto result = CompactionService{}.compact(conversation, provider, info, "checkpoint", {}, 8000);
    EXPECT_EQ(provider.model, "summary-model");
    EXPECT_EQ(provider.reasoning, "high");
    EXPECT_TRUE(provider.tools.empty());
    EXPECT_NE(provider.input.dump().find("Old request"), std::string::npos);
    EXPECT_NE(provider.input.dump().find("summary in English, even when the conversation is in another language"), std::string::npos);
    EXPECT_EQ(provider.input.dump().find("Recent request"), std::string::npos);
    EXPECT_EQ(provider.input.dump().find("OLD_END"), std::string::npos);
    EXPECT_NE(provider.input.dump().find("[truncated]"), std::string::npos);
    EXPECT_EQ(json(conversation.getMessages()), original);
    EXPECT_EQ(result.summary.compaction->retainedMessageIds, (std::vector<std::string>{"e", "f", "g", "h"}));
    const auto& active = result.conversation.getMessages();
    ASSERT_EQ(active.size(), 8);
    EXPECT_EQ(active[0].id, "system");
    EXPECT_EQ(active[1].role, MessageRole::USER);
    EXPECT_EQ(active[2].id, "checkpoint");
    EXPECT_EQ(active[3].id, "e");
    EXPECT_NE(std::get<TextContent>(active.back().content[0]).text.find("slop"), std::string::npos);
    EXPECT_FALSE(result.conversation.getProviderFormat()[2].contains("compaction"));
    EXPECT_EQ(json(json(result.summary).get<Message>()), json(result.summary));
}

TEST(CompactionTest, SmallBudgetSummarizesWholeTurnsAndRepeatedCompactionCarriesPriorSummary) {
    SummaryProvider provider;
    SessionInfo info;
    auto result = CompactionService{}.compact(history(), provider, info, "first", {}, 1);
    EXPECT_TRUE(result.summary.compaction->retainedMessageIds.empty());
    EXPECT_NE(provider.input.dump().find("Recent request"), std::string::npos);
    result.conversation.append(Message{"next", MessageRole::USER, {TextContent{"New work"}}});
    result.conversation.append(Message{"next_answer", MessageRole::ASSISTANT, {TextContent{"New result"}}});
    auto second = CompactionService{}.compact(result.conversation, provider, info, "second");
    EXPECT_NE(provider.input.dump().find("[Previous summary]"), std::string::npos);
    EXPECT_NE(provider.input.dump().find("Continue the work"), std::string::npos);
    EXPECT_NE(provider.input.dump().find("New work"), std::string::npos);
    EXPECT_TRUE(second.summary.compaction->retainedMessageIds.empty());
}

TEST(CompactionTest, RejectsEmptyToolAndIncompleteResponsesAndHonorsCancellation) {
    SummaryProvider provider;
    SessionInfo info;
    auto conversation = history();
    EXPECT_THROW(CompactionService{}.compact(conversation, provider, info, "cancel", [] { return true; }), core::Cancelled);
    EXPECT_TRUE(provider.input.is_null());
    provider.response["output"][0]["content"][0]["text"] = "   ";
    EXPECT_THROW(CompactionService{}.compact(conversation, provider, info, "empty"), std::runtime_error);
    provider.response["output"] = json::array({{{"type", "function_call"}, {"id", "tool"}, {"call_id", "call"},
        {"name", "write"}, {"arguments", "{}"}}});
    EXPECT_THROW(CompactionService{}.compact(conversation, provider, info, "tool"), std::runtime_error);
    provider.response["status"] = "incomplete";
    EXPECT_THROW(CompactionService{}.compact(conversation, provider, info, "partial"), std::exception);
}
}
