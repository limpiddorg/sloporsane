#ifndef SOS_IMPLEMENTATION_BOUNDARIES_HPP
#define SOS_IMPLEMENTATION_BOUNDARIES_HPP

#include "session_database.hpp"
#include "../snapshots/turn_files.hpp"
#include <agent-runtime/conversation/messages/implementation_completion.hpp>

struct ImplementationProgress {
    std::optional<std::size_t> substep;
    std::string status, explanation;
    std::optional<ImplementationCompletion> completion;
};
void to_json(nlohmann::json& json, const ImplementationProgress& progress);
void from_json(const nlohmann::json& json, ImplementationProgress& progress);

struct ImplementationBoundary {
    std::int64_t id = 0;
    ImplementationProgress progress;
    TurnSnapshot snapshot;
    std::optional<std::size_t> owner, activeSubstep;
    bool validating = false;
    std::vector<ImplementationDiff> diffs;
};
void to_json(nlohmann::json& json, const ImplementationBoundary& boundary);
void from_json(const nlohmann::json& json, ImplementationBoundary& boundary);

// Append-only evidence captured synchronously on the tool worker, before the
// next edit can run. Each owner has its own SQLite connection, never shared state.
class ImplementationBoundaries {
public:
    explicit ImplementationBoundaries(const std::filesystem::path& directory);
    std::vector<ImplementationBoundary> read(std::size_t step, std::uint64_t generation = 0) const;
    void requireActiveSubstep(std::size_t step, bool command = false, std::uint64_t generation = 0) const;
    std::int64_t capture(std::size_t step, std::size_t count, const ImplementationProgress& progress,
        const TurnSnapshot& snapshot, std::uint64_t generation = 0, std::int64_t progressAfter = 0,
        const std::vector<std::string>& mutationPaths = {}, bool autoAdvance = false);
    void seed(std::size_t step, std::uint64_t generation, const std::vector<ImplementationBoundary>& evidence);
private:
    void advance(ImplementationBoundary& boundary, std::size_t count,
        const std::vector<ImplementationBoundary>& previous) const;
    std::filesystem::path directory;
    mutable SessionDatabase db;
};

#endif // SOS_IMPLEMENTATION_BOUNDARIES_HPP
