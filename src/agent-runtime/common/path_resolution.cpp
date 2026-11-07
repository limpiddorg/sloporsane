#include "path_resolution.hpp"
#include <cstdlib>
#include <stdexcept>

std::filesystem::path PathResolution::home() {
    const auto* value = std::getenv("HOME");
    if (!value || !std::filesystem::path(value).is_absolute()) throw std::runtime_error("Set HOME to an absolute directory");
    return value;
}

std::filesystem::path PathResolution::xdgDirectory(const char* variable, const char* fallback) {
    if (const auto* value = std::getenv(variable); value && std::filesystem::path(value).is_absolute())
        return std::filesystem::path(value) / "sos";
    return home() / fallback / "sos";
}

std::filesystem::path PathResolution::dataDirectory() { return xdgDirectory("XDG_DATA_HOME", ".local/share"); }
std::filesystem::path PathResolution::stateDirectory() { return xdgDirectory("XDG_STATE_HOME", ".local/state"); }
std::filesystem::path PathResolution::configDirectory() { return xdgDirectory("XDG_CONFIG_HOME", ".config"); }
std::filesystem::path PathResolution::cacheDirectory() { return xdgDirectory("XDG_CACHE_HOME", ".cache"); }
std::filesystem::path PathResolution::binDirectory() { return home() / ".local/bin"; }
std::filesystem::path PathResolution::authFile() { return dataDirectory() / "auth.json"; }
std::filesystem::path PathResolution::configFile() { return configDirectory() / "config.json"; }
std::filesystem::path PathResolution::skillsDirectory() { return configDirectory() / "skills"; }
std::filesystem::path PathResolution::sessionsDirectory() { return stateDirectory() / "sessions"; }
std::filesystem::path PathResolution::daemonLog() { return stateDirectory() / "sosd.log"; }
std::filesystem::path PathResolution::installedAppsDirectory() { return dataDirectory() / "app"; }
std::filesystem::path PathResolution::managedToolsDirectory() { return dataDirectory() / "bin"; }
std::filesystem::path PathResolution::snapshotsDirectory() { return dataDirectory() / "snapshots"; }
std::filesystem::path PathResolution::worktreesDirectory() { return dataDirectory() / "worktrees"; }
std::filesystem::path PathResolution::workspacesDirectory() { return dataDirectory() / "workspaces"; }
std::filesystem::path PathResolution::mcpConfigFile() { return configDirectory() / "mcps.json"; }
std::filesystem::path PathResolution::mcpAuthFile() { return dataDirectory() / "mcps/auth.json"; }
std::filesystem::path PathResolution::mcpPreferencesFile() { return configDirectory() / "mcp-preferences.json"; }
