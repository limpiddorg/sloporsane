#ifndef SOS_ASYNC_QUEUE_HPP
#define SOS_ASYNC_QUEUE_HPP

#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <deque>

// Single consumer; push/receive are called only on the io_context thread.
template <typename T>
class AsyncQueue {
public:
    explicit AsyncQueue(asio::any_io_executor executor) : ready(executor) {}

    void push(T value) {
        values.push_back(std::move(value));
        ready.cancel_one();
    }

    void pushFront(T value) {
        values.push_front(std::move(value));
        ready.cancel_one();
    }

    asio::awaitable<T> receive() {
        while (values.empty()) {
            ready.expires_at(asio::steady_timer::time_point::max());
            asio::error_code error;
            co_await ready.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (error && error != asio::error::operation_aborted) throw asio::system_error(error);
        }
        auto value = std::move(values.front());
        values.pop_front();
        co_return value;
    }

private:
    std::deque<T> values;
    asio::steady_timer ready;
};

#endif // SOS_ASYNC_QUEUE_HPP
