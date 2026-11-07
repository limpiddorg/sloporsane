#ifndef SOS_MCP_PREFERENCES_HPP
#define SOS_MCP_PREFERENCES_HPP

#include "server_connection.hpp"
#include "json_file.hpp"

namespace mcp {
struct Preference {
    std::optional<bool> enabled;
    std::optional<bool> connected;
};

// One daemon-wide user choice per server name, independent of configuration identity.
// Publication only touches connection state; network work runs outside this lock.
class Preferences {
public:
    Preferences();
    void attach(const std::shared_ptr<ServerConnection>& connection);
    void enable(const std::string& name, bool value);
    void connection(const std::string& name, bool value, bool enabled);
private:
    void update(const std::string& name, bool value, bool connection, std::optional<bool> enabled = {});
    JsonFile file;
    std::mutex mutex;
    std::map<std::string, Preference> choices;
    std::vector<std::weak_ptr<ServerConnection>> connections;
};
}
#endif // SOS_MCP_PREFERENCES_HPP
