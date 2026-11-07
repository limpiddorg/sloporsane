#ifndef SOS_FS_COMMON_HPP
#define SOS_FS_COMMON_HPP

#include "../tool.hpp"
#include <mutex>

namespace fs_tools {
void checkpoint(const ToolContext& context);
std::filesystem::path resolve(const ToolContext& context, const std::string& path);
void checkExternal(const ToolContext& context, const std::filesystem::path& path, bool directory = false);
std::string relative(const ToolContext& context, const std::filesystem::path& path);
std::string readText(const std::filesystem::path& path);
void writeText(const std::filesystem::path& path, const std::string& content);
std::string diff(const std::filesystem::path& path, const std::string& before, const std::string& after);
std::vector<std::string> lines(const std::string& text);
std::string join(const std::vector<std::string>& lines);
std::string trim(const std::string& text);
std::string normalizeLines(const std::string& text);
std::string withOriginalFormat(const std::string& text, const std::string& original);
std::string withoutBom(const std::string& text);
std::mutex& mutationMutex();
inline void mutate(const ToolContext& context, const std::vector<std::filesystem::path>& paths, const std::function<void()>& action) {
    if (context.mutateFiles) context.mutateFiles(paths, action);
    else action();
}
}

#endif // SOS_FS_COMMON_HPP
