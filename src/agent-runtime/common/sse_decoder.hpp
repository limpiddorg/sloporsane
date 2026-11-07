#ifndef SOS_SSE_DECODER_HPP
#define SOS_SSE_DECODER_HPP

#include <functional>
#include <string_view>

// Decode SSE framing only. The consumer handles JSON and protocol-specific events.
// Each data view is valid only for the duration of the callback.
void forEachSseData(std::string_view stream, const std::function<void(std::string_view)>& consume);

#endif // SOS_SSE_DECODER_HPP
