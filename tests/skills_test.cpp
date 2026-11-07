#include <agent-runtime/skills/skill_catalog.hpp>
#include <agent-runtime/skills/project_instructions.hpp>
#include <agent-runtime/tools/fs/read.hpp>
#include <agent-runtime/agent.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <cstdlib>
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;
class SkillsTest : public testing::Test {
protected:
    fs::path root;
    std::optional<std::string> previousConfig;
    void SetUp() override {
        root = fs::temp_directory_path() / ("sos-skills-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        root = fs::canonical(root);
        if (const auto* value = std::getenv("XDG_CONFIG_HOME")) previousConfig = value;
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    }
    void TearDown() override {
        if (previousConfig) setenv("XDG_CONFIG_HOME", previousConfig->c_str(), 1);
        else unsetenv("XDG_CONFIG_HOME");
        fs::remove_all(root);
    }
    void write(const fs::path& path, const std::string& text) {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path) << text;
    }
    void skill() {
        write(".sos/skills/check-api/SKILL.md", R"(---
name: check-api
description: Check HTTP routes.
arguments: [directory]
argument-hint: "[directory]"
---
Read scripts/check.py before running it with directory $directory.
)");
    }
};
TEST_F(SkillsTest, CatalogContainsMetadataOnlyAndLoadDoesNotExecuteOrReadScripts) {
    skill();
    write(".sos/skills/check-api/scripts/check.py", "SECRET_SCRIPT_BODY\n");
    SkillCatalog catalog(root);
    ASSERT_EQ(catalog.metadata().size(), 1);
    EXPECT_TRUE(catalog.errors().empty());
    EXPECT_EQ(catalog.prompt().find("Read scripts/check.py"), std::string::npos);
    EXPECT_EQ(catalog.metadata()[0]["arguments"], json::array({"directory"}));
    EXPECT_FALSE(catalog.metadata()[0].contains("scripts"));
    EXPECT_EQ(catalog.metadata(true)[0]["scripts"], json::array({"scripts/check.py"}));
    EXPECT_EQ(catalog.metadata(true).dump().find("SECRET_SCRIPT_BODY"), std::string::npos);
    const auto output = catalog.load({"check-api", "src/http"});
    EXPECT_NE(output.find("Name: check-api\nLocation: " + (root / ".sos/skills/check-api").string()), std::string::npos);
    EXPECT_NE(output.find("src/http"), std::string::npos);
    EXPECT_NE(output.find("Content:\nRead scripts/check.py"), std::string::npos);
    EXPECT_NE(output.find("- scripts/check.py"), std::string::npos);
    EXPECT_EQ(output.find("SECRET_SCRIPT_BODY"), std::string::npos);
    ToolContext context{root, root, [](const auto&) {}};
    SkillTool tool(std::make_shared<SkillCatalog>(catalog));
    EXPECT_EQ(tool.execute({{"name", "check-api"}, {"input", "src/http"}}, context).output, output);
    context.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    EXPECT_THROW(tool.execute({{"name", "check-api"}, {"input", "src"}}, context), std::runtime_error);
}
TEST_F(SkillsTest, ToolPreservesArgumentNamesAndParsedValuesForDisplay) {
    skill();
    write(".sos/skills/raw/SKILL.md", "---\nname: raw\ndescription: Test.\n---\n$ARGUMENTS");
    SkillTool tool(std::make_shared<SkillCatalog>(root));
    ToolContext context{root, root, [](const auto&) {}};
    const auto named = tool.execute({{"name", "check-api"}, {"input", "'src/http routes' extra"}}, context);
    EXPECT_EQ(named.metadata["argument_names"], json::array({"directory"}));
    EXPECT_EQ(named.metadata["argument_values"], json::array({"src/http routes", "extra"}));
    const auto empty = tool.execute({{"name", "raw"}}, context);
    EXPECT_EQ(empty.metadata["argument_names"], json::array());
    EXPECT_EQ(empty.metadata["argument_values"], json::array());
    const auto raw = tool.execute({{"name", "raw"}, {"input", "l'utilisateur \"unfinished"}}, context);
    EXPECT_EQ(raw.metadata["argument_values"], json::array({"l'utilisateur \"unfinished"}));
    EXPECT_NE(raw.output.find("l'utilisateur"), std::string::npos);
}

