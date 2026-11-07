#ifndef SOS_PERMISSION_VERIFIER_HPP
#define SOS_PERMISSION_VERIFIER_HPP

#include <agent-runtime/tools/permission.hpp>
#include <agent-runtime/core/cancellation.hpp>

class PermissionVerifier {
public:
    explicit PermissionVerifier(std::string key,
        std::string endpoint = "https://openrouter.ai/api/alpha/decisions");
    std::optional<double> verify(const ToolPermissionRequest& request, const std::string& workspace,
        const std::string& userContext, core::CancellationCheck cancelled) const;
    static std::optional<double> runConfidence(const nlohmann::json& response);
    static bool allows(const nlohmann::json& response, int threshold = 80);
    static nlohmann::json stateFor(const ToolPermissionRequest& request, const std::string& workspace,
        const std::string& userContext);
private:
    std::string key;
    std::string endpoint;
};

#endif // SOS_PERMISSION_VERIFIER_HPP
