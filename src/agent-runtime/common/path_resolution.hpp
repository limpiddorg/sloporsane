#ifndef SOS_PATH_RESOLUTION_HPP
#define SOS_PATH_RESOLUTION_HPP

#include <filesystem>

struct PathResolution {
    static std::filesystem::path home();
    static std::filesystem::path dataDirectory();
    static std::filesystem::path stateDirectory();
    static std::filesystem::path configDirectory();
    static std::filesystem::path cacheDirectory();
    static std::filesystem::path binDirectory();
    static std::filesystem::path authFile();
    static std::filesystem::path configFile();
    static std::filesystem::path skillsDirectory();
    static std::filesystem::path sessionsDirectory();
    static std::filesystem::path daemonLog();
    static std::filesystem::path installedAppsDirectory();
    static std::filesystem::path managedToolsDirectory();
    static std::filesystem::path snapshotsDirectory();
    static std::filesystem::path worktreesDirectory();
    static std::filesystem::path workspacesDirectory();
    static std::filesystem::path mcpConfigFile();
    static std::filesystem::path mcpAuthFile();
    static std::filesystem::path mcpPreferencesFile();
private:
    static std::filesystem::path xdgDirectory(const char* variable, const char* fallback);
};

#endif // SOS_PATH_RESOLUTION_HPP
