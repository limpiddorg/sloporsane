#ifndef SOS_SKILL_TEMPLATE_HPP
#define SOS_SKILL_TEMPLATE_HPP

#include <string>
#include <vector>

struct SkillDefinition;
struct SkillInvocation;

namespace skill_templates {
std::vector<std::string> splitArguments(const std::string& input);
std::string expand(const SkillDefinition& skill, const SkillInvocation& invocation);
}

#endif
