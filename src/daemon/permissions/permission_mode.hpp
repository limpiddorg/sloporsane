#ifndef SOS_PERMISSION_MODE_HPP
#define SOS_PERMISSION_MODE_HPP

#include <stdexcept>
#include <string>

enum class PermissionMode { ASK, AUTO, DANGER };

inline PermissionMode parsePermissionMode(const std::string& value) {
    if (value == "ask") return PermissionMode::ASK;
    if (value == "auto") return PermissionMode::AUTO;
    if (value == "danger") return PermissionMode::DANGER;
    throw std::invalid_argument("Expected ask, auto or danger permission mode");
}

inline const char* permissionModeName(PermissionMode mode) {
    switch (mode) {
        case PermissionMode::ASK: return "ask";
        case PermissionMode::AUTO: return "auto";
        case PermissionMode::DANGER: return "danger";
    }
    throw std::invalid_argument("Unknown permission mode");
}

#endif // SOS_PERMISSION_MODE_HPP