TEST_F(SkillsTest, MetadataPreservesPositionalNamesAndDisplayHint) {
    write(".sos/skills/ordered/SKILL.md", "---\nname: ordered\ndescription: Test.\n"
        "arguments: [zebra, alpha, beta]\nargument-hint: '[first] [second] [third]'\n---\n$zebra / $alpha / $beta");
    const SkillCatalog catalog(root);
    ASSERT_TRUE(catalog.errors().empty());
    EXPECT_EQ(catalog.metadata()[0]["arguments"], json::array({"zebra", "alpha", "beta"}));
    EXPECT_EQ(catalog.metadata(true)[0]["argument-hint"], "[first] [second] [third]");
    EXPECT_FALSE(catalog.metadata(true)[0].contains("argument_order"));
    EXPECT_NE(catalog.load({"ordered", "one two three extra"}).find("Content:\none / two / three"), std::string::npos);
    EXPECT_NE(catalog.load({"ordered", ""}).find("Content:\n /  / "), std::string::npos);
}

TEST_F(SkillsTest, ArgumentDescriptionsAreOptionalMetadataNotValueValidation) {
    write(".sos/skills/described/SKILL.md", R"(---
name: described
description: Test.
arguments: [zebra, alpha]
argument-hint: "<zebra> [alpha]"
argument-descriptions:
  alpha: Optional information.
  zebra: Directory to inspect.
---
Inspect $ARGUMENTS.
)");
    const SkillCatalog catalog(root);
    ASSERT_TRUE(catalog.errors().empty());
    const auto descriptions = json{{"zebra", "Directory to inspect."}, {"alpha", "Optional information."}};
    EXPECT_EQ(catalog.metadata()[0]["argument-descriptions"], descriptions);
    EXPECT_EQ(catalog.metadata(true)[0]["argument-descriptions"], descriptions);
    EXPECT_EQ(catalog.metadata(true)[0]["arguments"], json::array({"zebra", "alpha"}));
    EXPECT_EQ(catalog.metadata(true)[0]["argument-hint"], "<zebra> [alpha]");
    EXPECT_NO_THROW(catalog.load({"described", ""}));
    EXPECT_NO_THROW(catalog.load({"described", "one two three extra"}));
    EXPECT_NE(catalog.load({"described", "l'utilisateur \"unfinished"}).find("Inspect l'utilisateur \"unfinished."), std::string::npos);
    EXPECT_NE(catalog.load({"described", ""}).find("Directory to inspect."), std::string::npos);
    skill();
    EXPECT_FALSE(SkillCatalog(root).metadata()[0].contains("argument-descriptions"));
}

TEST_F(SkillsTest, InvalidArgumentDescriptionMetadataIsReported) {
    for (const std::string fields : {"argument-descriptions: []\n", "argument-descriptions: null\n",
            "argument-descriptions: {name: []}\n", "argument-descriptions: {name: null}\n",
            "argument-descriptions: {name: ''}\n",
            "argument-descriptions:\n  name: First\n  name: Second\n"}) {
        SCOPED_TRACE(fields);
        write(".sos/skills/described/SKILL.md", "---\nname: described\ndescription: Test.\n" + fields + "---\nBody");
        EXPECT_EQ(SkillCatalog(root).errors().size(), 1);
        EXPECT_TRUE(SkillCatalog(root).metadata().empty());
    }
    write(".sos/skills/described/SKILL.md", "---\nname: described\ndescription: Test.\nargument-descriptions: {}\n---\nBody");
    EXPECT_EQ(SkillCatalog(root).metadata()[0]["argument-descriptions"], json::object());
}

TEST_F(SkillsTest, UnknownFrontmatterIsIgnoredWithoutGrantingCapabilities) {
    write(".sos/skills/foreign/SKILL.md", R"(---
name: foreign
description: Foreign skill.
allowed-tools: Bash(*)
disable-model-invocation: true
context: fork
agent: custom-agent
model: custom-model
hooks:
  SessionStart: {command: "touch foreign-hook-ran"}
unknown-extension: [anything, {nested: value}]
unknown-extension: ignored duplicate
---
Only these instructions.
)");
    const SkillCatalog catalog(root);
    ASSERT_TRUE(catalog.errors().empty());
    const auto expected = json{{"name", "foreign"}, {"description", "Foreign skill."},
        {"arguments", json::array()}, {"source", "workspace"}};
    EXPECT_EQ(catalog.metadata()[0], expected);
    const auto loaded = catalog.load({"foreign", ""});
    EXPECT_NE(loaded.find("Only these instructions."), std::string::npos);
    EXPECT_EQ(loaded.find("foreign-hook-ran"), std::string::npos);
    EXPECT_EQ(catalog.prompt().find("custom-agent"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "foreign-hook-ran"));
}

