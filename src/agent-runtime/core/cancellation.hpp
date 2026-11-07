#ifndef SOS_CANCELLATION_HPP
#define SOS_CANCELLATION_HPP

#include <functional>
#include <stdexcept>

namespace core {
using CancellationCheck = std::function<bool()>;

class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("Execution cancelled") {}
};

inline void checkCancellation(const CancellationCheck& check) {
    if (check && check()) throw Cancelled();
}
}

#endif // SOS_CANCELLATION_HPP
