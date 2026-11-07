#ifndef SOS_TOOL_REGISTRY_HPP
#define SOS_TOOL_REGISTRY_HPP

#include "tool.hpp"

#include <map>
#include <memory>

// Register tools before executing them. Owns tools, not per-call context.
class ToolRegistry {
public:
    void add(std::unique_ptr<Tool> tool);
    void remove(const std::string& id);
    // Replace only the dynamic catalogue, between executions. Validation is
    // transactional; built-ins cannot be shadowed. Not thread-safe.
    void replaceDynamic(std::vector<std::unique_ptr<Tool>> replacement);
    const Tool& get(const std::string& id) const;
    std::vector<ToolDefinition> getDefinitions() const;

private:
    std::map<std::string, std::unique_ptr<Tool>> tools;
    std::map<std::string, std::unique_ptr<Tool>> dynamicTools;
};

#endif // SOS_TOOL_REGISTRY_HPP
