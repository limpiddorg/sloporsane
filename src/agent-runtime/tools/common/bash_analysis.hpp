#ifndef SOS_BASH_ANALYSIS_HPP
#define SOS_BASH_ANALYSIS_HPP

#include <filesystem>
#include <string>
#include <vector>

struct BashAnalysis {
    bool readOnly = true;
    std::vector<std::string> commands;
    std::vector<std::string> approvalScopes;
    std::vector<std::filesystem::path> paths;
};

// Syntactic permission analysis, not an OS sandbox. Unknown constructs are potentially mutating.
BashAnalysis analyzeBash(const std::string& command, const std::filesystem::path& cwd);

#endif // SOS_BASH_ANALYSIS_HPP
