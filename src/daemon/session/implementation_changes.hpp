#ifndef SOS_IMPLEMENTATION_CHANGES_HPP
#define SOS_IMPLEMENTATION_CHANGES_HPP
#include "../snapshots/change_diff.hpp"
#include <agent-runtime/conversation/messages/implementation_completion.hpp>
#include "implementation_boundaries.hpp"

nlohmann::json implementationChangeEvidence(const std::vector<ImplementationDiff>& diffs);
std::vector<std::vector<ImplementationDiff>> preserveImplementationOwnership(
    const std::vector<ImplementationBoundary>& boundaries, const std::vector<ImplementationDiff>& finalDiffs,
    std::size_t count);
#endif // SOS_IMPLEMENTATION_CHANGES_HPP
