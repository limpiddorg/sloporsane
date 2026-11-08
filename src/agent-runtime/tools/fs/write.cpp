#include "write.hpp"
#include "common.hpp"

#include <stdexcept>

void from_json(const nlohmann::json& json, WriteArguments& value) {
    json.at("filePath").get_to(value.filePath);
    json.at("content").get_to(value.content);
}

ToolDefinition WriteTool::getDefinition() const {
    return fs_tools::withExplanation({"write", "Write a file, creating parent directories as needed. Replaces the entire content of an existing file. Read existing files before overwriting them; prefer edit for targeted changes.",
        {{"type", "object"}, {"properties", {{"filePath", {{"type", "string"}}}, {"content", {{"type", "string"}}}}},
            {"required", {"filePath", "content"}}}}, explained);
}

ToolResult WriteTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto explanation = fs_tools::explanation(arguments, explained);
    const auto params = arguments.get<WriteArguments>();
    const auto path = fs_tools::resolve(context, params.filePath);
    fs_tools::checkExternal(context, path);
    std::unique_lock lock(fs_tools::mutationMutex());
    const bool exists = std::filesystem::exists(path);
    const auto before = exists ? fs_tools::readText(path) : "";
    const auto after = before.starts_with("\xef\xbb\xbf") && !params.content.starts_with("\xef\xbb\xbf") ?
        "\xef\xbb\xbf" + params.content : params.content;
    const auto diff = fs_tools::diff(path, before, after);
    lock.unlock();
    context.ask({"edit", {fs_tools::relative(context, path)}, {"*"}, {{"filepath", path.string()}, {"exists", exists}, {"diff", diff}}});
    lock.lock();
    fs_tools::checkpoint(context);
    if (std::filesystem::exists(path) != exists || (exists && fs_tools::readText(path) != before))
        throw std::runtime_error("File changed while awaiting permission: " + path.string());
    fs_tools::mutate(context, {path}, [&] { fs_tools::writeText(path, after); }, explanation);
    return {fs_tools::relative(context, path), "Wrote file successfully.",
        {{"filepath", path.string()}, {"exists", exists}, {"diff", diff}}, {}};
}
