#ifndef SOS_IMPLEMENTATION_RUN_HPP
#define SOS_IMPLEMENTATION_RUN_HPP

#include "session_database.hpp"
#include "../snapshots/turn_snapshot.hpp"
#include "../snapshots/change_diff.hpp"
#include "implementation_boundaries.hpp"
#include <agent-runtime/conversation/messages/implementation_plan.hpp>
#include <agent-runtime/conversation/messages/implementation_completion.hpp>
#include <optional>
#include <chrono>
#include <map>

struct ImplementationItem {
    bool complete = false;
    bool reviewed = false;
    std::string explanation;
    std::vector<ImplementationDiff> diffs;
    std::optional<TurnSnapshot> snapshot;
    bool started = false, announced = false;
    bool finalized = false;
    bool unnecessary = false;
    std::vector<Message> activity;
    std::optional<std::uint64_t> durationMs;
    std::optional<ModelCallMetadata> modelStatistics;
};
void to_json(nlohmann::json& json, const ImplementationItem& item);
void from_json(const nlohmann::json& json, ImplementationItem& item);

struct ImplementationStepState {
    std::vector<ImplementationItem> substeps;
    ImplementationItem whole;
    ImplementationItem validation;
    std::uint64_t generation = 0;
    std::string identity;
    std::uint64_t displayRevision = 0;
    std::int64_t progressBoundary = 0;
    std::string conclusion;
    std::string reviewBranch;
};
void to_json(nlohmann::json& json, const ImplementationStepState& step);
void from_json(const nlohmann::json& json, ImplementationStepState& step);

struct ImplementationRevisionProposal {
    std::string request;
    std::optional<ImplementationPlan> plan;
    std::vector<std::optional<std::size_t>> retainedSteps;
    std::vector<std::vector<std::size_t>> substepOwners;
};
void to_json(nlohmann::json& json, const ImplementationRevisionProposal& proposal);
void from_json(const nlohmann::json& json, ImplementationRevisionProposal& proposal);
struct ImplementationRevisionArchive {
    ImplementationPlan plan;
    std::vector<ImplementationStepState> steps;
    std::string request;
    std::uint64_t revision = 0;
    std::size_t planNumber = 0;
    std::size_t reworkedStep = 0;
};
inline void to_json(nlohmann::json& json, const ImplementationRevisionArchive& archive) {
    json = {{"plan", archive.plan}, {"steps", archive.steps}, {"request", archive.request}, {"revision", archive.revision}, {"plan_number", archive.planNumber}};
    json["reworked_step"] = archive.reworkedStep;
}
inline void from_json(const nlohmann::json& json, ImplementationRevisionArchive& archive) {
    json.at("plan").get_to(archive.plan); json.at("steps").get_to(archive.steps);
    json.at("request").get_to(archive.request); json.at("revision").get_to(archive.revision);
    json.at("plan_number").get_to(archive.planNumber);
    json.at("reworked_step").get_to(archive.reworkedStep);
    if (!archive.planNumber || archive.reworkedStep >= archive.steps.size())
        throw std::invalid_argument("Invalid implementation revision archive");
}
struct ImplementationRevisionSource {
    ImplementationStepState state;
    std::vector<std::size_t> owners;
    bool seeded = false;
    std::string conflicts;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ImplementationRevisionSource, state, owners, seeded, conflicts)
struct ImplementationRevisionSeed {
    TurnSnapshot snapshot;
    std::vector<ImplementationBoundary> boundaries;
    std::string conflicts;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ImplementationRevisionSeed, snapshot, boundaries, conflicts)
struct ImplementationPlanVersion {
    std::size_t number = 1;
    std::string beforeStep;
    ImplementationPlan plan;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ImplementationPlanVersion, number, beforeStep, plan)

