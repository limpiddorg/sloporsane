#include "integration.hpp"
#include "../session/conversation_recap.hpp"
#include "../transport/await_future.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>

asio::awaitable<nlohmann::json> Integration::recapSession(const IntegrationRequest& request) {
    requireIdleSession();
    if (!session) throw std::logic_error("Start a conversation before requesting a recap");
    if (!providers && !summaryProvider) throw std::runtime_error("Recap provider is unavailable");
    applyCurrentSelection();
    const auto info = session->getInfo();
    const auto cancel = summaryCancellation;
    const auto cancelledReply = [&] {
        return nlohmann::json{{"type", "recap.cancelled"}, {"requestId", request.requestId}, {"sessionId", info.id}};
    };
    co_await sendReply({{"type", "recap.started"}, {"requestId", request.requestId}, {"sessionId", info.id}});
    if (!co_await session->prepareAgent([cancel] { return cancel->load(); })) {
        summaryCancellation.reset();
        co_return cancelledReply();
    }
    auto conversation = session->activeConversation();
    const auto id = "recap_" + info.id + "_" + std::to_string(session->messageCount() + 1);
    auto job = std::async(std::launch::async,
        [conversation = std::move(conversation), info, id, cancel, factory = summaryProvider, connections = providers] {
            core::CancellationCheck check = [cancel] { return cancel->load(); };
            core::checkCancellation(check);
            auto provider = factory ? factory(info) : connections->create(info.provider);
            if (!provider) throw std::runtime_error("Recap requires a provider");
            provider->setCancellationCheck(check);
            return generateConversationRecap(conversation, *provider, info, id);
        });
    co_await awaitFuture(job);
    std::optional<Message> result;
    std::exception_ptr failure;
    try { result = job.get(); } catch (...) { failure = std::current_exception(); }
    summaryCancellation.reset();
    if (closing || cancel->load()) co_return cancelledReply();
    if (failure) std::rethrow_exception(failure);
    session->saveRecap(*result);
    co_return nlohmann::json{{"type", "recap.completed"}, {"requestId", request.requestId}, {"sessionId", info.id},
        {"messages", std::vector<Message>{*result}}};
}
