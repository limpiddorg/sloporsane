#include "registry.hpp"

#include <stdexcept>

void ToolRegistry::add(std::unique_ptr<Tool> tool) {
    if (!tool) throw std::invalid_argument("Cannot register a null tool");
    const auto definition = tool->getDefinition();
    if (definition.id.empty()) throw std::invalid_argument("Tool id must not be empty");
    if (dynamicTools.contains(definition.id)) throw std::invalid_argument("Tool already registered: " + definition.id);
    if (!tools.try_emplace(definition.id, std::move(tool)).second)
        throw std::invalid_argument("Tool already registered: " + definition.id);
}

void ToolRegistry::remove(const std::string& id) { tools.erase(id); }

const Tool& ToolRegistry::get(const std::string& id) const {
    const auto found = tools.find(id);
    if (found != tools.end()) return *found->second;
    const auto dynamic = dynamicTools.find(id);
    if (dynamic != dynamicTools.end()) return *dynamic->second;
    throw std::out_of_range("Unknown tool: " + id);
}

void ToolRegistry::replaceDynamic(std::vector<std::unique_ptr<Tool>> replacement) {
    ToolRegistry validated;
    for (auto& tool : replacement) {
        if (!tool) throw std::invalid_argument("Cannot register a null tool");
        const auto id = tool->getDefinition().id;
        if (tools.contains(id)) throw std::invalid_argument("Dynamic tool shadows a built-in: " + id);
        validated.add(std::move(tool));
    }
    dynamicTools.swap(validated.tools);
}

std::vector<ToolDefinition> ToolRegistry::getDefinitions() const {
    std::vector<ToolDefinition> definitions;
    for (const auto& [id, tool] : tools) definitions.push_back(tool->getDefinition());
    for (const auto& [id, tool] : dynamicTools) definitions.push_back(tool->getDefinition());
    return definitions;
}
