#include "view_image.hpp"
#include <stdexcept>
ToolDefinition ViewImageTool::getDefinition() const {
    return {"view_image", "View a previously attached image by its image_id in this session, including after compaction. Pixels re-enter the active context.",
        {{"type", "object"}, {"properties", {{"image_id", {{"type", "string"}}}}},
         {"required", {"image_id"}}, {"additionalProperties", false}}};
}
ToolResult ViewImageTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    if (context.checkCancelled) context.checkCancelled();
    if (!context.readImage) throw std::runtime_error("No session image store is available");
    auto image = context.readImage(arguments.at("image_id").get<std::string>());
    ToolResult result;
    result.title = image.name;
    result.output = "Image " + image.id + " (" + image.mime + ", " + std::to_string(image.width) + "x" + std::to_string(image.height) + ")";
    result.images.push_back(std::move(image));
    return result;
}
