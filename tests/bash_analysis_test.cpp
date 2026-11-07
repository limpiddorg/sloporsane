#include <agent-runtime/tools/common/bash_analysis.hpp>
#include <gtest/gtest.h>

TEST(BashAnalysisTest, RecognizesReadOnlyCommandsAndTreatsUnknownEffectsAsMutating) {
    for (const auto* command : {"pwd", "git status --short", "git diff --stat", "ls -la", "cat 'file with spaces' | head -n 5"})
        EXPECT_TRUE(analyzeBash(command, "/workspace").readOnly) << command;
    for (const auto* command : {"rm file", "python script.py", "npm test", "git checkout main", "git diff --output=result",
        "echo hi > file", "cat file | tee other", "echo $(touch x)", "ls; touch x", "rg --pre=script foo", "find . -delete",
        "VAR=value ls", "for x in a; do echo $x; done", "echo 'broken"})
        EXPECT_FALSE(analyzeBash(command, "/workspace").readOnly) << command;
}

TEST(BashAnalysisTest, DetectsExplicitExternalPathsAndRedirectionTargets) {
    const auto analysis = analyzeBash("cat /outside/a.txt && echo done > ../other/b.txt", "/workspace/project");
    EXPECT_FALSE(analysis.readOnly);
    EXPECT_NE(std::find(analysis.paths.begin(), analysis.paths.end(), "/outside/a.txt"), analysis.paths.end());
    EXPECT_NE(std::find(analysis.paths.begin(), analysis.paths.end(), "/workspace/other/b.txt"), analysis.paths.end());
    EXPECT_EQ(analysis.approvalScopes, (std::vector<std::string>{"cat /outside/a.txt && echo done > ../other/b.txt"}));
}

TEST(BashAnalysisTest, ApprovalsKeepUnknownInterpretersExactAndSeparateShellCommands) {
    EXPECT_EQ(analyzeBash("git status && rm a", "/workspace").approvalScopes,
        (std::vector<std::string>{"git status *", "rm *"}));
    EXPECT_EQ(analyzeBash("python script.py --flag", "/workspace").approvalScopes,
        (std::vector<std::string>{"python script.py --flag"}));
}
