#ifndef SOS_PERMISSION_VERIFIER_HPP
#define SOS_PERMISSION_VERIFIER_HPP

#include <agent-runtime/tools/permission.hpp>
#include <agent-runtime/core/cancellation.hpp>

enum class DecisionProtocol { TYPESAFE, OPENAI_DECISIONS };

struct PermissionVerifierConfig {
    DecisionProtocol protocol = DecisionProtocol::TYPESAFE;
    std::string endpoint = "https://openrouter.ai/api/alpha/decisions";
    std::string model = "typesafe/jev-1.13";
    void validate() const;
};
const char* decisionProtocolName(DecisionProtocol protocol);
void to_json(nlohmann::json& json, const PermissionVerifierConfig& config);
void from_json(const nlohmann::json& json, PermissionVerifierConfig& config);

class PermissionVerifier {
public:
    explicit PermissionVerifier(std::string key,
        std::string endpoint = "https://openrouter.ai/api/alpha/decisions");
    PermissionVerifier(std::string key, PermissionVerifierConfig config);
    std::optional<double> verify(const ToolPermissionRequest& request, const std::string& workspace,
        const std::string& userContext, core::CancellationCheck cancelled) const;
    static std::optional<double> allowProbability(const nlohmann::json& response,
        DecisionProtocol protocol = DecisionProtocol::TYPESAFE);
    static bool allows(const nlohmann::json& response, int threshold = 80,
        DecisionProtocol protocol = DecisionProtocol::TYPESAFE);
    static nlohmann::json stateFor(const ToolPermissionRequest& request, const std::string& workspace,
        const std::string& userContext);
private:
    std::string key;
    PermissionVerifierConfig config;
};

#endif // SOS_PERMISSION_VERIFIER_HPP
