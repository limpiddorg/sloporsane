#ifndef SOS_EXECUTION_MODE_HPP
#define SOS_EXECUTION_MODE_HPP

#include <stdexcept>
#include <string>

enum class ExecutionMode { SANE, SLOP };

// A daemon-owned execution scope, not a user-selectable Sane/Slop mode.
// Planning keeps the usual mode restrictions; review is always read-only.
enum class ImplementationPhase { PLANNING, EXECUTION, REVIEW };

inline ExecutionMode parseExecutionMode(const std::string& value) {
    if (value == "sane") return ExecutionMode::SANE;
    if (value == "slop") return ExecutionMode::SLOP;
    throw std::invalid_argument("Expected sane or slop mode");
}

inline const char* executionModeName(ExecutionMode mode) {
    return mode == ExecutionMode::SANE ? "sane" : "slop";
}

#endif // SOS_EXECUTION_MODE_HPP