TEST_F(SkillsTest, ValidatesTransportButDoesNotRequireOrBindArgumentValues) {
    skill();
    const SkillCatalog catalog(root);
    EXPECT_NO_THROW(catalog.load({"check-api", ""}));
    EXPECT_NO_THROW(catalog.load({"check-api", "one two three"}));
    EXPECT_THROW(catalog.load({"missing", ""}), std::invalid_argument);
    EXPECT_THROW((json{{"name", "check-api"}, {"arguments", json::object()}}.get<SkillInvocation>()), std::invalid_argument);
    EXPECT_THROW((json{{"name", "../outside"}}.get<SkillInvocation>()), std::invalid_argument);
    const auto output = catalog.load({"check-api", "'$(touch marker)'"});
    EXPECT_NE(output.find("directory $(touch marker)"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "marker"));
}

TEST_F(SkillsTest, FreeFormInputIsLiteralAndRoundTrips) {
    write(".sos/skills/caveman/SKILL.md", "---\nname: caveman\ndescription: Brief replies.\n---\nRespond briefly.\n");
    skill();
    const SkillCatalog catalog(root);
    EXPECT_TRUE(catalog.errors().empty());
    const std::string input = "status \"unfinished\n$(touch marker) $directory \\";
    const json request = {{"name", "caveman"}, {"input", input}};
    const auto invocation = request.get<SkillInvocation>();
    EXPECT_EQ(json(invocation), request);
    const auto output = catalog.load(invocation);
    EXPECT_NE(output.find("Input (JSON string data):\n" + json(input).dump()), std::string::npos);
    EXPECT_NE(output.find("Content:\nRespond briefly.\n"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "marker"));
    SkillTool tool(std::make_shared<SkillCatalog>(catalog));
    ToolContext context{root, root, [](const auto&) {}};
    EXPECT_EQ(tool.execute(request, context).output, output);
    EXPECT_TRUE(tool.getDefinition().parameters["properties"].contains("input"));
    EXPECT_THROW((json{{"name", "caveman"}, {"input", 42}}.get<SkillInvocation>()), std::invalid_argument);
    EXPECT_THROW((json{{"name", "caveman"}, {"input", nullptr}}.get<SkillInvocation>()), std::invalid_argument);
    EXPECT_EQ(json(SkillInvocation{"caveman", {}}), (json{{"name", "caveman"}, {"input", ""}}));
}

TEST_F(SkillsTest, DollarTemplatesExpandRawAndIndexedArgumentsBeforeModelGeneration) {
    write(".sos/skills/template/SKILL.md", "---\nname: template\ndescription: Templates.\n---\n"
        "Raw: $ARGUMENTS\nPositions: $0 / $1 / $ARGUMENTS[2] / $10\nMissing: $3 / $ARGUMENTS[99]\n");
    const SkillCatalog catalog(root);
    const std::string input = "\"hello world\" second '$(touch marker)'";
    const auto output = catalog.load({"template", input});
    EXPECT_NE(output.find("Raw: " + input), std::string::npos);
    EXPECT_NE(output.find("Positions: hello world / second / $(touch marker) / $10"), std::string::npos);
    EXPECT_NE(output.find("Missing: $3 / $ARGUMENTS[99]"), std::string::npos);
    EXPECT_EQ(output.find("\n\nARGUMENTS: "), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "marker"));
    const auto empty = catalog.load({"template", {}});
    EXPECT_NE(empty.find("Raw: \nPositions: $0 / $1 / $ARGUMENTS[2] / $10"), std::string::npos);
}

TEST_F(SkillsTest, DollarTemplatesHonorEscapesMissingIndicesAndSinglePassExpansion) {
    write(".sos/skills/template/SKILL.md", R"(---
name: template
description: Templates.
---
Escaped: \$0 \$ARGUMENTS \$ARGUMENTS[1] \$1.00
Double: \\$0
Triple: \\\$1
Values: $0 / $1
Huge: $999999999999999999999999999999999999999
Other: $HOME \$HOME $ARGUMENTS_suffix
)");
    const SkillCatalog catalog(root);
    const auto output = catalog.load({"template", "'$1 {note}' '$ARGUMENTS'"});
    EXPECT_NE(output.find("Escaped: $0 $ARGUMENTS $ARGUMENTS[1] $1.00"), std::string::npos);
    EXPECT_NE(output.find("Double: \\\\$1 {note}"), std::string::npos);
    EXPECT_NE(output.find("Triple: \\\\\\$ARGUMENTS"), std::string::npos);
    EXPECT_NE(output.find("Values: $1 {note} / $ARGUMENTS"), std::string::npos);
    EXPECT_NE(output.find("Huge: $999999999999999999999999999999999999999"), std::string::npos);
    EXPECT_NE(output.find("Other: $HOME \\$HOME $ARGUMENTS_suffix"), std::string::npos);
}

