#ifndef SOS_AWAIT_FUTURE_HPP
#define SOS_AWAIT_FUTURE_HPP

#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <future>

// Keep the owner and worker alive until completion. The caller consumes the result.
template<class Future>
asio::awaitable<void> awaitFuture(Future& future) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        timer.expires_after(std::chrono::milliseconds(5));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

#endif // SOS_AWAIT_FUTURE_HPP
