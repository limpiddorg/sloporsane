#ifndef SOS_QUESTION_STATE_HPP
#define SOS_QUESTION_STATE_HPP

#include <agent-runtime/core/cancellation.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct QuestionEvent {
    std::string id, question;
    std::vector<std::string> options;
};

// The worker waits without blocking Asio. A reply is correlated to this
// integration's one pending question; dismissal or disconnect releases it.
class QuestionState {
public:
    std::string ask(const std::string& question, const std::vector<std::string>& options,
        const std::function<void()>& check, const std::function<void(QuestionEvent)>& notify) {
        std::unique_lock lock(mutex);
        if (closed || pending) throw std::logic_error("A question is already pending");
        pending = QuestionEvent{std::to_string(++sequence), question, options};
        answer.reset();
        try {
            auto event = *pending;
            lock.unlock();
            notify(std::move(event));
            lock.lock();
            while (!answer && !closed) {
                lock.unlock();
                check();
                lock.lock();
                changed.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (closed || !*answer) throw core::Cancelled();
            auto result = options.at(**answer);
            pending.reset();
            answer.reset();
            return result;
        } catch (...) {
            if (!lock.owns_lock()) lock.lock();
            pending.reset();
            answer.reset();
            throw;
        }
    }
    void reply(const std::string& id, std::optional<std::size_t> choice) {
        std::lock_guard lock(mutex);
        if (!pending || pending->id != id || answer || (choice && *choice >= pending->options.size()))
            throw std::invalid_argument("Question is no longer pending or choice is invalid");
        answer = choice;
        changed.notify_all();
    }
    void close() {
        std::lock_guard lock(mutex);
        closed = true;
        changed.notify_all();
    }
private:
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<QuestionEvent> pending;
    std::optional<std::optional<std::size_t>> answer;
    std::size_t sequence = 0;
    bool closed = false;
};

#endif // SOS_QUESTION_STATE_HPP
