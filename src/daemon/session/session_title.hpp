#ifndef SOS_SESSION_TITLE_HPP
#define SOS_SESSION_TITLE_HPP

#include <agent-runtime/providers/provider.hpp>
#include <functional>

using SessionTitleGenerator = std::function<std::string(const std::string& model, const std::string& firstMessage,
    core::CancellationCheck cancelled)>;

// One isolated provider call, without tools or changes to the discussion history.
std::string generateSessionTitle(Provider& provider, const std::string& model, const std::string& firstMessage);

#endif // SOS_SESSION_TITLE_HPP
