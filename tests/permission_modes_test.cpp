#include <daemon/permissions/permission_manager.hpp>
#include <daemon/permissions/permission_verifier.hpp>
#include <daemon/providers/provider_connections.hpp>
#include <daemon/integration/integration.hpp>
#include <daemon/prompts.hpp>
#include <agent-runtime/core/cancellation.hpp>
#include <agent-runtime/skills/skill_catalog.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <future>
#include <fstream>
#include <cstdlib>
#include <optional>
#include <limits>

namespace {
using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

class PermissionModesTest : public testing::Test {
protected:
    fs::path directory, configurationDirectory;
    std::optional<std::string> previousConfig;
    void SetUp() override {
        std::string pattern = "/tmp/sos-policy-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = fs::canonical(pattern);
        if (const auto* value = std::getenv("XDG_CONFIG_HOME")) previousConfig = value;
        configurationDirectory = directory.string() + "-config";
        fs::create_directories(configurationDirectory);
        setenv("XDG_CONFIG_HOME", configurationDirectory.c_str(), 1);
    }
    void TearDown() override {
        if (previousConfig) setenv("XDG_CONFIG_HOME", previousConfig->c_str(), 1); else unsetenv("XDG_CONFIG_HOME");
        fs::remove_all(configurationDirectory);
        fs::remove_all(directory);
    }
    ToolPermissionRequest edit() {
        return {"edit", {"file"}, {"*"}, {{"arguments", {{"filePath", "file"}, {"content", "hello"}}}}, "c", "write"};
    }
};

TEST_F(PermissionModesTest, DangerBypassesPromptsButNeverSaneRestrictions) {
    PermissionManager manager([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    manager.configure(directory, ExecutionMode::SLOP);
    manager.setPermissionMode(PermissionMode::DANGER);
    EXPECT_EQ(manager.ask(edit(), [] {}).decision, PermissionDecision::ALLOW);
    auto bash = ToolPermissionRequest{"bash", {}, {}, {}, "b", "bash", false};
    EXPECT_EQ(manager.ask(bash, [] {}).decision, PermissionDecision::ALLOW);
    manager.setMode(ExecutionMode::SANE);
    EXPECT_THROW(manager.ask(edit(), [] {}), std::runtime_error);
    EXPECT_THROW(manager.ask(bash, [] {}), std::runtime_error);
    EXPECT_EQ(manager.ask({"external_directory", {}, {}, {{"parentDir", "/"}}, "r", "read", true}, [] {}).decision,
        PermissionDecision::ALLOW);
}

TEST_F(PermissionModesTest, ImplementationExecutionAllowsWritesWithoutChangingSaneMode) {
    PermissionManager manager([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    manager.configure(directory, ExecutionMode::SANE);
    manager.setPermissionMode(PermissionMode::DANGER);
    manager.setImplementationPhase(ImplementationPhase::EXECUTION);
    EXPECT_EQ(manager.mode(), ExecutionMode::SANE);
    EXPECT_EQ(manager.ask(edit(), [] {}).decision, PermissionDecision::ALLOW);
    EXPECT_EQ(manager.ask({"bash", {}, {}, {}, "b", "bash", false}, [] {}).decision, PermissionDecision::ALLOW);
    EXPECT_EQ(manager.ask({"mcp", {}, {}, {}, "m", "mcp.write", false}, [] {}).decision, PermissionDecision::ALLOW);
    manager.setImplementationPhase(ImplementationPhase::PLANNING);
    EXPECT_THROW(manager.ask(edit(), [] {}), std::runtime_error);
}

TEST_F(PermissionModesTest, WaitNeverBypassesSaneOrReviewRestrictions) {
    PermissionManager manager([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    manager.configure(directory, ExecutionMode::SANE);
    manager.setPermissionMode(PermissionMode::DANGER);
    auto shell = ToolPermissionRequest{"bash", {}, {}, {}, "w", "wait", false};
    auto external = ToolPermissionRequest{"external_directory", {}, {}, {{"parentDir", "/"}}, "w", "wait", false};
    EXPECT_THROW(manager.ask(shell, [] {}), std::runtime_error);
    EXPECT_THROW(manager.ask(external, [] {}), std::runtime_error);
    shell.readOnly = true;
    EXPECT_EQ(manager.ask(shell, [] {}).decision, PermissionDecision::ALLOW);
    manager.setMode(ExecutionMode::SLOP);
    manager.setImplementationPhase(ImplementationPhase::REVIEW);
    shell.readOnly = false;
    EXPECT_THROW(manager.ask(shell, [] {}), std::runtime_error);
    EXPECT_THROW(manager.ask(external, [] {}), std::runtime_error);
}

TEST_F(PermissionModesTest, ImplementationAutoAcceptanceContinuesWithoutHumanPrompt) {
    PermissionManager manager([](auto event) {
        EXPECT_EQ(event.type, PermissionEventType::RESOLVED);
        EXPECT_EQ(event.decision, PermissionDecision::AI_ACCEPTED);
    });
    manager.configure(directory, ExecutionMode::SANE);
    manager.setImplementationPhase(ImplementationPhase::EXECUTION);
    manager.setPermissionMode(PermissionMode::AUTO, [](const auto&, const auto&) { return 0.95; });
    EXPECT_EQ(manager.ask(edit(), [] {}).decision, PermissionDecision::AI_ACCEPTED);
}

TEST_F(PermissionModesTest, ImplementationAskAndAutoRefusalSuspendExactActionForHumanDecision) {
    for (int scenario = 0; scenario < 4; ++scenario) {
        std::promise<PermissionEvent> prompt;
        auto ready = prompt.get_future();
        std::vector<PermissionEvent> events;
        PermissionManager manager([&](auto event) {
            events.push_back(event);
            if (event.type == PermissionEventType::REQUESTED) prompt.set_value(event);
        });
        manager.configure(directory, ExecutionMode::SANE);
        manager.setImplementationPhase(ImplementationPhase::EXECUTION);
        if (scenario) manager.setPermissionMode(PermissionMode::AUTO,
            [scenario](const auto&, const auto&) -> std::optional<double> {
                if (scenario == 2) throw std::runtime_error("Verifier unavailable");
                if (scenario == 3) return {};
                return 0.1;
            });
        auto answer = std::async(std::launch::async, [&] { return manager.ask(edit(), [] {}); });
        const auto status = ready.wait_for(2s);
        if (status != std::future_status::ready) {
            manager.close();
            FAIL() << "Expected blocking confirmation for scenario " << scenario;
        }
        const auto event = ready.get();
        EXPECT_EQ(event.request.toolName, "write");
        EXPECT_EQ(event.request.metadata, edit().metadata);
        EXPECT_EQ(answer.wait_for(30ms), std::future_status::timeout);
        manager.reply(event.id, PermissionDecision::YES);
        EXPECT_EQ(answer.get().decision, PermissionDecision::YES);
        EXPECT_EQ(events.back().type, PermissionEventType::RESOLVED);
        if (scenario) {
            ASSERT_EQ(events.size(), 3);
            EXPECT_EQ(events.front().decision, PermissionDecision::AI_REFUSED);
        }
    }
}

TEST_F(PermissionModesTest, ImplementationConfirmationCanRefuseOrCancelWithoutAllowingAction) {
    for (bool cancel : {false, true}) {
        std::promise<PermissionEvent> prompt;
        auto ready = prompt.get_future();
        PermissionManager manager([&](auto event) {
            if (event.type == PermissionEventType::REQUESTED) prompt.set_value(event);
        });
        manager.configure(directory, ExecutionMode::SANE);
        manager.setImplementationPhase(ImplementationPhase::EXECUTION);
        manager.setPermissionMode(PermissionMode::AUTO, [](const auto&, const auto&) { return 0.0; });
        auto answer = std::async(std::launch::async, [&] { return manager.ask(edit(), [] {}); });
        if (ready.wait_for(2s) != std::future_status::ready) {
            manager.close();
            FAIL() << "Expected confirmation";
        }
        if (cancel) {
            manager.close();
            EXPECT_THROW(answer.get(), core::Cancelled);
        } else {
            manager.reply(ready.get().id, PermissionDecision::NO);
            EXPECT_EQ(answer.get().decision, PermissionDecision::NO);
        }
    }
}

TEST_F(PermissionModesTest, ReviewNeverAllowsMutationsEvenInSlopDangerOrWithManualApproval) {
    PermissionManager manager([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    manager.configure(directory, ExecutionMode::SLOP);
    manager.setPermissionMode(PermissionMode::DANGER);
    manager.setImplementationPhase(ImplementationPhase::REVIEW);
    auto request = edit();
    request.manuallyApproved = true;
    EXPECT_THROW(manager.ask(request, [] {}), std::runtime_error);
    EXPECT_THROW(manager.ask({"bash", {}, {}, {}, "b", "bash", false}, [] {}), std::runtime_error);
    EXPECT_THROW(manager.ask({"mcp", {}, {}, {}, "m", "mcp.write", false}, [] {}), std::runtime_error);
    EXPECT_EQ(manager.ask({"read", {}, {}, {}, "r", "read", true}, [] {}).decision, PermissionDecision::ALLOW);
    EXPECT_EQ(manager.ask({"bash", {}, {}, {}, "b", "bash", true}, [] {}).decision, PermissionDecision::ALLOW);
}

TEST_F(PermissionModesTest, ReviewTransitionDismissesPendingImplementationApproval) {
    std::promise<PermissionEvent> prompt;
    auto ready = prompt.get_future();
    std::vector<PermissionEvent> events;
    PermissionManager manager([&](auto event) {
        events.push_back(event);
        if (event.type == PermissionEventType::REQUESTED) prompt.set_value(event);
    });
    manager.configure(directory, ExecutionMode::SANE);
    manager.setImplementationPhase(ImplementationPhase::EXECUTION);
    manager.setPermissionMode(PermissionMode::AUTO, [](const auto&, const auto&) { return 0.0; });
    auto answer = std::async(std::launch::async, [&] { return manager.ask(edit(), [] {}); });
    if (ready.wait_for(2s) != std::future_status::ready) {
        manager.close();
        FAIL() << "Expected confirmation";
    }
    const auto id = ready.get().id;
    manager.setImplementationPhase(ImplementationPhase::REVIEW);
    EXPECT_THROW(answer.get(), std::runtime_error);
    EXPECT_EQ(events.back().type, PermissionEventType::DISMISSED);
    EXPECT_THROW(manager.reply(id, PermissionDecision::YES), std::invalid_argument);
}

TEST_F(PermissionModesTest, ImplementationEscalationReevaluatesChangedPermissionPolicy) {
    std::promise<PermissionEvent> prompt;
    auto ready = prompt.get_future();
    PermissionManager manager([&](auto event) {
        if (event.type == PermissionEventType::REQUESTED) prompt.set_value(event);
    });
    manager.configure(directory, ExecutionMode::SANE);
    manager.setImplementationPhase(ImplementationPhase::EXECUTION);
    manager.setPermissionMode(PermissionMode::AUTO, [](const auto&, const auto&) { return 0.0; });
    auto answer = std::async(std::launch::async, [&] { return manager.ask(edit(), [] {}); });
    if (ready.wait_for(2s) != std::future_status::ready) {
        manager.close();
        FAIL() << "Expected confirmation";
    }
    const auto id = ready.get().id;
    manager.setPermissionMode(PermissionMode::DANGER);
    EXPECT_EQ(answer.get().decision, PermissionDecision::ALLOW);
    EXPECT_THROW(manager.reply(id, PermissionDecision::YES), std::invalid_argument);
}

TEST_F(PermissionModesTest, ImplementationReviewDoesNotUnlockAnotherConversationsPermissions) {
    PermissionManager execution([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    PermissionManager review([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    for (auto* manager : {&execution, &review}) {
        manager->configure(directory, ExecutionMode::SANE);
        manager->setPermissionMode(PermissionMode::DANGER);
    }
    execution.setImplementationPhase(ImplementationPhase::EXECUTION);
    review.setImplementationPhase(ImplementationPhase::REVIEW);
    EXPECT_EQ(execution.ask(edit(), [] {}).decision, PermissionDecision::ALLOW);
    EXPECT_THROW(review.ask(edit(), [] {}), std::runtime_error);
    EXPECT_EQ(review.ask({"read", {}, {}, {}, "r", "read", true}, [] {}).decision, PermissionDecision::ALLOW);
    EXPECT_EQ(execution.ask(edit(), [] {}).decision, PermissionDecision::ALLOW);
}

TEST(ImplementationPhasePromptTest, ExecutionAndReviewAnnounceDifferentAuthority) {
    const auto execution = prompts::implementationPhaseAnnouncement(ImplementationPhase::EXECUTION);
    const auto review = prompts::implementationPhaseAnnouncement(ImplementationPhase::REVIEW);
    EXPECT_NE(execution.find("You may modify files"), std::string::npos);
    EXPECT_NE(execution.find("suspends the exact action"), std::string::npos);
    EXPECT_NE(review.find("read-only review"), std::string::npos);
    EXPECT_NE(review.find("exact preserved diffs"), std::string::npos);
    EXPECT_NE(review.find("Do not modify files"), std::string::npos);
}

TEST(ImplementationPhasePromptTest, GeneratedUiContentUsesEnglishRegardlessOfUserLanguage) {
    const std::string discussion = prompts::DISCUSSION_SYSTEM_PROMPT;
    EXPECT_NE(discussion.find("Write generated UI content in English"), std::string::npos);
    EXPECT_NE(discussion.find("Conversational chat may use the user's language"), std::string::npos);
    EXPECT_NE(discussion.find("do not translate stored history"), std::string::npos);
    for (const auto phase : {ImplementationPhase::PLANNING, ImplementationPhase::EXECUTION, ImplementationPhase::REVIEW}) {
        const auto announcement = prompts::implementationPhaseAnnouncement(phase);
        EXPECT_NE(announcement.find("in English, even when the user writes in another language"), std::string::npos);
    }
}

TEST_F(PermissionModesTest, LoadingSkillNeverRunsItsScriptAndBashStillEnforcesSane) {
    const auto location = directory / ".sos/skills/check-api";
    fs::create_directories(location / "scripts");
    std::ofstream(location / "SKILL.md") << "---\nname: check-api\ndescription: Check routes\n---\nRead and run scripts/check.sh.";
    std::ofstream(location / "scripts/check.sh") << "printf checked > marker\n";
    PermissionManager manager([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    manager.configure(directory, ExecutionMode::SANE);
    manager.setPermissionMode(PermissionMode::DANGER);
    std::string toolName = "skill";
    ToolContext context{directory, directory, [&](auto request) {
        request.toolName = toolName;
        manager.ask(request, [] {});
    }};
    SkillTool skill(std::make_shared<SkillCatalog>(directory));
    EXPECT_NO_THROW(skill.execute({{"name", "check-api"}, {"input", ""}}, context));
    EXPECT_FALSE(fs::exists(directory / "marker"));
    toolName = "bash";
    const json command{{"command", "bash .sos/skills/check-api/scripts/check.sh"}, {"mainCommand", "bash"}, {"timeout", 1000}};
    EXPECT_THROW(BashTool{}.execute(command, context), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory / "marker"));
    manager.setMode(ExecutionMode::SLOP);
    EXPECT_NO_THROW(BashTool{}.execute(command, context));
    EXPECT_TRUE(fs::exists(directory / "marker"));
}

TEST_F(PermissionModesTest, ChangingPolicyReevaluatesAPendingAskWithoutCancellingTheRun) {
    for (const auto mode : {PermissionMode::AUTO, PermissionMode::DANGER}) {
        std::promise<void> prompted;
        auto ready = prompted.get_future();
        std::vector<PermissionEventType> events;
        PermissionManager manager([&](auto event) {
            events.push_back(event.type);
            if (event.type == PermissionEventType::REQUESTED) prompted.set_value();
        });
        manager.configure(directory, ExecutionMode::SLOP);
        auto pending = std::async(std::launch::async, [&] { return manager.ask(edit(), [] {}); });
        const auto status = ready.wait_for(2s);
        manager.setPermissionMode(mode, [](const auto&, const auto&) { return true; });
        const auto answer = pending.get();
        ASSERT_EQ(status, std::future_status::ready);
        EXPECT_EQ(answer.decision, mode == PermissionMode::AUTO ? PermissionDecision::AI_ACCEPTED : PermissionDecision::ALLOW);
        ASSERT_GE(events.size(), 2);
        EXPECT_EQ(events[1], PermissionEventType::DISMISSED);
    }
}

TEST_F(PermissionModesTest, AutoOnlyChecksActionsRequiringPermission) {
    int calls = 0;
    PermissionManager manager([](auto event) {
        EXPECT_EQ(event.type, PermissionEventType::RESOLVED);
        EXPECT_EQ(event.decision, PermissionDecision::AI_ACCEPTED);
    });
    manager.configure(directory, ExecutionMode::SLOP);
    manager.setPermissionMode(PermissionMode::AUTO, [&](const auto& request, const auto& check) {
        check();
        ++calls;
        EXPECT_TRUE(request.metadata.contains("arguments"));
        return true;
    });
    manager.ask({"read", {}, {}, {}, "r", "read", true}, [] {});
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(manager.ask(edit(), [] {}).decision, PermissionDecision::AI_ACCEPTED);
    EXPECT_EQ(calls, 1);
    manager.setMode(ExecutionMode::SANE);
    EXPECT_THROW(manager.ask(edit(), [] {}), std::runtime_error);
    EXPECT_EQ(calls, 1);
    manager.ask({"external_directory", {}, {}, {{"parentDir", "/"}, {"arguments", {{"filePath", "/example"}}}},
        "r", "read", true}, [] {});
    EXPECT_EQ(calls, 2);
}

TEST_F(PermissionModesTest, AutoFailureAndRejectionReturnToolErrorsWithoutHumanPrompts) {
    for (int scenario = 0; scenario < 3; ++scenario) {
        PermissionManager manager([](auto event) {
            EXPECT_EQ(event.type, PermissionEventType::RESOLVED);
            EXPECT_EQ(event.decision, PermissionDecision::AI_REFUSED);
        });
        manager.configure(directory, ExecutionMode::SLOP);
        PermissionManager::Verifier verifier;
        if (scenario != 2) verifier = [scenario](const auto&, const auto&) {
            if (scenario == 1) throw std::runtime_error("API unavailable");
            return false;
        };
        manager.setPermissionMode(PermissionMode::AUTO, verifier);
        int checks = 0;
        const auto answer = manager.ask(edit(), [&] {
            if (++checks > 2) throw std::runtime_error("Unexpected wait for human approval");
        });
        EXPECT_EQ(answer.decision, PermissionDecision::AI_REFUSED);
        EXPECT_FALSE(answer.id.empty());
        {
            const std::string message = answer.reason;
            EXPECT_NE(message.find(scenario == 0 ? "refused by the command-verification AI" : "check failed"), std::string::npos);
            EXPECT_NE(message.find("not executed"), std::string::npos);
            EXPECT_NE(message.find("Do not retry or bypass"), std::string::npos);
            EXPECT_NE(message.find("click Approve"), std::string::npos);
            EXPECT_NE(message.find("continue without this action"), std::string::npos);
        }
    }
}

TEST_F(PermissionModesTest, VerificationCancellationAndModeChangesCannotApprove) {
    PermissionManager manager([](auto) { ADD_FAILURE() << "Unexpected prompt"; });
    manager.configure(directory, ExecutionMode::SLOP);
    manager.setPermissionMode(PermissionMode::AUTO, [](const auto&, const auto&) -> bool { throw core::Cancelled(); });
    EXPECT_THROW(manager.ask(edit(), [] {}), core::Cancelled);
    manager.setPermissionMode(PermissionMode::AUTO, [&](const auto&, const auto&) {
        manager.setMode(ExecutionMode::SANE);
        return true;
    });
    EXPECT_THROW(manager.ask(edit(), [] {}), std::runtime_error);
}

TEST_F(PermissionModesTest, SettingsPersistWithoutReplacingProviderCredentials) {
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auth->set("openrouter", {.key = "chat-key"});
    ProviderConnections connections(auth, directory / "config.json");
    EXPECT_EQ(connections.permissionMode(), PermissionMode::ASK);
    EXPECT_THROW(connections.setPermissionMode(PermissionMode::AUTO), std::invalid_argument);
    auth->setPermissionKey("verifier-key");  // Historical OpenRouter credential.
    connections.setPermissionMode(PermissionMode::AUTO);
    EXPECT_EQ(auth->get("openrouter").key, "chat-key");
    EXPECT_EQ(auth->permissionKey(), "verifier-key");
    ProviderConnections reopened(std::make_shared<AuthStore>(directory / "auth.json"), directory / "config.json");
    EXPECT_EQ(reopened.permissionMode(), PermissionMode::AUTO);
    EXPECT_TRUE(reopened.hasPermissionKey());
    EXPECT_EQ(readPrivateJson(directory / "config.json").dump().find("verifier-key"), std::string::npos);
    struct stat status{};
    ASSERT_EQ(::stat((directory / "auth.json").c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    connections.setPermissionMode(PermissionMode::DANGER);
    EXPECT_EQ(reopened.permissionMode(), PermissionMode::DANGER);
}

TEST_F(PermissionModesTest, DecisionModelRoundTripBindsKeysAndPreservesChatAndThreshold) {
    const auto path = directory / "config.json";
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auth->setPermissionKey("legacy-key");
    auth->set("openai", {.key = "chat-key"});
    writePrivateJson(path, {{"permission_mode", "auto"}, {"permission_confidence", 93},
        {"provider", "openai"}, {"model", "chat-model"}});
    ProviderConnections connections(auth, path);
    EXPECT_EQ(connections.permissionModel().endpoint, "https://openrouter.ai/api/alpha/decisions");
    EXPECT_TRUE(connections.hasPermissionKey());
    PermissionVerifierConfig selected{DecisionProtocol::OPENAI_DECISIONS, "https://api.openai.com/v1/decisions", "decision-model"};
    const auto before = readPrivateJson(path);
    EXPECT_THROW(connections.savePermissionSettings(PermissionMode::AUTO, {}, selected), std::invalid_argument);
    EXPECT_EQ(readPrivateJson(path), before);
    connections.savePermissionSettings(PermissionMode::AUTO, {}, selected, "new-key");
    ProviderConnections reopened(auth, path);
    EXPECT_EQ(reopened.permissionConfidence(), 93);
    EXPECT_EQ(reopened.permissionModel().model, "decision-model");
    EXPECT_TRUE(reopened.hasPermissionKey());
    EXPECT_EQ(auth->get("openai").key, "chat-key");
    EXPECT_EQ(reopened.selection().model, "chat-model");
    const auto saved = readPrivateJson(path);
    EXPECT_EQ(saved.dump().find("new-key"), std::string::npos);
    const auto revision = saved.at("permission_credential").get<std::string>();
    EXPECT_EQ(auth->permissionKey("openai_decisions", selected.endpoint, revision), "new-key");
    EXPECT_TRUE(auth->permissionKey("typesafe", selected.endpoint, revision).empty());
    EXPECT_TRUE(auth->permissionKey("openai_decisions", "https://other.example/decisions", revision).empty());
    EXPECT_TRUE(auth->permissionKey("typesafe", "https://api.typesafe.ai/v1/systemone", "").empty());
    EXPECT_EQ(reopened.permissionModelInfo().dump().find("new-key"), std::string::npos);
    struct stat status{};
    ASSERT_EQ(::stat((directory / "auth.json").c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    selected.endpoint = "https://other.example/decisions";
    EXPECT_THROW(reopened.savePermissionSettings(PermissionMode::AUTO, 90, selected), std::invalid_argument);
    EXPECT_EQ(readPrivateJson(path), saved);
    auto invalid = selected;
    invalid.model = "";
    EXPECT_THROW(reopened.savePermissionSettings(PermissionMode::AUTO, 90, invalid, "secret"), std::invalid_argument);
    EXPECT_EQ(readPrivateJson(path), saved);
    reopened.savePermissionSettings(PermissionMode::AUTO, 0, selected);
    EXPECT_FALSE(reopened.hasPermissionKey());
}

TEST_F(PermissionModesTest, FailedSettingsWriteCannotActivateStagedCredential) {
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    const auto path = directory / std::string(252, 'c');
    // The valid filename fits NAME_MAX, but the atomic writer's temporary suffix does not.
    const auto revision = auth->stagePermissionKey("typesafe", "https://api.typesafe.ai/v1/systemone", "old-key");
    const json original{{"permission_mode", "auto"}, {"permission_confidence", 88},
        {"permission_model", {{"protocol", "typesafe"}, {"endpoint", "https://api.typesafe.ai/v1/systemone"}, {"model", "jev-latest"}}},
        {"permission_credential", revision}};
    std::ofstream(path) << original;
    ProviderConnections connections(auth, path);
    EXPECT_THROW(connections.savePermissionSettings(PermissionMode::AUTO, 99, {}, "replacement-key"), std::runtime_error);
    EXPECT_EQ(readPrivateJson(path), original);
    EXPECT_EQ(auth->permissionKey("typesafe", "https://api.typesafe.ai/v1/systemone", revision), "old-key");
    EXPECT_TRUE(connections.hasPermissionKey());
}

TEST_F(PermissionModesTest, ManualApprovalIsExactRepeatableRevocableAndNeverOverridesSane) {
    int verified = 0;
    PermissionManager manager([](auto) {});
    manager.configure(directory, ExecutionMode::SLOP);
    manager.setPermissionMode(PermissionMode::AUTO, [&](const auto&, const auto&) { ++verified; return false; });
    ToolPermissionRequest request{"bash", {}, {}, {{"arguments", {{"command", "echo test > file"}}},
        {"working_directory", directory.string()}}, "first-call", "bash"};
    const json action{{"tool", "bash"}, {"arguments", request.metadata["arguments"]},
        {"working_directory", directory.string()}, {"workspace", directory.string()}};
    manager.setManualActions({action});
    for (int attempt = 0; attempt < 3; ++attempt) {
        request.callId = "retry-" + std::to_string(attempt);
        EXPECT_EQ(manager.ask(request, [] {}).decision, PermissionDecision::MANUALLY_APPROVED);
    }
    EXPECT_EQ(verified, 0);
    auto changed = request;
    changed.metadata["arguments"]["command"] = "echo other > file";
    EXPECT_EQ(manager.ask(changed, [] {}).decision, PermissionDecision::AI_REFUSED);
    changed = request;
    changed.metadata["working_directory"] = "/";
    EXPECT_EQ(manager.ask(changed, [] {}).decision, PermissionDecision::AI_REFUSED);
    changed = request;
    changed.toolName = "write";
    EXPECT_EQ(manager.ask(changed, [] {}).decision, PermissionDecision::AI_REFUSED);
    manager.setMode(ExecutionMode::SANE);
    EXPECT_THROW(manager.ask(request, [] {}), std::runtime_error);
    manager.setMode(ExecutionMode::SLOP);
    manager.setManualActions({});
    EXPECT_EQ(manager.ask(request, [] {}).decision, PermissionDecision::AI_REFUSED);
    auto foreign = action;
    foreign["workspace"] = "/";
    manager.setManualActions({foreign});
    EXPECT_EQ(manager.ask(request, [] {}).decision, PermissionDecision::AI_REFUSED);
    EXPECT_EQ(verified, 5);
}

TEST(PermissionModesProtocolTest, ManualApprovalRequiresSessionTargetAndBoolean) {
    const auto request = json{{"type", "permission.manual"}, {"sessionId", "s"},
        {"refusedMessageId", "r"}, {"approved", true}}.get<IntegrationRequest>();
    EXPECT_EQ(request.sessionId, "s");
    EXPECT_EQ(request.refusedMessageId, "r");
    EXPECT_TRUE(request.manuallyApproved);
    EXPECT_THROW((json{{"type", "permission.manual"}, {"sessionId", "s"},
        {"refusedMessageId", "r"}, {"approved", "true"}}.get<IntegrationRequest>()), json::exception);
    EXPECT_THROW(parsePermissionDecision("manually_approved"), std::invalid_argument);
}

TEST(PermissionVerifierTest, DefaultThresholdRequiresEightyPercentAllowProbability) {
    for (const auto protocol : {DecisionProtocol::TYPESAFE, DecisionProtocol::OPENAI_DECISIONS}) {
        const auto responseFor = [protocol](double allow) {
            json answer{{"type", "choice"}, {"choice", allow >= 0.5 ? "allow" : "deny"},
                {"confidence", 0.01}};
            if (protocol == DecisionProtocol::TYPESAFE) {
                answer["probabilities"] = {{"allow", allow}, {"deny", 1 - allow}};
                return json{{"answers", {{"permission", answer}}}};
            }
            answer["name"] = "permission";
            answer["probabilities"] = json::array({{{"value", "deny"}, {"probability", 1 - allow}},
                {{"value", "allow"}, {"probability", allow}}});
            return json{{"answers", json::array({answer})}};
        };
        for (const double value : {0.8, 0.94, 0.99, 1.0})
            EXPECT_TRUE(PermissionVerifier::allows(responseFor(value), 80, protocol));
        for (const double value : {0.7999999999999, 0.0, 0.5})
            EXPECT_FALSE(PermissionVerifier::allows(responseFor(value), 80, protocol));
        EXPECT_DOUBLE_EQ(*PermissionVerifier::allowProbability(responseFor(0.02), protocol), 0.02);
        EXPECT_TRUE(PermissionVerifier::allows(responseFor(0.02), 2, protocol));
        EXPECT_FALSE(PermissionVerifier::allows(responseFor(0.02), 3, protocol));
        EXPECT_TRUE(PermissionVerifier::allows(responseFor(1), 100, protocol));
        EXPECT_TRUE(PermissionVerifier::allows(json::object(), 0, protocol));
        EXPECT_THROW(PermissionVerifier::allows(responseFor(1), 101, protocol), std::invalid_argument);
    }
}

TEST(PermissionVerifierTest, InvalidDistributionsAndRefusalsNeverAuthorize) {
    const json valid{{"type", "choice"}, {"choice", "allow"},
        {"probabilities", {{"allow", 0.95}, {"deny", 0.05}}}};
    std::vector<json> invalid;
    for (const auto& value : {json(-1), json(1.01), json("0.95"), json(nullptr),
         json(std::numeric_limits<double>::infinity()), json(std::numeric_limits<double>::quiet_NaN())}) {
        auto answer = valid;
        answer["probabilities"]["allow"] = value;
        invalid.push_back(answer);
    }
    for (const auto& patch : std::vector<json>{
        {{"choice", "deny"}}, {{"choice", 42}}, {{"type", "refusal"}}, {{"type", nullptr}},
        {{"probabilities", {{"allow", 0.9}, {"deny", 0.5}}}},
        {{"probabilities", {{"allow", 0.95}}}},
        {{"probabilities", {{"allow", 0.95}, {"other", 0.05}}}},
        {{"probabilities", json::array()}}, {{"probabilities", nullptr}}}) {
        auto answer = valid;
        answer.update(patch);
        invalid.push_back(answer);
    }
    invalid.push_back(json{{"type", "choice"}, {"choice", "allow"}, {"confidence", 1.0}});
    invalid.push_back(nullptr);
    for (const auto& answer : invalid) {
        const json response{{"answers", {{"permission", answer}}}};
        EXPECT_FALSE(PermissionVerifier::allowProbability(response)) << response;
    }
    EXPECT_FALSE(PermissionVerifier::allowProbability({{"answers", {{"permission", valid}, {"extra", valid}}}}));
    EXPECT_FALSE(PermissionVerifier::allowProbability({{"error", "refused"}, {"answers", {{"permission", valid}}}}));
    EXPECT_FALSE(PermissionVerifier::allowProbability(json::array()));
    for (const auto& answers : std::vector<json>{json::array(),
        json::array({valid, valid}), json::array({{{"type", "refusal"}, {"name", "permission"}}}),
        json::array({{{"type", "choice"}, {"name", "wrong"}}}),
        json::array({{{"type", "choice"}, {"name", "permission"}, {"choice", "allow"},
            {"probabilities", json::array({{{"value", "allow"}, {"probability", 0.5}},
                {{"value", "allow"}, {"probability", 0.5}}})}}})})
        EXPECT_FALSE(PermissionVerifier::allowProbability({{"answers", answers}}, DecisionProtocol::OPENAI_DECISIONS));
}

TEST_F(PermissionModesTest, ThresholdUsesRunConfidenceAndZeroDisablesVerifierWithoutOverridingSane) {
    std::vector<PermissionEvent> events;
    PermissionManager manager([&](auto event) { events.push_back(event); });
    manager.configure(directory, ExecutionMode::SLOP);
    int calls = 0;
    const PermissionManager::Verifier verifier = [&](const auto&, const auto&) { ++calls; return 0.02; };
    manager.setPermissionMode(PermissionMode::AUTO, verifier, 3);
    auto answer = manager.ask(edit(), [] {});
    EXPECT_EQ(answer.decision, PermissionDecision::AI_REFUSED);
    EXPECT_EQ(answer.confidence, 0.02);
    EXPECT_EQ(json(events.back()).at("confidence"), 0.02);
    manager.setPermissionMode(PermissionMode::AUTO, verifier, 2);
    EXPECT_EQ(manager.ask(edit(), [] {}).decision, PermissionDecision::AI_ACCEPTED);
    EXPECT_EQ(calls, 2);
    manager.setPermissionMode(PermissionMode::AUTO, verifier, 0);
    EXPECT_EQ(manager.ask(edit(), [] {}).decision, PermissionDecision::ALLOW);
    EXPECT_EQ(calls, 2);
    manager.setMode(ExecutionMode::SANE);
    EXPECT_THROW(manager.ask(edit(), [] {}), std::runtime_error);
    EXPECT_THROW(manager.ask(edit(), [] { throw core::Cancelled(); }), core::Cancelled);
    EXPECT_EQ(calls, 2);
    manager.setMode(ExecutionMode::SLOP);
    manager.setPermissionMode(PermissionMode::AUTO, {}, 1);
    answer = manager.ask(edit(), [] {});
    EXPECT_EQ(answer.decision, PermissionDecision::AI_REFUSED);
    EXPECT_FALSE(answer.confidence);
    EXPECT_FALSE(json(events.back()).contains("confidence"));
}

TEST_F(PermissionModesTest, ConfidencePreferencePersistsAndAcceptsOnlyWholePercentages) {
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auth->set("openrouter", {.key = "chat-key"});
    ProviderConnections connections(auth, directory / "config.json");
    EXPECT_EQ(connections.permissionConfidence(), 80);
    connections.setPermissionMode(PermissionMode::AUTO, 0);
    EXPECT_EQ(connections.permissionMode(), PermissionMode::AUTO);
    EXPECT_EQ(connections.permissionConfidence(), 0);
    EXPECT_FALSE(connections.hasPermissionKey());
    EXPECT_THROW(connections.setPermissionMode(PermissionMode::AUTO, 1), std::invalid_argument);
    EXPECT_EQ(connections.permissionConfidence(), 0);
    connections.savePermissionKey("verifier");
    connections.setPermissionMode(PermissionMode::AUTO, 95);
    ProviderConnections restored(auth, directory / "config.json");
    EXPECT_EQ(restored.permissionConfidence(), 95);
    restored.select({"openrouter", "model", "high"});
    EXPECT_EQ(restored.permissionConfidence(), 95);
    restored.setPermissionMode(PermissionMode::ASK);
    EXPECT_EQ(restored.permissionConfidence(), 95);
    for (const auto invalid : {json(-1), json(101), json(20.5), json("80"), json(true), json(nullptr), json(1e20)})
        EXPECT_THROW((json{{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", invalid}}.get<IntegrationRequest>()), std::invalid_argument);
    for (const int value : {0, 1, 80, 100})
        EXPECT_EQ((json{{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", value}}.get<IntegrationRequest>()).permissionConfidence, value);
}

TEST(PermissionVerifierTest, OversizedOrSecretActionsNeverReachTransport) {
    PermissionVerifier verifier("secret", "http://127.0.0.1:1");
    ToolPermissionRequest request{"bash", {}, {}, {{"arguments", {{"command", std::string(48001, 'x')}}}}, "c", "bash"};
    EXPECT_FALSE(verifier.verify(request, "/workspace", "test", {}));
    request.metadata["arguments"]["command"] = "secret";
    EXPECT_FALSE(verifier.verify(request, "/workspace", "test", {}));
    EXPECT_THROW(verifier.verify(request, "/workspace", "test", [] { return true; }), core::Cancelled);
}

TEST_F(PermissionModesTest, FileVerificationOmitsContentsAndDiffs) {
    const auto path = (directory / "file.cpp").string();
    const std::string code(60000, 'x');
    for (const auto* tool : {"write", "edit"}) {
        for (const bool exists : {false, true}) {
            ToolPermissionRequest request{"edit", {"file.cpp"}, {},
                {{"filepath", path}, {"exists", exists}, {"diff", code},
                 {"arguments", {{"content", code}, {"oldString", code}, {"newString", code}}}}, "c", tool};
            const auto state = PermissionVerifier::stateFor(request, directory.string(), "test");
            EXPECT_EQ(state["details"], (json{{"filepath", path}, {"exists", exists},
                {"operation", exists ? "modify" : "create"}}));
            EXPECT_LT(state.dump().size(), 1000);
        }
    }
}

TEST_F(PermissionModesTest, PatchVerificationRetainsEveryOperationAndResolvedTarget) {
    const auto path = (directory / "file.cpp").string();
    const auto target = (directory / "moved.cpp").string();
    std::ofstream(path) << "existing file";
    fs::create_symlink(directory / "file.cpp", directory / "link.cpp");
    auto files = json::array();
    for (const auto* operation : {"add", "update", "delete", "move"})
        files.push_back({{"filePath", (directory / "link.cpp").string()}, {"type", operation},
            {"movePath", target}, {"patch", "PRIVATE_CODE"}, {"future_content_field", "PRIVATE_CODE"}});
    ToolPermissionRequest request{"edit", {}, {}, {{"files", files}, {"diff", "PRIVATE_CODE"},
        {"arguments", {{"patchText", "PRIVATE_CODE"}}}}, "c", "apply_patch"};
    const auto state = PermissionVerifier::stateFor(request, directory.string(), "test");
    ASSERT_EQ(state["details"]["files"].size(), 4);
    for (std::size_t i = 0; i < files.size(); ++i)
        EXPECT_EQ(state["details"]["files"][i], (json{{"filePath", path},
            {"type", files[i]["type"]}, {"movePath", target}}));
    EXPECT_EQ(state.dump().find("PRIVATE_CODE"), std::string::npos);
}

TEST_F(PermissionModesTest, ExternalFileVerificationAlsoOmitsArguments) {
    for (const auto* tool : {"write", "edit", "apply_patch"}) {
        ToolPermissionRequest request{"external_directory", {}, {},
            {{"filepath", (directory / "file").string()}, {"parentDir", directory.string()},
             {"arguments", {{"content", "PRIVATE_CODE"}, {"patchText", "PRIVATE_CODE"}}}}, "c", tool};
        const auto details = PermissionVerifier::stateFor(request, directory.string(), "test")["details"];
        EXPECT_EQ(details, (json{{"filepath", (directory / "file").string()},
            {"parentDir", directory.string()}, {"operation", "access_for_file_change"}}));
    }
}

TEST(PermissionVerifierTest, BashRetainsExactArgumentsAndContext) {
    ToolPermissionRequest request{"bash", {"bash"}, {},
        {{"arguments", {{"command", "echo hello > file"}, {"timeout", 1000}}},
         {"working_directory", "/workspace"}}, "c", "bash"};
    const auto state = PermissionVerifier::stateFor(request, "/workspace", "user request");
    EXPECT_EQ(state["details"], request.metadata);
    EXPECT_EQ(state["recent_user_request"], "user request");
    EXPECT_EQ(state["workspace"], "/workspace");
}

TEST(PermissionModesProtocolTest, ParsesOnlyKnownModesAndSeparatesCredentials) {
    const auto request = json{{"type", "permissions.select"}, {"mode", "auto"}, {"key", "test-key"}}.get<IntegrationRequest>();
    EXPECT_EQ(request.permissionMode, PermissionMode::AUTO);
    EXPECT_EQ(request.permissionKey, "test-key");
    EXPECT_TRUE(request.text.empty());
    EXPECT_THROW((json{{"type", "permissions.select"}, {"mode", "anything"}}.get<IntegrationRequest>()), std::invalid_argument);
    EXPECT_NO_THROW((json{{"type", "permissions.get"}}.get<IntegrationRequest>()));
}
}