TEST_F(SkillsTest, ArgumentFallbackOnlyAppliesWhenNoTemplateReceivesAnArgument) {
    const auto path = ".sos/skills/template/SKILL.md";
    const std::string header = "---\nname: template\ndescription: Templates.\n---\n";
    for (const std::string body : {"No placeholder.", "Escaped \\$0."}) {
        write(path, header + body);
        const auto output = SkillCatalog(root).load({"template", "keep \"unfinished"});
        EXPECT_NE(output.find("\n\nARGUMENTS: keep \"unfinished"), std::string::npos);
    }
    write(path, header + "Missing $99.");
    EXPECT_NE(SkillCatalog(root).load({"template", "hello"}).find("Missing $99.\n\nARGUMENTS: hello"), std::string::npos);
    write(path, header + "Raw: $ARGUMENTS");
    EXPECT_NO_THROW(SkillCatalog(root).load({"template", "l'utilisateur \"unfinished"}));
    write(path, header + "Position: $0");
    EXPECT_THROW(SkillCatalog(root).load({"template", "\"unfinished"}), std::invalid_argument);
    EXPECT_THROW(SkillCatalog(root).load({"template", "unfinished\\"}), std::invalid_argument);
}

TEST_F(SkillsTest, IndexedArgumentsUseQuotingWithoutEvaluatingShellSyntax) {
    write(".sos/skills/template/SKILL.md", "---\nname: template\ndescription: Templates.\n---\n"
        "$0|$1|$2|$3|$4|$5|$6|$7");
    const auto output = SkillCatalog(root).load({"template",
        R"(one\ two 'three four' "five\"six" '' "a\q" "\$HOME" '$(touch marker)' a"b"'c')"});
    EXPECT_NE(output.find("Content:\none two|three four|five\"six||a\\q|$HOME|$(touch marker)|abc"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "marker"));
}

TEST_F(SkillsTest, NamedAliasesUsePositionsAndNeverReexpandInsertedValues) {
    write(".sos/skills/template/SKILL.md", "---\nname: template\ndescription: Test.\n"
        "arguments: directory note\n---\nNamed: $directory / $note\nPositional: $0 / $1\nRaw: $ARGUMENTS");
    const SkillCatalog catalog(root);
    ASSERT_TRUE(catalog.errors().empty());
    const auto output = catalog.load({"template", "'$1 {note}' hello"});
    EXPECT_NE(output.find("Named: $1 {note} / hello"), std::string::npos);
    EXPECT_NE(output.find("Positional: $1 {note} / hello"), std::string::npos);
    EXPECT_NE(catalog.load({"template", "src"}).find("Named: src / \n"), std::string::npos);
}

TEST_F(SkillsTest, ExpandsNamedAliasesInsideCodeButLeavesBracesAndUnknownNamesLiteral) {
    write(".sos/skills/template/SKILL.md", "---\nname: template\ndescription: Test.\n"
        "arguments: [name, note]\n---\nHello $name! {name} $unknown\n```text\n$note\n```\n");
    const SkillCatalog catalog(root);
    EXPECT_TRUE(catalog.errors().empty());
    const auto output = catalog.load({"template", "Alice 'Line 1\nLine 2'"});
    EXPECT_NE(output.find("Hello Alice! {name} $unknown"), std::string::npos);
    EXPECT_NE(output.find("```text\nLine 1\nLine 2\n```"), std::string::npos);
    EXPECT_NE(catalog.load({"template", "Alice"}).find("```text\n\n```"), std::string::npos);
}

TEST_F(SkillsTest, NamedAliasesMatchWholeNamesAndHonorEscapingAndEmptyValues) {
    write(".sos/skills/template/SKILL.md", R"(---
name: template
description: Test.
arguments: [name, name_long, Name, empty]
---
$name|$name_long|$Name|$empty|$name_extra|\$name|{name}
)");
    const SkillCatalog catalog(root);
    EXPECT_TRUE(catalog.errors().empty());
    const auto output = catalog.load({"template", "first 'second word' third '' extra"});
    EXPECT_NE(output.find("Content:\nfirst|second word|third||$name_extra|$name|{name}"), std::string::npos);
    EXPECT_EQ(output.find("\n\nARGUMENTS:"), std::string::npos);
    EXPECT_THROW(catalog.load({"template", "\"unfinished"}), std::invalid_argument);
}

TEST_F(SkillsTest, RejectsRemovedMappingAndInvalidArgumentDeclarations) {
    for (const std::string fields : {"arguments: {name: {description: Name, required: true}}\n",
            "arguments: [name, name]\n", "arguments: [ARGUMENTS]\n", "arguments: [9bad]\n",
            "arguments: [null]\n", "arguments: null\n", "argument-hint: []\n"}) {
        SCOPED_TRACE(fields);
        write(".sos/skills/template/SKILL.md", "---\nname: template\ndescription: Test.\n" + fields + "---\nBody");
        EXPECT_EQ(SkillCatalog(root).errors().size(), 1);
        EXPECT_TRUE(SkillCatalog(root).metadata().empty());
    }
}

TEST_F(SkillsTest, UnknownPlaceholdersAreLiteralWithoutDiagnostics) {
    write("config/sos/skills/shared/SKILL.md", "---\nname: shared\ndescription: Global\n---\n{unknown}");
    EXPECT_TRUE(SkillCatalog(root).errors().empty());
    write(".sos/skills/shared/SKILL.md", "---\nname: shared\ndescription: Local\n---\nLocal body");
    const SkillCatalog catalog(root);
    EXPECT_TRUE(catalog.errors().empty());
    EXPECT_EQ(catalog.metadata().size(), 1);
}

TEST_F(SkillsTest, OptionalStandardMetadataCoexistsWithPositionalArgumentsAndSurvivesLoading) {
    write(".sos/skills/check-api/SKILL.md", R"(---
name: check-api
description: Check routes.
license: MIT
compatibility: Requires Python 3.11+ and git.
metadata:
  author: example-team
  version: "1.0"
  empty: ""
arguments: [note, directory]
argument-hint: "[directory] [note]"
---
Inspect $directory. Optional note: {note}
)");
    const SkillCatalog catalog(root);
    ASSERT_TRUE(catalog.errors().empty());
    const auto entry = catalog.metadata()[0];
    EXPECT_EQ(entry["license"], "MIT");
    EXPECT_EQ(entry["compatibility"], "Requires Python 3.11+ and git.");
    EXPECT_EQ(entry["metadata"], (json{{"author", "example-team"}, {"version", "1.0"}, {"empty", ""}}));
    EXPECT_EQ(catalog.metadata(true)[0]["metadata"], entry["metadata"]);
    EXPECT_EQ(catalog.metadata(true)[0]["arguments"], json::array({"note", "directory"}));
    EXPECT_NO_THROW(catalog.load({"check-api", {}}));
    const auto output = catalog.load({"check-api", "src"});
    EXPECT_NE(output.find("Skill attributes (JSON data):"), std::string::npos);
    EXPECT_NE(output.find("\"license\": \"MIT\""), std::string::npos);
    EXPECT_NE(output.find("Requires Python 3.11+ and git."), std::string::npos);
    EXPECT_NE(output.find("\"version\": \"1.0\""), std::string::npos);
    EXPECT_NE(catalog.prompt().find("example-team"), std::string::npos);
}

TEST_F(SkillsTest, StandardMetadataIsOptionalAndEmptyMetadataIsPreserved) {
    skill();
    const SkillCatalog catalog(root);
    for (const auto* field : {"license", "compatibility", "metadata"})
        EXPECT_FALSE(catalog.metadata()[0].contains(field));
    EXPECT_EQ(catalog.load({"check-api", "src"}).find("\"license\""), std::string::npos);
    write(".sos/skills/check-api/SKILL.md", "---\nname: check-api\ndescription: Test\nmetadata: {}\n---\nBody");
    EXPECT_EQ(SkillCatalog(root).metadata()[0]["metadata"], json::object());
}

TEST_F(SkillsTest, InvalidStandardMetadataReportsDiagnosticsWithoutHidingOtherSkills) {
    skill();
    for (const std::string fields : {
            "license: []\n", "license: null\n", "license: ' '\n",
            "compatibility: {}\n", "compatibility: null\n", "compatibility: ''\n",
            "metadata: []\n", "metadata: null\n", "metadata: {author: []}\n",
            "metadata: {author: {name: nested}}\n", "metadata: {author: null}\n",
            "metadata:\n  author: first\n  author: second\n",
            "license: MIT\nlicense: Apache-2.0\n"}) {
        SCOPED_TRACE(fields);
        write(".sos/skills/broken/SKILL.md", "---\nname: broken\ndescription: Test\n" + fields + "---\nBody");
        const SkillCatalog catalog(root);
        EXPECT_EQ(catalog.errors().size(), 1);
        ASSERT_EQ(catalog.metadata().size(), 1);
        EXPECT_EQ(catalog.metadata()[0]["name"], "check-api");
    }
}

TEST_F(SkillsTest, CompatibilityLimitCountsUnicodeCharactersNotUtf8Bytes) {
    std::string compatibility;
    for (int index = 0; index < 500; ++index) compatibility += "é";
    const auto frontmatter = "---\nname: check-api\ndescription: Test\ncompatibility: ";
    write(".sos/skills/check-api/SKILL.md", frontmatter + compatibility + "\n---\nBody");
    EXPECT_TRUE(SkillCatalog(root).errors().empty());
    write(".sos/skills/check-api/SKILL.md", frontmatter + compatibility + "x\n---\nBody");
    EXPECT_EQ(SkillCatalog(root).errors().size(), 1);
}

TEST_F(SkillsTest, UserSkillsMergeWithWorkspaceAndWorkspaceNamesWin) {
    write("config/sos/skills/shared/SKILL.md", "---\nname: shared\ndescription: Global\n---\nGlobal body");
    write("config/sos/skills/global-only/SKILL.md", "---\nname: global-only\ndescription: Global only\n---\nUser body");
    write("config/sos/skills/global-only/scripts/run.py", "SCRIPT_NOT_LOADED");
    write(".sos/skills/shared/SKILL.md", "---\nname: shared\ndescription: Local\n---\nLocal body");
    write(".claude/skills/ignored/SKILL.md", "Ignored");
    write("config/.agents/skills/ignored/SKILL.md", "Ignored");
    SkillCatalog catalog(root);
    ASSERT_EQ(catalog.metadata().size(), 2);
    EXPECT_EQ(catalog.metadata()[0]["source"], "user");
    EXPECT_EQ(catalog.metadata()[1]["source"], "workspace");
    EXPECT_NE(catalog.load({"shared", {}}).find("Local body"), std::string::npos);
    EXPECT_EQ(catalog.load({"shared", {}}).find("Global body"), std::string::npos);
    EXPECT_NE(catalog.load({"global-only", {}}).find("scripts/run.py"), std::string::npos);
    EXPECT_EQ(catalog.load({"global-only", {}}).find("SCRIPT_NOT_LOADED"), std::string::npos);
    EXPECT_NE(catalog.prompt().find(PathResolution::skillsDirectory().string()), std::string::npos);
    EXPECT_EQ(catalog.prompt().find("Create <name>/SKILL.md"), std::string::npos);
    EXPECT_EQ(catalog.prompt().find("frontmatter"), std::string::npos);
    EXPECT_EQ(catalog.prompt().find("execute scripts"), std::string::npos);
    ToolContext context{root, root, [&](const auto& request) {
        EXPECT_EQ(request.patterns[0], (PathResolution::skillsDirectory() / "global-only/SKILL.md").string());
    }};
    SkillTool tool(std::make_shared<SkillCatalog>(catalog));
    EXPECT_NO_THROW(tool.execute({{"name", "global-only"}, {"input", ""}}, context));
    write(".sos/skills/shared/SKILL.md", "Invalid local skill");
    EXPECT_THROW(SkillCatalog(root).load({"shared", {}}), std::invalid_argument);
}

TEST_F(SkillsTest, UserSkillSymlinksAreRejected) {
    skill();
    fs::create_directories(root / "config/sos");
    fs::create_directory_symlink(root / ".sos/skills", root / "config/sos/skills");
    SkillCatalog catalog(root);
    EXPECT_EQ(catalog.errors().size(), 1);
    EXPECT_EQ(catalog.metadata().size(), 1);
    EXPECT_EQ(catalog.metadata()[0]["source"], "workspace");
}
TEST_F(SkillsTest, InvalidSkillsAreReportedAndOtherSourcesIgnored) {
    skill();
    write(".sos/skills/broken/SKILL.md", "---\nname: broken\n---\nBody\n");
    write(".agents/skills/ignored/SKILL.md", "Ignored");
    write(".sos/skills/mismatch/SKILL.md", "---\nname: other\ndescription: test\n---\nBody");
    SkillCatalog catalog(root);
    EXPECT_EQ(catalog.metadata().size(), 1);
    EXPECT_EQ(catalog.errors().size(), 2);
    write(".sos/skills/check-api/SKILL.md", "---\nname: check-api\ndescription: changed\n---\nNew instructions");
    EXPECT_EQ(catalog.metadata()[0]["description"], "Check HTTP routes.");
    EXPECT_EQ(SkillCatalog(root).metadata()[0]["description"], "changed");
}
TEST_F(SkillsTest, RejectsDuplicateYamlFieldsAndRemovedTypeField) {
    write(".sos/skills/duplicate/SKILL.md", "---\nname: duplicate\nname: duplicate\ndescription: test\n---\nBody");
    write(".sos/skills/typed/SKILL.md", "---\nname: typed\ndescription: test\narguments:\n  count:\n    type: string\n    description: count\n---\nBody");
    EXPECT_EQ(SkillCatalog(root).errors().size(), 2);
    EXPECT_TRUE(SkillCatalog(root).metadata().empty());
}
TEST_F(SkillsTest, DisplayMetadataListsSortedScriptPathsWithoutLoadingInstructions) {
    skill();
    EXPECT_EQ(SkillCatalog(root).metadata(true)[0]["scripts"], json::array());
    write(".sos/skills/check-api/scripts/z.py", "SECRET");
    write(".sos/skills/check-api/scripts/nested/a.sh", "SECRET");
    const SkillCatalog catalog(root);
    EXPECT_EQ(catalog.metadata(true)[0]["scripts"], json::array({"scripts/nested/a.sh", "scripts/z.py"}));
    EXPECT_EQ(catalog.metadata(true).dump().find("Read scripts/check.py"), std::string::npos);
    EXPECT_EQ(catalog.prompt().find("scripts/nested/a.sh"), std::string::npos);
}
TEST_F(SkillsTest, DisplayMetadataListsReferencesAndAssetsWithoutReadingTheirContents) {
    skill();
    const auto empty = SkillCatalog(root).metadata(true)[0];
    EXPECT_EQ(empty["references"], json::array());
    EXPECT_EQ(empty["assets"], json::array());
    write(".sos/skills/check-api/references/z.md", "SECRET_REFERENCE");
    write(".sos/skills/check-api/references/nested/a.md", "SECRET_REFERENCE");
    write(".sos/skills/check-api/assets/template.txt", "SECRET_ASSET");
    const SkillCatalog catalog(root);
    const auto display = catalog.metadata(true)[0];
    EXPECT_EQ(display["references"], json::array({"references/nested/a.md", "references/z.md"}));
    EXPECT_EQ(display["assets"], json::array({"assets/template.txt"}));
    EXPECT_EQ(display.dump().find("SECRET"), std::string::npos);
    EXPECT_FALSE(catalog.metadata()[0].contains("references"));
    EXPECT_FALSE(catalog.metadata()[0].contains("assets"));
}
TEST_F(SkillsTest, ResourceListingErrorsRemainSeparateAndDoNotFollowSymlinks) {
    skill();
    write(".sos/skills/check-api/scripts/run.py", "SECRET");
    fs::create_directory_symlink(root, root / ".sos/skills/check-api/references");
    write(".sos/skills/check-api/assets/valid.txt", "SECRET");
    fs::create_symlink(root / ".sos/skills/check-api/SKILL.md", root / ".sos/skills/check-api/assets/link.md");
    const auto display = SkillCatalog(root).metadata(true)[0];
    EXPECT_EQ(display["scripts"], json::array({"scripts/run.py"}));
    EXPECT_TRUE(display.contains("references_error"));
    EXPECT_TRUE(display.contains("assets_error"));
    EXPECT_FALSE(display.contains("references"));
    EXPECT_FALSE(display.contains("assets"));
}
TEST_F(SkillsTest, DoesNotFollowSkillOrScriptSymlinks) {
    skill();
    fs::create_directory_symlink(root / ".sos/skills/check-api", root / ".sos/skills/alias");
    EXPECT_EQ(SkillCatalog(root).errors().size(), 1);
    fs::create_directory_symlink(root, root / ".sos/skills/check-api/scripts");
    const auto metadata = SkillCatalog(root).metadata(true);
    EXPECT_TRUE(metadata[0].contains("scripts_error"));
    EXPECT_FALSE(metadata[0].contains("scripts"));
    EXPECT_THROW(SkillCatalog(root).load({"check-api", "src"}), std::invalid_argument);
}
TEST_F(SkillsTest, InstructionsAreScopedDeduplicatedAndRefreshAfterChanges) {
    write("AGENTS.md", "Root instructions");
    write("src/AGENTS.md", "Source instructions");
    write("src/http/AGENTS.md", "HTTP instructions");
    write("src/http/routes.cpp", "routes");
    write("other/AGENTS.md", "Unrelated instructions");
    auto messages = projectInstructions(root, root, {});
    ASSERT_EQ(messages.size(), 1);
    auto nested = projectInstructions(root, root / "src/http/routes.cpp", messages);
    ASSERT_EQ(nested.size(), 2);
    EXPECT_NE(std::get<TextContent>(nested[0].content[0]).text.find("Source instructions"), std::string::npos);
    EXPECT_NE(std::get<TextContent>(nested[1].content[0]).text.find("HTTP instructions"), std::string::npos);
    messages.insert(messages.end(), nested.begin(), nested.end());
    EXPECT_TRUE(projectInstructions(root, root / "src/http/routes.cpp", messages).empty());
    write("src/AGENTS.md", "Changed instructions");
    EXPECT_EQ(projectInstructions(root, root / "src/http/routes.cpp", messages).size(), 1);
    EXPECT_TRUE(projectInstructions(root, root.parent_path(), messages).empty());
    fs::remove(root / "AGENTS.md");
    const auto removed = projectInstructions(root, root, messages);
    ASSERT_EQ(removed.size(), 1);
    EXPECT_NE(std::get<TextContent>(removed[0].content[0]).text.find("no longer exists"), std::string::npos);
}
TEST_F(SkillsTest, InstructionSymlinksDoNotReadTheirTargets) {
    write("secret", "DO_NOT_READ");
    fs::create_symlink(root / "secret", root / "AGENTS.md");
    const auto messages = projectInstructions(root, root, {});
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(std::get<TextContent>(messages[0].content[0]).text.find("DO_NOT_READ"), std::string::npos);
}
class SkillProvider : public Provider {
public:
    std::vector<json> inputs;
    std::vector<ToolDefinition> definitions;
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    json generate(const json& input, const std::string&, const std::vector<ToolDefinition>& tools, const std::string&) override {
        inputs.push_back(input);
        definitions = tools;
        const auto id = "response_" + std::to_string(inputs.size());
        if (inputs.size() == 1) return {{"id", id}, {"status", "completed"}, {"output", json::array({
            {{"type", "function_call"}, {"id", "call1"}, {"call_id", "call1"}, {"name", "skill"},
             {"arguments", R"({"name":"check-api","input":"src/http"})"}},
            {{"type", "function_call"}, {"id", "call2"}, {"call_id", "call2"}, {"name", "read"},
             {"arguments", R"({"filePath":"src/http/routes.cpp"})"}}
        })}};
        return {{"id", id}, {"status", "completed"}, {"output", json::array({{{"type", "message"}, {"id", id + "_message"}, {"role", "assistant"},
            {"content", json::array({{{"type", "output_text"}, {"text", "Checked"}, {"annotations", json::array()}}})}}})}};
    }
};
TEST_F(SkillsTest, AgentLoadsCatalogThenSkillAndScopedInstructionsAndResumesWithoutDuplicates) {
    skill();
    write("AGENTS.md", "ROOT_RULE");
    write("src/http/AGENTS.md", "HTTP_RULE");
    write("src/http/routes.cpp", "routes");
    auto provider = std::make_unique<SkillProvider>();
    auto* fake = provider.get();
    ToolRegistry tools;
    tools.add(std::make_unique<ReadTool>());
    Agent agent(std::move(provider), "test", std::move(tools), {root, root, [](const auto&) {}});
    agent.enableProjectContext(root);
    agent.append({"system", MessageRole::SYSTEM, {TextContent{"Harness rules"}}});
    agent.append({"user", MessageRole::USER, {TextContent{"Check src/http"}}});
    agent.run().get();
    ASSERT_EQ(fake->inputs.size(), 2);
    EXPECT_NE(fake->inputs[0].dump().find("ROOT_RULE"), std::string::npos);
    EXPECT_EQ(fake->inputs[0].dump().find("HTTP_RULE"), std::string::npos);
    EXPECT_NE(fake->inputs[1].dump().find("HTTP_RULE"), std::string::npos);
    EXPECT_NE(fake->inputs[1].dump().find("Associated scripts"), std::string::npos);
    Conversation restored(ProviderFormat::OPENAI_RESPONSES);
    for (const auto& message : agent.getMessages()) restored.append(message);
    const auto size = restored.getMessages().size();
    agent.replaceConversation(std::move(restored));
    agent.append({"next", MessageRole::USER, {TextContent{"Continue"}}});
    agent.run().get();
    EXPECT_EQ(agent.getMessages().size(), size + 2);
}

TEST_F(SkillsTest, AgentDoesNotImplicitlyLoadWorkspaceOutsideAuthorizedRoot) {
    skill();
    write("AGENTS.md", "ROOT_SECRET");
    fs::create_directory(root / "authorized");
    auto provider = std::make_unique<SkillProvider>();
    auto* fake = provider.get();
    ToolContext context{root, root, [](const auto&) {}};
    context.permissionRoot = root / "authorized";
    Agent agent(std::move(provider), "test", {}, context);
    agent.enableProjectContext(root);
    agent.append({"user", MessageRole::USER, {TextContent{"Hello"}}});
    agent.run().get();
    EXPECT_EQ(fake->inputs[0].dump().find("ROOT_SECRET"), std::string::npos);
    EXPECT_EQ(fake->inputs[0].dump().find("check-api"), std::string::npos);
    EXPECT_NE(fake->inputs[1].dump().find("Unknown skill"), std::string::npos);
}
}
