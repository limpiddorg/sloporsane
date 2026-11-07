#include <agent-runtime/agent.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <limits>

namespace {
using nlohmann::json;
using namespace std::chrono_literals;

json answer() {
    return {{"id", "resp_1"}, {"status", "completed"}, {"output", json::array({
        {{"type", "message"}, {"id", "msg_1"}, {"role", "assistant"}, {"content", json::array({
            {{"type", "output_text"}, {"text", "Hello"}, {"annotations", json::array()}}
        })}}
    })}};
}

class FakeProvider : public Provider {
public:
    json received;
    json response = answer();
    std::string model;
    std::string reasoning;
    std::string sessionId;
    void setSessionId(const std::string& id) override { sessionId = id; }
    std::optional<std::uint64_t> usedContext = 120;
    std::optional<std::uint64_t> contextLimit = 4096;
    std::optional<std::uint64_t> maxContextLimit;
    ModelInfo getModelInfo(const std::string& id) override {
        if (catalogFailures-- > 0) throw RetryableGenerationError("Model catalog connection problem");
        return {id, contextLimit, {}, {}, maxContextLimit};
    }
    bool fail = false;
    int transientFailures = 0;
    int catalogFailures = 0;
    int calls = 0;
    std::promise<void> entered;
    std::shared_future<void> release;
    std::function<json(const std::vector<ToolDefinition>&)> generateTools;

    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    std::optional<std::uint64_t> getContextLimit(const std::string&) override { return contextLimit; }
    std::optional<std::uint64_t> getUsedContext(const json&) const override { return usedContext; }
    json generate(const json& input, const std::string& selectedModel,
        const std::vector<ToolDefinition>& tools = {}, const std::string& reasoningEffort = "medium") override {
        received = input;
        model = selectedModel;
        reasoning = reasoningEffort;
        ++calls;
        if (transientFailures-- > 0) throw RetryableGenerationError("stream interrupted");
        if (release.valid()) {
            entered.set_value();
            release.wait();
        }
        if (fail) throw std::runtime_error("Fake provider failure");
        if (generateTools) return generateTools(tools);
        return response;
    }
};

TEST(AgentTest, KeepsSessionIdentityWhenReplacingProviders) {
    auto initial = std::make_unique<FakeProvider>();
    auto* first = initial.get();
    Agent agent(std::move(initial), "model");
    agent.setSessionId("conversation");
    EXPECT_EQ(first->sessionId, "conversation");
    auto replacement = std::make_unique<FakeProvider>();
    auto* second = replacement.get();
    agent.setGenerationSettings("model", "medium", 0, std::move(replacement));
    EXPECT_EQ(second->sessionId, "conversation");
    auto queued = std::make_unique<FakeProvider>();
    auto* third = queued.get();
    agent.queueGenerationSettings("model", "medium", {}, std::move(queued));
    agent.finishGenerationSettings();
    EXPECT_EQ(third->sessionId, "conversation");
}

TEST(AgentTest, DisplaysMaximumContextWithoutChangingOperationalLimit) {
    auto provider = std::make_unique<FakeProvider>();
    provider->maxContextLimit = 1050000;
    Agent agent(std::move(provider), "model");
    agent.setContextPreparation([](const CompactionContext& context, Provider&, core::CancellationCheck) -> std::optional<Conversation> {
        EXPECT_EQ(context.model.contextLimit, 4096);
        return std::nullopt;
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Hi"}}});
    agent.run().get();
    EXPECT_EQ(agent.getContextUsage().totalContext, 1050000);
}

TEST(AgentTest, ImplementationProgressIsDisplayOnlyAndDoesNotCompleteTheStep) {
    auto provider = std::make_unique<FakeProvider>();
    provider->generateTools = [call = 0](const std::vector<ToolDefinition>& tools) mutable {
        EXPECT_TRUE(std::any_of(tools.begin(), tools.end(), [](const auto& tool) { return tool.id == "report_implementation_progress"; }));
        if (++call > 2) return answer();
        return json{{"id", "progress-response"}, {"status", "completed"}, {"output", json::array({
            {{"type", "function_call"}, {"id", "progress-call"}, {"call_id", "progress-call"},
             {"name", "report_implementation_progress"},
             {"arguments", json{{"substep", 0}, {"status", call == 1 ? "completed" : "unnecessary"}, {"explanation", "- Provisional result"}}.dump()}}
        })}};
    };
    Agent agent(std::move(provider), "model");
    agent.setImplementationExecution(true, [] { return json::object(); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Build the step"}}});
    agent.run().get();
    bool announced = false, unnecessary = false;
    for (const auto& message : agent.getMessages()) for (const auto& content : message.content) {
        if (const auto* result = std::get_if<ToolResultContent>(&content)) {
            EXPECT_FALSE(result->isError);
            announced |= result->metadata.contains("implementation_progress");
            if (result->metadata.contains("implementation_progress"))
                unnecessary |= result->metadata["implementation_progress"]["status"] == "unnecessary";
            EXPECT_FALSE(result->metadata.contains("implementation_completion"));
        }
    }
    EXPECT_TRUE(announced);
    EXPECT_TRUE(unnecessary);
}

TEST(AgentTest, ImplementationCapturesValidationAndCompletionWithoutFinalHunkAssignments) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    provider->generateTools = [call = 0](const std::vector<ToolDefinition>& tools) mutable {
        if (++call > 1) return answer();
        const auto complete = std::find_if(tools.begin(), tools.end(), [](const auto& tool) { return tool.id == "complete_implementation_step"; });
        EXPECT_NE(complete, tools.end());
        EXPECT_FALSE(complete->parameters["properties"]["substeps"]["items"]["properties"].contains("changes"));
        EXPECT_TRUE(complete->parameters["properties"].contains("conclusion"));
        EXPECT_NE(complete->description.find("demonstrated to predate"), std::string::npos);
        EXPECT_NE(complete->description.find("not end with a prose audit"), std::string::npos);
        EXPECT_NE(complete->description.find("validation does not require new checks"), std::string::npos);
        const auto progress = std::find_if(tools.begin(), tools.end(), [](const auto& tool) { return tool.id == "report_implementation_progress"; });
        EXPECT_NE(progress, tools.end());
        if (progress != tools.end()) {
            EXPECT_NE(progress->description.find("Reuse still-valid earlier results"), std::string::npos);
            EXPECT_NE(progress->description.find("do not reread all files"), std::string::npos);
            EXPECT_NE(progress->description.find("If checks are needed, run them in this phase"), std::string::npos);
            EXPECT_NE(progress->description.find("complete the step without inventing checks"), std::string::npos);
        }
        json output = json::array();
        for (const auto& status : {"started", "completed", "validating", "correcting", "completed"}) {
            const auto id = "progress-" + std::to_string(output.size());
            output.push_back({{"type", "function_call"}, {"id", id}, {"call_id", id}, {"name", "report_implementation_progress"},
                {"arguments", json{{"substep", std::string(status) == "validating" ? json(nullptr) : json(0)},
                    {"status", status}, {"explanation", "- Concrete result"}}.dump()}});
        }
        output.push_back({{"type", "function_call"}, {"id", "complete"}, {"call_id", "complete"}, {"name", "complete_implementation_step"},
            {"arguments", json{{"explanation", "Checks passed"}, {"substeps", {{{"explanation", "Corrected model"}}}},
                {"conclusion", "Model implemented. Targeted checks pass; unrelated baseline failures remain."}}.dump()}});
        return json{{"id", "constructed"}, {"status", "completed"}, {"output", output}};
    };
    std::vector<std::string> captured;
    Agent agent(std::move(provider), "model");
    agent.setImplementationExecution(true, {}, [&](const json& progress) {
        captured.push_back(progress.at("status"));
        if (captured.back() == "finished") {
            EXPECT_EQ(progress["completion"]["substeps"].size(), 1);
            EXPECT_NE(progress["completion"]["conclusion"].get<std::string>().find("baseline failures"), std::string::npos);
        }
        return static_cast<std::int64_t>(captured.size());
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Build and validate"}}});
    agent.run().get();
    EXPECT_EQ(fake->calls, 1); // Submission itself ends the turn, without a prose completion call.
    EXPECT_EQ(captured, (std::vector<std::string>{"started", "completed", "validating", "correcting", "completed", "finished"}));
    unsigned boundaries = 0, completions = 0;
    for (const auto& message : agent.getMessages()) for (const auto& content : message.content)
        if (const auto* result = std::get_if<ToolResultContent>(&content)) {
            EXPECT_FALSE(result->isError);
            boundaries += result->metadata.contains("implementation_boundary");
            completions += result->metadata.contains("implementation_completion");
            if (result->metadata.contains("implementation_progress") && result->metadata["implementation_progress"]["status"] == "validating") {
                EXPECT_NE(result->output.get<std::string>().find("call complete_implementation_step before ending normally"), std::string::npos);
                EXPECT_NE(result->output.get<std::string>().find("Genuine blockers"), std::string::npos);
                EXPECT_NE(result->output.get<std::string>().find("Reuse still-valid earlier results"), std::string::npos);
                EXPECT_NE(result->output.get<std::string>().find("last step is not a fresh global audit"), std::string::npos);
                EXPECT_NE(result->output.get<std::string>().find("Entering validation does not require new checks"), std::string::npos);
            }
        }
    EXPECT_EQ(boundaries, 6); EXPECT_EQ(completions, 1);
}

TEST(AgentTest, ImplementationCannotPublishCompletionWhenBoundaryCaptureFails) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    provider->generateTools = [call = 0](const std::vector<ToolDefinition>&) mutable {
        if (++call > 1) return answer();
        return json{{"id", "blocked"}, {"status", "completed"}, {"output", json::array({
            {{"type", "function_call"}, {"id", "complete"}, {"call_id", "complete"}, {"name", "complete_implementation_step"},
                {"arguments", json{{"explanation", "Claimed done"}, {"substeps", {{{"explanation", "Claimed checked"}}}}}.dump()}}
        })}};
    };
    Agent agent(std::move(provider), "model");
    agent.setImplementationExecution(true, {}, [](const json&) -> std::int64_t {
        throw std::logic_error("Technical validation has not started");
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Build the step"}}});
    agent.run().get();
    EXPECT_EQ(fake->calls, 2); // A failed submission must still let the model react.
    bool sawError = false;
    for (const auto& message : agent.getMessages()) for (const auto& content : message.content)
        if (const auto* result = std::get_if<ToolResultContent>(&content)) {
            sawError |= result->isError;
            EXPECT_FALSE(result->metadata.contains("implementation_completion"));
        }
    EXPECT_TRUE(sawError);
}

TEST(AgentTest, SuccessfulCompletionSkipsTrailingToolsAndPreservesConclusion) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    const json completion{{"explanation", "Checks passed"}, {"substeps", json::array({json::object()})},
        {"conclusion", "The entire implementation is ready; checks passed."}};
    provider->response = {{"id", "finished"}, {"status", "completed"}, {"output", json::array({
        {{"type", "function_call"}, {"call_id", "complete"}, {"name", "complete_implementation_step"}, {"arguments", completion.dump()}},
        {{"type", "function_call"}, {"call_id", "trailing"}, {"name", "report_implementation_progress"},
            {"arguments", json{{"substep", 0}, {"status", "started"}, {"explanation", "Do not execute"}}.dump()}}
    })}};
    Agent agent(std::move(provider), "model");
    unsigned captures = 0;
    agent.setImplementationExecution(true, {}, [&](const json& progress) {
        ++captures;
        EXPECT_EQ(progress["status"], "finished");
        EXPECT_EQ(progress["completion"]["conclusion"], completion["conclusion"]);
        EXPECT_EQ(progress["completion"]["substeps"][0]["explanation"], "");
        return 1;
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Build the last step"}}});
    agent.run().get();
    EXPECT_EQ(fake->calls, 1);
    EXPECT_EQ(captures, 1);
    const auto messages = agent.getMessages();
    ASSERT_EQ(messages.size(), 4);
    const auto& submitted = std::get<ToolResultContent>(messages[2].content.front());
    EXPECT_FALSE(submitted.isError);
    EXPECT_TRUE(messages[2].metadata.has_value());
    EXPECT_EQ(submitted.metadata["implementation_completion"]["conclusion"], completion["conclusion"]);
    EXPECT_TRUE(std::get<ToolResultContent>(messages[3].content.front()).isError);
}

TEST(AgentTest, RetriesGenerationWithoutDuplicatingTheUserMessage) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->transientFailures = 1;
    Agent agent(std::move(provider), "model");
    std::vector<AgentEvent> events;
    agent.setEventHandler([&](auto event) { events.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Hi"}}});
    agent.run().get();
    EXPECT_EQ(fake->calls, 2);
    EXPECT_EQ(agent.getMessages().size(), 2);
    ASSERT_EQ(events.size(), 5);
    EXPECT_EQ(events[1].type, AgentEventType::EXECUTION_RETRY);
    EXPECT_EQ(events[1].retry->attempt, 1);
    EXPECT_EQ(events[2].retry->attempt, 0);
    EXPECT_EQ(events.back().type, AgentEventType::EXECUTION_COMPLETED);
}

TEST(AgentTest, RetriesModelCatalogBeforeGenerating) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->catalogFailures = 1;
    Agent agent(std::move(provider), "model");
    std::vector<GenerationRetryStatus> statuses;
    agent.setEventHandler([&](const auto& event) {
        if (event.retry) statuses.push_back(*event.retry);
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Hi"}}});
    agent.run().get();
    EXPECT_EQ(fake->calls, 1);
    ASSERT_EQ(statuses.size(), 2);
    EXPECT_EQ(statuses[0].reason, "Connection problem");
    EXPECT_EQ(statuses[1].attempt, 0);
    EXPECT_EQ(agent.getMessages().size(), 2);
}

TEST(AgentTest, CancellationDuringRetriesKeepsTheAcceptedMessage) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->transientFailures = 20;
    Agent agent(std::move(provider), "model");
    std::vector<AgentEvent> events;
    agent.setEventHandler([&](auto event) {
        if (event.retry && event.retry->seconds) agent.cancel();
        events.push_back(std::move(event));
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Hi"}}});
    agent.run().get();
    EXPECT_EQ(fake->calls, 2);
    const auto messages = agent.getMessages();
    ASSERT_EQ(messages.size(), 2); // User message plus the existing interruption marker.
    EXPECT_EQ(messages[0].id, "user");
    EXPECT_EQ(messages[1].role, MessageRole::SYSTEM);
    EXPECT_EQ(events.back().type, AgentEventType::EXECUTION_CANCELLED);
}

TEST(AgentTest, UserContextBudgetReachesCompactionAndDisplay) {
    auto provider = std::make_unique<FakeProvider>();
    provider->contextLimit = 272000;
    provider->maxContextLimit = 872000;
    Agent agent(std::move(provider), "model");
    EXPECT_THROW(agent.setContextLimit(32767), std::invalid_argument);
    agent.setContextLimit(150000);
    agent.setContextPreparation([](const CompactionContext& context, Provider&, core::CancellationCheck) -> std::optional<Conversation> {
        EXPECT_EQ(context.model.contextLimit, 150000);
        EXPECT_EQ(context.model.inputLimit, 150000);
        EXPECT_EQ(context.model.maxContextLimit, 150000);
        return {};
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Hi"}}});
    agent.run().get();
    EXPECT_EQ(agent.getContextUsage().totalContext, 150000);
    agent.setContextLimit(872001);
    EXPECT_THROW(agent.run().get(), std::runtime_error);
    agent.setContextLimit({});
    agent.setContextPreparation({});
    agent.run().get();
    EXPECT_EQ(agent.getContextUsage().totalContext, 872000);
}

TEST(AgentTest, RunsAsynchronouslyAndForbidsConcurrentEdits) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    auto entered = fake->entered.get_future();
    std::promise<void> release;
    fake->release = release.get_future().share();
    Agent agent(std::move(provider), "openai/gpt-5.6-luna");
    const Message user{"user_1", MessageRole::USER, {TextContent{"Hi"}}};
    agent.append(user);
    const auto execution = agent.run();
    const auto started = entered.wait_for(2s);
    // Always release the worker before any fatal assertion can destroy the agent.
    if (started == std::future_status::ready) {
        EXPECT_TRUE(agent.isRunning());
        EXPECT_EQ(execution.wait_for(0s), std::future_status::timeout);
        EXPECT_THROW(agent.append(user), std::logic_error);
        EXPECT_THROW(agent.run(), std::logic_error);
        EXPECT_THROW(agent.replaceConversation(Conversation(ProviderFormat::OPENAI_RESPONSES)), std::logic_error);
        EXPECT_THROW(agent.setContextPreparation({}), std::logic_error);
        EXPECT_EQ(agent.getMessages().size(), 1);
    }
    release.set_value();
    execution.get();
    ASSERT_EQ(started, std::future_status::ready);
    EXPECT_FALSE(agent.isRunning());
    ASSERT_EQ(agent.getMessages().size(), 2);
    EXPECT_EQ(fake->model, "openai/gpt-5.6-luna");
    EXPECT_EQ(fake->received[0]["content"], "Hi");
    EXPECT_EQ(std::get<TextContent>(agent.getMessages().back().content[0]).text, "Hello");
}

TEST(AgentTest, SendsFullHistoryOnNextTurn) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    Agent agent(std::move(provider), "test-model");
    agent.append(Message{"user_1", MessageRole::USER, {TextContent{"Hi"}}});
    agent.run().get();
    agent.append(Message{"user_2", MessageRole::USER, {TextContent{"Again"}}});
    agent.run().get();
    ASSERT_EQ(fake->received.size(), 3);
    EXPECT_EQ(fake->received[1]["role"], "assistant");
    EXPECT_EQ(fake->received[2]["content"], "Again");
    EXPECT_EQ(agent.getMessages().size(), 4);
}

TEST(AgentTest, ReasoningChangesApplyToNextRunWithoutChangingCurrentMetadata) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    auto entered = fake->entered.get_future();
    std::promise<void> release;
    fake->release = release.get_future().share();
    Agent agent(std::move(provider), "test-model", {}, {}, "high");
    agent.append(Message{"user_1", MessageRole::USER, {TextContent{"First"}}});
    const auto execution = agent.run();
    const auto started = entered.wait_for(2s);
    agent.setReasoningEffort("low");
    release.set_value();
    execution.get();
    ASSERT_EQ(started, std::future_status::ready);
    EXPECT_EQ(fake->reasoning, "high");
    ASSERT_TRUE(agent.getMessages().back().metadata);
    EXPECT_EQ(agent.getMessages().back().metadata->reasoning, "high");
    fake->release = {};
    agent.append(Message{"user_2", MessageRole::USER, {TextContent{"Next"}}});
    agent.run().get();
    EXPECT_EQ(fake->reasoning, "low");
    EXPECT_EQ(agent.getMessages().back().metadata->reasoning, "low");
}

TEST(AgentTest, LiveModelSelectionKeepsInflightMetadataAndChangesNextCallInSameRun) {
    auto first = std::make_unique<FakeProvider>();
    auto entered = first->entered.get_future();
    std::promise<void> release;
    first->release = release.get_future().share();
    first->response = {{"id", "tools"}, {"status", "completed"}, {"output", json::array({{
        {"type", "function_call"}, {"id", "tool"}, {"call_id", "call"}, {"name", "unknown"}, {"arguments", "{}"}}})}};
    Agent agent(std::move(first), "old-model", {}, {}, "high");
    agent.append({"user", MessageRole::USER, {TextContent{"Continue this task"}}});
    auto execution = agent.run();
    const auto started = entered.wait_for(2s);
    auto second = std::make_unique<FakeProvider>();
    second->contextLimit = 65536;
    auto* replacement = second.get();
    agent.queueGenerationSettings("new-model", "low", 32768, std::move(second));
    EXPECT_EQ(agent.executionGeneration().model, "old-model");
    release.set_value();
    execution.get();
    ASSERT_EQ(started, std::future_status::ready);
    const auto messages = agent.getMessages();
    ASSERT_EQ(messages.size(), 4);
    EXPECT_EQ(messages[1].modelCall->model, "old-model");
    EXPECT_EQ(messages[1].modelCall->reasoning, "high");
    EXPECT_EQ(messages.back().modelCall->model, "new-model");
    EXPECT_EQ(messages.back().metadata->model, "new-model");
    EXPECT_EQ(messages.back().metadata->reasoning, "low");
    EXPECT_EQ(replacement->calls, 1);
    EXPECT_NE(replacement->received.dump().find("Continue this task"), std::string::npos);
    EXPECT_NE(replacement->received.dump().find("call"), std::string::npos);
    EXPECT_EQ(agent.getContextUsage().totalContext, 32768);
}

TEST(AgentTest, SelectionDuringFinalCallDoesNotRelabelItsAnswer) {
    auto first = std::make_unique<FakeProvider>();
    auto entered = first->entered.get_future();
    std::promise<void> release;
    first->release = release.get_future().share();
    Agent agent(std::move(first), "old-model");
    agent.append({"user", MessageRole::USER, {TextContent{"Question"}}});
    auto execution = agent.run();
    const auto started = entered.wait_for(2s);
    agent.queueGenerationSettings("new-model", "low", {}, std::make_unique<FakeProvider>());
    release.set_value();
    execution.get();
    ASSERT_EQ(started, std::future_status::ready);
    agent.finishGenerationSettings();
    EXPECT_EQ(agent.getMessages().back().metadata->model, "old-model");
    EXPECT_EQ(agent.executionGeneration().model, "new-model");
    agent.append({"next", MessageRole::USER, {TextContent{"Next"}}});
    agent.run().get();
    EXPECT_EQ(agent.getMessages().back().metadata->model, "new-model");
}

TEST(AgentTest, PropagatesFailureAndAllowsRetry) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->fail = true;
    Agent agent(std::move(provider), "test-model");
    EXPECT_THROW(agent.run(), std::logic_error);
    agent.append(Message{"user_1", MessageRole::USER, {TextContent{"Hi"}}});
    EXPECT_THROW(agent.run().get(), std::runtime_error);
    EXPECT_FALSE(agent.isRunning());
    EXPECT_EQ(agent.getMessages().size(), 1);
    fake->fail = false;
    agent.run().get();
    EXPECT_EQ(agent.getMessages().size(), 2);
}

TEST(AgentTest, RequiresProviderAndModel) {
    EXPECT_THROW((Agent{nullptr, "test-model"}), std::invalid_argument);
    EXPECT_THROW((Agent{std::make_unique<FakeProvider>(), ""}), std::invalid_argument);
}

TEST(AgentTest, TracksLatestContextRestoresItAndIncludesItInEveryEvent) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    Agent agent(std::move(provider), "test-model", {}, {}, "high");
    agent.restoreContextUsage({75, 2048});
    std::vector<AgentEvent> events;
    agent.setEventHandler([&](AgentEvent event) { events.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Hello"}}});
    EXPECT_EQ(events.back().context.lastTurnUsedContext, 75);
    EXPECT_EQ(events.back().context.totalContext, 2048);
    agent.run().get();
    EXPECT_EQ(fake->reasoning, "high");
    EXPECT_EQ(agent.getContextUsage().lastTurnUsedContext, 120);
    EXPECT_EQ(events.back().context.totalContext, 4096);
    fake->usedContext = 200;
    agent.append(Message{"user2", MessageRole::USER, {TextContent{"Again"}}});
    agent.run().get();
    EXPECT_EQ(agent.getContextUsage().lastTurnUsedContext, 200); // Not 120 + 200.
    fake->fail = true;
    EXPECT_THROW(agent.run().get(), std::runtime_error);
    EXPECT_EQ(events.back().context.lastTurnUsedContext, 200);
    fake->fail = false;
    fake->usedContext.reset();
    agent.run().get();
    EXPECT_FALSE(events.back().context.lastTurnUsedContext);
    EXPECT_EQ(events.back().context.totalContext, 4096);
}

TEST(AgentTest, RecordsTurnDurationAndSettingsOnFinalMessageAndTerminalEvent) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    Agent agent(std::move(provider), "chosen-model", {}, {}, "high");
    std::vector<AgentEvent> events;
    agent.setEventHandler([&](AgentEvent event) { events.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Hello"}}});
    agent.run(std::chrono::steady_clock::now() - 2s).get();
    const auto final = agent.getMessages().back();
    ASSERT_TRUE(final.metadata);
    ASSERT_TRUE(final.elapsedMs);
    EXPECT_GE(*final.elapsedMs, 2000);
    EXPECT_EQ(final.metadata->model, "chosen-model");
    EXPECT_EQ(final.metadata->reasoning, "high");
    EXPECT_GE(final.metadata->durationMs, 2000);
    ASSERT_TRUE(final.modelCall);
    EXPECT_EQ(final.modelCall->model, "chosen-model");
    EXPECT_EQ(final.modelCall->reasoning, "high");
    EXPECT_FALSE(final.modelCall->outputTokens);
    EXPECT_LT(final.modelCall->durationMs, final.metadata->durationMs);
    ASSERT_TRUE(events.back().metadata);
    EXPECT_EQ(events.back().metadata->durationMs, final.metadata->durationMs);
    ASSERT_TRUE(events[1].messages[0].metadata);
    EXPECT_EQ(events[1].messages[0].metadata->durationMs, final.metadata->durationMs);
    agent.append(Message{"user2", MessageRole::USER, {TextContent{"Again"}}});
    agent.run().get();
    EXPECT_EQ(fake->received.dump().find("duration_ms"), std::string::npos);
    EXPECT_EQ(fake->received.dump().find("elapsed_ms"), std::string::npos);
    EXPECT_EQ(fake->received.dump().find("model_call"), std::string::npos);
    fake->fail = true;
    EXPECT_THROW(agent.run().get(), std::runtime_error);
    EXPECT_TRUE(events.back().metadata);
}

TEST(AgentTest, OutputUsageIsSeparateAndMissingOrInvalidCountsStayUnknown) {
    FakeProvider provider;
    EXPECT_EQ(provider.getOutputTokens({{"usage", {{"input_tokens", 9999}, {"output_tokens", 123}}}}), 123);
    EXPECT_EQ(provider.getOutputTokens({{"usage", {{"output_tokens", 0}}}}), 0);
    EXPECT_FALSE(provider.getOutputTokens(json::object()));
    EXPECT_FALSE(provider.getOutputTokens({{"usage", nullptr}}));
    for (const auto& value : {json(nullptr), json(-1), json(1.5), json("123")})
        EXPECT_FALSE(provider.getOutputTokens({{"usage", {{"output_tokens", value}}}}));
}

TEST(AgentTest, ModelCallMetadataRoundTripsAndAggregatesWithoutEstimatingUnknownUsage) {
    ModelCallMetadata sum{"model", "high", 1000, 10};
    sum.accumulate({"model", "high", 3000, 90});
    EXPECT_EQ(sum.durationMs, 4000);
    EXPECT_EQ(sum.outputTokens, 100);
    EXPECT_EQ(json(json(sum).get<ModelCallMetadata>()), json(sum));
    sum.accumulate({"model", "high", 1000, {}});
    EXPECT_FALSE(sum.outputTokens);
    sum.accumulate({"model", "high", 1000, 12});
    EXPECT_FALSE(sum.outputTokens);
    EXPECT_EQ(sum.durationMs, 6000);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    ModelCallMetadata overflow{"model", "high", maximum, 1};
    overflow.accumulate({"model", "high", 1, 1});
    EXPECT_FALSE(overflow.outputTokens);
    for (const auto& field : {"duration_ms", "output_tokens"}) {
        auto invalid = json(ModelCallMetadata{"model", "high", 1, 1});
        invalid[field] = -1;
        EXPECT_THROW(invalid.get<ModelCallMetadata>(), std::invalid_argument);
    }
}

class TimedTool : public Tool {
public:
    mutable int executions = 0;
    ToolDefinition getDefinition() const override { return {"timed", "Test", json::object()}; }
    ToolResult execute(const json&, const ToolContext&) const override {
        ++executions;
        std::this_thread::sleep_for(80ms);
        return {"Test", "Done"};
    }
};

class TimedProvider : public Provider {
public:
    int calls = 0;
    bool failAfterTool = false;
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    json generate(const json&, const std::string&, const std::vector<ToolDefinition>&, const std::string&) override {
        std::this_thread::sleep_for(20ms);
        if (++calls == 1) return {{"id", "tool-response"}, {"status", "completed"}, {"usage", {{"output_tokens", 40}}}, {"output", json::array({
            {{"type", "function_call"}, {"id", "item"}, {"call_id", "call"}, {"name", "timed"}, {"arguments", "{}"}}
        })}};
        if (calls == 2 && failAfterTool) throw RetryableGenerationError("broken stream");
        auto result = answer();
        result["usage"] = {{"output_tokens", 160}};
        return result;
    }
};

TEST(AgentTest, ModelTimingExcludesToolsPermissionChecksAndContextPreparation) {
    ToolRegistry tools;
    tools.add(std::make_unique<TimedTool>());
    Agent agent(std::make_unique<TimedProvider>(), "model", std::move(tools), {}, "high");
    agent.setPermissionHandler([](const auto&, const auto&) {
        std::this_thread::sleep_for(80ms);
        return PermissionAnswer{};
    }, std::filesystem::current_path());
    agent.setContextPreparation([](const auto&, auto&, auto) -> std::optional<Conversation> {
        std::this_thread::sleep_for(80ms);
        return {};
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run"}}});
    agent.run().get();
    const auto messages = agent.getMessages();
    ASSERT_EQ(messages.size(), 4);
    ASSERT_TRUE(messages[1].modelCall);
    ASSERT_TRUE(messages[3].modelCall);
    EXPECT_FALSE(messages[2].modelCall);
    EXPECT_EQ(messages[1].modelCall->outputTokens, 40);
    EXPECT_EQ(messages[3].modelCall->outputTokens, 160);
    EXPECT_GE(messages[1].modelCall->durationMs, 20);
    const auto modelMs = messages[1].modelCall->durationMs + messages[3].modelCall->durationMs;
    EXPECT_GE(messages[3].metadata->durationMs, modelMs + 300);
    EXPECT_EQ(json(json(messages[1]).get<Message>()), json(messages[1]));
}

TEST(AgentTest, RetryAfterAToolDoesNotExecuteItAgain) {
    ToolRegistry tools;
    auto tool = std::make_unique<TimedTool>();
    auto* recordedTool = tool.get();
    tools.add(std::move(tool));
    auto provider = std::make_unique<TimedProvider>();
    auto* recordedProvider = provider.get();
    provider->failAfterTool = true;
    Agent agent(std::move(provider), "model", std::move(tools));
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run"}}});
    agent.run().get();
    EXPECT_EQ(recordedTool->executions, 1);
    EXPECT_EQ(recordedProvider->calls, 3);
    EXPECT_EQ(agent.getMessages().size(), 4);
}

class CatalogTool : public Tool {
public:
    CatalogTool(std::string id, int& executions) : id(std::move(id)), executions(executions) {}
    ToolDefinition getDefinition() const override { return {id, "test", {{"type", "object"}}}; }
    ToolResult execute(const json&, const ToolContext&) const override { ++executions; return {id, "ok"}; }
private:
    std::string id;
    int& executions;
};

TEST(AgentTest, DynamicRegistryReplacementIsAtomicAndCannotShadowBuiltins) {
    int executions = 0;
    ToolRegistry registry;
    registry.add(std::make_unique<CatalogTool>("builtin", executions));
    const auto replace = [&](const std::vector<std::string>& ids) {
        std::vector<std::unique_ptr<Tool>> tools;
        for (const auto& id : ids) tools.push_back(std::make_unique<CatalogTool>(id, executions));
        registry.replaceDynamic(std::move(tools));
    };
    replace({"old"});
    EXPECT_THROW(replace({"new", "builtin"}), std::invalid_argument);
    EXPECT_NO_THROW(registry.get("old"));
    EXPECT_THROW(registry.get("new"), std::out_of_range);
    EXPECT_THROW(replace({"new", "new"}), std::invalid_argument);
    EXPECT_NO_THROW(registry.get("old"));
    replace({"new"});
    EXPECT_THROW(registry.get("old"), std::out_of_range);
    EXPECT_NO_THROW(registry.get("new"));
    registry.replaceDynamic({});
    ASSERT_EQ(registry.getDefinitions().size(), 1);
    EXPECT_EQ(registry.getDefinitions().front().id, "builtin");
}

TEST(AgentTest, DynamicCatalogueIsStableThroughAResponseAndRefreshedBeforeTheNextGeneration) {
    int executions = 0, loads = 0, generations = 0, prepared = 0;
    std::string current = "old";
    auto provider = std::make_unique<FakeProvider>();
    provider->generateTools = [&](const auto& tools) -> json {
        EXPECT_EQ(tools.size(), 1);
        if (++generations == 1) {
            EXPECT_EQ(tools.front().id, "old");
            current = "new"; // Simulates a management update during generation.
            return {{"id", "calls"}, {"status", "completed"}, {"output", json::array({
                {{"type", "function_call"}, {"id", "a"}, {"call_id", "a"}, {"name", "old"}, {"arguments", "{}"}},
                {{"type", "function_call"}, {"id", "b"}, {"call_id", "b"}, {"name", "old"}, {"arguments", "{}"}}
            })}};
        }
        EXPECT_EQ(tools.front().id, "new");
        EXPECT_EQ(executions, 2);
        return answer();
    };
    Agent agent(std::move(provider), "model");
    agent.setToolSource([&](core::CancellationCheck cancelled) {
        core::checkCancellation(cancelled);
        ++loads;
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::make_unique<CatalogTool>(current, executions));
        return tools;
    });
    agent.setContextPreparation([&](const CompactionContext& context, auto&, auto) -> std::optional<Conversation> {
        ++prepared;
        EXPECT_EQ(context.tools.front().id, prepared == 1 ? "old" : "new");
        return {};
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run"}}});
    agent.run().get();
    EXPECT_EQ(loads, 2);
    EXPECT_EQ(generations, 2);
    EXPECT_EQ(prepared, 2);
    EXPECT_EQ(executions, 2);
}

TEST(AgentTest, DynamicCatalogueIsNotReloadedDuringGenerationRetries) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->transientFailures = 1;
    Agent agent(std::move(provider), "model");
    int loads = 0;
    agent.setToolSource([&](auto) { ++loads; return std::vector<std::unique_ptr<Tool>>{}; });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run"}}});
    agent.run().get();
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(fake->calls, 2);
}

TEST(AgentTest, ToolSourceFailureStopsBeforeGeneratingAndCanBeReplacedWhileIdle) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    Agent agent(std::move(provider), "model");
    agent.setToolSource([](auto) -> std::vector<std::unique_ptr<Tool>> { throw std::runtime_error("catalogue failed"); });
    std::vector<AgentEvent> events;
    agent.setEventHandler([&](auto event) { events.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run"}}});
    EXPECT_THROW(agent.run().get(), std::runtime_error);
    EXPECT_EQ(fake->calls, 0);
    EXPECT_EQ(events.back().type, AgentEventType::EXECUTION_FAILED);
    agent.setToolSource({});
    agent.run().get();
    EXPECT_EQ(fake->calls, 1);
}

TEST(AgentTest, ToolSourceCannotBeReplacedWhileItsWorkerIsActive) {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    Agent agent(std::move(provider), "model");
    std::promise<void> entered, release;
    auto ready = entered.get_future();
    auto proceed = release.get_future();
    agent.setToolSource([&](core::CancellationCheck cancelled) {
        entered.set_value();
        proceed.wait();
        core::checkCancellation(cancelled);
        return std::vector<std::unique_ptr<Tool>>{};
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run"}}});
    const auto execution = agent.run();
    ready.wait();
    EXPECT_THROW(agent.setToolSource({}), std::logic_error);
    agent.cancel();
    release.set_value();
    EXPECT_NO_THROW(execution.get());
    EXPECT_FALSE(agent.isRunning());
    EXPECT_EQ(fake->calls, 0);
}

TEST(AgentTest, ApprovedReplayResolvesToolsFromTheCurrentDynamicCatalogue) {
    int executions = 0, generations = 0;
    auto provider = std::make_unique<FakeProvider>();
    provider->generateTools = [&](const auto&) -> json {
        if (++generations > 1) return answer();
        return {{"id", "replay"}, {"status", "completed"}, {"output", json::array({
            {{"type", "function_call"}, {"id", "item"}, {"call_id", "call"},
             {"name", "rerun_approved_action"}, {"arguments", "{\"id\":\"grant\"}"}}
        })}};
    };
    ToolContext context;
    context.ask = [](const auto&) {};
    Agent agent(std::move(provider), "model", {}, context);
    agent.setApprovedActionResolver([](const auto& id) { return ApprovedToolAction{id, "dynamic", json::object(), {}, {}}; });
    agent.setToolSource([&](auto) {
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::make_unique<CatalogTool>("dynamic", executions));
        return tools;
    });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Run"}}});
    agent.run().get();
    EXPECT_EQ(executions, 1);
    EXPECT_EQ(generations, 2);
}
}
