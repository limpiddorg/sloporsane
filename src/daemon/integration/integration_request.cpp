#include "integration.hpp"

void from_json(const nlohmann::json& json, IntegrationRequest& request) {
    json.at("type").get_to(request.type);
    request.requestId = json.value("requestId", "");
    if (request.type == "worktree.list") {
    } else if (request.type == "worktree.create") {
        json.at("name").get_to(request.worktreeName);
    } else if (request.type == "worktree.switch" || request.type == "worktree.delete") {
        json.at("path").get_to(request.worktreePath);
    } else if (request.type == "mcp.cancel") {
        // Cancellation is applied immediately by the socket reader.
    } else if (request.type.starts_with("mcp.")) {
        request.mcpRequest = json.get<mcp::ManagementRequest>();
    } else if (request.type.starts_with("auth.") || request.type.starts_with("provider.") || request.type.starts_with("model.")) {
        request.providerRequest = json.get<ProviderRequest>();
    } else if (request.type == "btw.send" || request.type == "btw.list" || request.type == "btw.history") {
        json.at("sessionId").get_to(request.sessionId);
        request.branchId = json.value("branchId", "");
        request.objectiveId = json.value("objectiveId", "");
        if (request.type == "btw.send") json.at("text").get_to(request.text);
    } else if (request.type == "message.send") {
        json.at("text").get_to(request.text);
        request.saneRequest = json.value("sane_request", false);
        request.implementationRequest = json.value("implementation_request", false);
        if (request.implementationRequest && (request.saneRequest || request.text != "Implementation plan request" ||
            json.contains("images") || json.contains("skill") || json.contains("skills")))
            throw std::invalid_argument("Invalid implementation plan request");
        if (request.saneRequest && (request.text != "Sane Implementation request" || json.contains("images") || json.contains("skill") || json.contains("skills")))
            throw std::invalid_argument("Invalid Sane Implementation request");
        if (json.contains("skill") && json.contains("skills"))
            throw std::invalid_argument("Use either skill or skills, not both");
        if (json.contains("skill")) request.skills = {json.at("skill").get<SkillInvocation>()};
        if (json.contains("skills")) {
            request.skills = json.at("skills").get<std::vector<SkillInvocation>>();
            if (request.skills.empty() || request.skills.size() > MAX_STACKED_SKILLS)
                throw std::invalid_argument("Expected between 1 and 6 stacked skills");
        }
        request.images = json.value("images", std::vector<Image>{});
        validateImages(request.images);
        request.workspace = json.value("workspace", "");
        request.model = json.value("model", "");
        if (json.contains("reasoning")) request.reasoning = json.at("reasoning").get<std::string>();
        if (json.contains("mode")) request.messageMode = parseExecutionMode(json.at("mode").get<std::string>());
        if (request.saneRequest || request.implementationRequest) {
            if (request.messageMode && *request.messageMode != ExecutionMode::SANE)
                throw std::invalid_argument("Planning requests require Sane mode");
            request.messageMode = ExecutionMode::SANE;
        }
    } else if (request.type == "message.check") {
        request.hasImages = json.value("has_images", false);
    } else if (request.type == "question.reply" || request.type == "question.reject") {
        json.at("questionId").get_to(request.questionId);
        if (request.type == "question.reply") {
            const auto& choice = json.at("choice");
            if (!choice.is_number_unsigned() && (!choice.is_number_integer() || choice.get<std::int64_t>() < 0))
                throw std::invalid_argument("Question choice must be a non-negative integer");
            request.questionChoice = choice.get<std::size_t>();
        }
    } else if (request.type == "implementation.open" || request.type == "implementation.view" || request.type == "implementation.close") {
        json.at("sessionId").get_to(request.sessionId);
        request.branchId = json.value("branchId", "");
        request.objectiveId = json.value("objectiveId", "");
    } else if (request.type == "implementation.start" || request.type == "implementation.auto_plan" || request.type == "implementation.resume" || request.type == "implementation.state" ||
        request.type == "implementation.validate" || request.type == "implementation.unvalidate" || request.type == "implementation.review" ||
        request.type == "implementation.review.cancel" || request.type == "implementation.rework" ||
        request.type == "implementation.rework.reject" || request.type == "implementation.history" || request.type == "implementation.directory" ||
        request.type == "implementation.revision.open" || request.type == "implementation.revision.review") {
        json.at("sessionId").get_to(request.sessionId);
        if (request.type == "implementation.directory") {
            if (!json.at("step").is_number_integer() || json.at("step") < 0 || json.at("step") > 30)
                throw std::invalid_argument("Invalid repository step");
            request.stepIndex = json.at("step").get<std::size_t>();
            request.repositoryTree = json.at("tree").get<std::string>();
            request.repositoryPath = json.value("path", std::string{});
            if (request.repositoryPath.size() > 4096) throw std::invalid_argument("Repository path is too long");
        }
        if (request.type == "implementation.start" || request.type == "implementation.auto_plan")
            request.implementationPace = json.at("pace").get<std::string>();
        if (json.contains("revision")) {
            if (!json.at("revision").is_number_integer() || json.at("revision") < 0)
                throw std::invalid_argument("Invalid revision");
            request.revisionProvided = true;
            request.revision = json.at("revision").get<std::uint64_t>();
        }
        if (request.type == "implementation.rework" || request.type == "implementation.rework.reject" || request.type == "implementation.revision.open") {
            if (!json.at("revision").is_number_integer() || json.at("revision") < 0)
                throw std::invalid_argument("Invalid revision");
            request.revision = json.at("revision").get<std::uint64_t>();
        }
        if (request.type == "implementation.revision.review") {
            request.text = json.at("text").get<std::string>();
            if (request.text.empty() || request.text.size() > 100000) throw std::invalid_argument("Invalid revision discussion text");
        }
        if (request.type == "implementation.history") {
            request.historyAll = json.value("all", false);
            if (!request.historyAll) {
                if (!json.at("step").is_number_integer() || json.at("step") < 0 || json.at("step") > 30) throw std::invalid_argument("Invalid history selection");
                request.stepIndex = json.at("step").get<std::size_t>();
            }
        }
        if (request.type == "implementation.validate" || request.type == "implementation.unvalidate" || request.type == "implementation.review") {
            for (const auto key : {"step", "substep"})
                if (!json.at(key).is_number_integer() || json.at(key) < 0 || json.at(key) > 30)
                    throw std::invalid_argument("Invalid implementation selection");
            request.stepIndex = json.at("step"); request.substepIndex = json.at("substep");
        }
        if (request.type == "implementation.review") {
            json.at("text").get_to(request.text);
            request.reviewPath = json.value("path", "");
            if (json.contains("hunk")) {
                if (!json.at("hunk").is_number_integer() || json.at("hunk") < 0)
                    throw std::invalid_argument("Invalid review hunk");
                request.reviewHunk = json.at("hunk").get<std::size_t>();
            }
            request.branchId = json.value("branchId", "");
            if (json.contains("skills")) {
                request.skills = json.at("skills").get<std::vector<SkillInvocation>>();
                if (request.skills.empty() || request.skills.size() > MAX_STACKED_SKILLS)
                    throw std::invalid_argument("Expected between 1 and 6 stacked skills");
            }
        }
    } else if (request.type == "session.attach" || request.type == "session.preview") {
        json.at("sessionId").get_to(request.sessionId);
    } else if (request.type == "session.history" || request.type == "session.activities" || request.type == "session.activity_detail") {
        json.at("sessionId").get_to(request.sessionId);
        request.direction = json.value("direction", "latest");
        request.anchor = json.value("anchor", "");
        request.clean = json.value("clean", false);
        if (request.type != "session.history") json.at("groupId").get_to(request.groupId);
        if (request.type == "session.activity_detail") {
            const auto& index = json.at("activityIndex");
            if (!index.is_number_integer() || (!index.is_number_unsigned() && index.get<std::int64_t>() < 0))
                throw std::invalid_argument("Activity index must be a non-negative integer");
            request.activityIndex = index.get<std::size_t>();
        }
    } else if (request.type == "integration.pong") {
        json.at("pingId").get_to(request.pingId);
    } else if (request.type == "integration.configure" || request.type == "mode.set") {
        request.mode = parseExecutionMode(json.at("mode").get<std::string>());
        if (request.type == "integration.configure") {
            json.at("workspace").get_to(request.workspace);
            request.terminalSessionId = json.value("terminalSessionId", "");
            if (request.terminalSessionId.size() > 256)
                throw std::invalid_argument("Terminal session identity is too long");
        }
    } else if (request.type == "permissions.select") {
        request.permissionMode = parsePermissionMode(json.at("mode").get<std::string>());
        request.permissionKey = json.value("key", "");
        if (json.contains("model_config")) {
            if (json.value("check_version", nlohmann::json{}) != 1)
                throw std::invalid_argument("AI Check configuration requires check_version 1");
            request.permissionModel = json.at("model_config").get<PermissionVerifierConfig>();
        } else if (json.contains("check_version") && json.at("check_version") != 1) {
            throw std::invalid_argument("Unsupported AI Check configuration version");
        }
        if (json.contains("confidence")) {
            const auto& value = json.at("confidence");
            if (!value.is_number_integer() || value < 0 || value > 100)
                throw std::invalid_argument("Confidence must be an integer between 0 and 100");
            request.permissionConfidence = value.get<int>();
        }
    } else if (request.type == "permissions.get") {
        // No credentials are returned by this request.
    } else if (request.type == "permission.manual") {
        json.at("sessionId").get_to(request.sessionId);
        json.at("refusedMessageId").get_to(request.refusedMessageId);
        json.at("approved").get_to(request.manuallyApproved);
    } else if (request.type == "permission.reply") {
        json.at("permissionId").get_to(request.permissionId);
        request.decision = parsePermissionDecision(json.at("decision").get<std::string>());
    } else if (request.type != "session.list" && request.type != "session.new" && request.type != "session.compact" && request.type != "session.recap" &&
               request.type != "session.undo" && request.type != "session.redo" &&
               request.type != "execution.cancel" && request.type != "reasoning.next" && request.type != "integration.close") {
        throw std::invalid_argument("Unknown request type: " + request.type);
    }
}