// Durable construction and review cursors. Only the daemon advances construction.
class ImplementationRun {
public:
    explicit ImplementationRun(const std::filesystem::path& directory);
    bool exists() const { return plan.has_value(); }
    bool fullyReviewed() const { return exists() && status == "completed" && reviewStep == steps.size(); }
    std::optional<std::size_t> planHistoryEnd() const {
        if (steps.empty() || steps.front().substeps.empty() || !steps.front().substeps.front().snapshot) return {};
        return steps.front().substeps.front().snapshot->start;
    }
    void start(const ImplementationPlan& accepted, const std::string& pace);
    bool needsCheckout() const { return checkoutPending; }
    void checkoutPrepared() { checkoutPending = false; save(); }
    void resume();
    void begin(const TurnSnapshot& snapshot);
    void recover(const std::optional<TurnSnapshot>& snapshot);
    std::optional<std::pair<std::size_t, std::size_t>> next() const;
    std::size_t currentSubstepCount() const { return steps.at(workStep).substeps.size(); }
    std::string reviewConstructionKey(std::size_t step) const {
        return steps.at(step).identity + ":" + std::to_string(steps.at(step).generation);
    }
    std::uint64_t currentGeneration() const { return steps.at(workStep).generation; }
    std::int64_t currentProgressBoundary() const { return steps.at(workStep).progressBoundary; }
    void finish(const TurnSnapshot& snapshot, const ImplementationCompletion& completion,
        std::vector<ImplementationDiff> diffs, bool completed,
        const std::string& error = {});
    std::optional<TurnSnapshot> baseline() const;
    void stop(const std::string& reason);
    void recordActivity(const std::vector<Message>& messages);
    void setReviewBranch(std::size_t step, const std::string& id);
    std::string stepReviewBranch(std::size_t step) const { return steps.at(step).reviewBranch; }
    std::size_t stepCount() const { return steps.size(); }
    std::vector<std::string> previousReviewBranches(std::size_t step) const;
    void saveReviewConstruction(std::size_t step, const std::vector<Message>& messages);
    std::optional<std::vector<Message>> reviewConstruction(std::size_t step, bool latestGeneration = false);
    TurnSnapshot reviewFiles(std::size_t step) const;
    std::optional<std::size_t> reviewHistoryEnd(std::size_t step) const;
    TurnSnapshot substepCombined(const TurnSnapshot& latest) const;
    void review(std::size_t step, std::size_t substep, bool validate);
    std::optional<TurnSnapshot> importBoundary(std::size_t step, std::size_t substep) const;
    const ImplementationItem& item(std::size_t step, std::size_t substep) const;
    std::string instruction() const;
    nlohmann::json view() const;
    nlohmann::json versions() const;
    nlohmann::json repositoryDirectory(std::size_t step, const std::string& tree, const std::string& path) const;
    TurnSnapshot repositoryBaseline(std::size_t step, const std::string& tree) const;
    std::string reviewContext(std::size_t step, std::size_t substep, const std::string& path = {},
        std::optional<std::size_t> hunk = {}) const;
    bool canRework(std::size_t step) const;
    std::uint64_t revisionVersion() const { return revision; }
    std::size_t reviewIndex() const { return reviewStep; }
    void proposeRework(std::size_t step, std::uint64_t expected, const std::string& request);
    void proposeRevision(std::size_t step, std::uint64_t expected, ImplementationRevisionProposal proposal);
    bool hasRevisionProposal() const { return !proposedRework.empty(); }
    void rejectRevision(std::uint64_t expected);
    void openRevisionPlan(std::uint64_t expected);
    bool planningRevision() const { return revisionPlanning; }
    std::string revisionPlanContext() const;
    void setRevisionBranch(const std::string& id) { revisionBranch = id; save(); }
    std::string revisionBranch;
    TurnSnapshot prepareRevision(std::uint64_t expected);
    void commitRevision(std::uint64_t expected);
    bool needsRevisionSeed() const;
    const ImplementationRevisionSource& revisionSource() const { return revisionSources.at(workStep); }
    void seedRevision(const ImplementationRevisionSeed& seed);
    nlohmann::json history(std::optional<std::size_t> step = {}) const;
    void acceptRework(std::uint64_t expected);
    std::string status = "idle", error, pace, reviewBranch;
private:
    void applyBoundaries(std::int64_t through);
    void recordCapturedActivity(const Message& message);
    void recordStatistics(const Message& message, ImplementationItem& item);
    std::string resumeProgress() const;
    std::string completionGuidance() const;
    void finishCapturedChanges(ImplementationStepState& step, const TurnSnapshot& snapshot, const std::vector<ImplementationDiff>& diffs);
    void save();
    void validateRevision(const ImplementationRevisionProposal& proposal) const;
    void captureRevisionSources();
    std::string correctionInstruction() const;
    nlohmann::json data() const;
    std::filesystem::path storageDirectory;
    SessionDatabase db;
    ImplementationBoundaries boundaries;
    std::optional<ImplementationPlan> plan;
    std::vector<ImplementationStepState> steps;
    std::size_t workStep = 0, workSubstep = 0, reviewStep = 0, reviewSubstep = 0;
    bool checkoutPending = false;
    std::int64_t activeTurn = 0;
    std::uint64_t activityElapsedMs = 0;
    std::optional<std::chrono::steady_clock::time_point> executionStarted;
    std::int64_t boundaryCursor = 0;
    std::size_t activitySubstep = 0;
    std::map<std::string, std::optional<std::size_t>> activityRoutes;
    std::uint64_t revision = 0;
    std::string proposedRework, acceptedRework;
    std::optional<ImplementationRevisionProposal> proposedRevision;
    std::optional<TurnSnapshot> revisionRollback;
    std::map<std::size_t, ImplementationRevisionSource> revisionSources;
    std::vector<ImplementationRevisionArchive> revisionArchive;
    bool revisionPlanning = false;
    std::vector<ImplementationPlanVersion> planVersions;
    std::map<std::size_t, std::vector<ImplementationStepState>> revisionHistory;
};

#endif // SOS_IMPLEMENTATION_RUN_HPP
