#include "responses_stream_assembler.hpp"
#include "generation_retry.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
void require(bool condition, const char* message) {
    if (!condition) throw RetryableGenerationError(message);
}
}

void ResponsesStreamAssembler::consume(const Json& event) {
    try {
        require(state == State::READING, "OpenAI stream received an event after termination");
        require(event.is_object() && event.contains("type") && event.at("type").is_string(), "Invalid OpenAI stream event");
        const auto type = event.at("type").get<std::string>();
        if (type == "error" || type == "response.failed" || type == "response.incomplete" || type == "response.cancelled") {
            const auto response = event.find("response");
            const auto& source = response != event.end() && response->is_object() ? *response : event;
            const auto error = source.find("error");
            const auto details = source.find("incomplete_details");
            const auto& failure = error != source.end() && !error->is_null()
                ? *error : details != source.end() ? *details : source;
            if (retryableApiError(failure))
                throw RetryableGenerationError("OpenAI " + describeApiError(failure), true);
            throw std::runtime_error("OpenAI " + describeApiError(failure));
        }
        if (event.contains("output_index")) expectedIndices.insert(outputIndex(event));
        if (type == "response.output_item.done") recordItem(event);
        else if (type == "response.completed") recordCompletion(event);
    } catch (...) {
        state = State::FAILED;
        throw;
    }
}

std::size_t ResponsesStreamAssembler::outputIndex(const Json& event) {
    require(event.contains("output_index") && event.at("output_index").is_number_integer(), "Invalid OpenAI output index");
    const auto& index = event.at("output_index");
    require(index.is_number_unsigned() || index.get<std::int64_t>() >= 0, "Invalid OpenAI output index");
    const auto value = index.get<std::uint64_t>();
    require(value <= std::numeric_limits<std::size_t>::max(), "Invalid OpenAI output index");
    return static_cast<std::size_t>(value);
}

std::string ResponsesStreamAssembler::itemId(const Json& item) {
    const auto id = item.find("id");
    if (id == item.end()) return {};
    require(id->is_string(), "Invalid OpenAI output item ID");
    return id->get<std::string>();
}

void ResponsesStreamAssembler::validateItem(const Json& item) {
    require(item.is_object() && item.contains("type") && item.at("type").is_string(), "Invalid OpenAI output item");
    if (item.contains("status") && !item.at("status").is_null())
        require(item.at("status") == "completed", "OpenAI output item is incomplete");
    itemId(item);
}

void ResponsesStreamAssembler::recordItem(const Json& event) {
    require(event.contains("item"), "OpenAI output event is missing its item");
    const auto index = outputIndex(event);
    const auto& item = event.at("item");
    validateItem(item);
    if (const auto existing = completedItems.find(index); existing != completedItems.end()) {
        require(existing->second == item, "Conflicting OpenAI output items at the same index");
        return; // Repeated identical completion events must not duplicate content.
    }
    const auto id = itemId(item);
    if (!id.empty())
        for (const auto& [position, previous] : completedItems)
            require(itemId(previous) != id, "Duplicate OpenAI output item ID at different indices");
    completedItems.emplace(index, item);
}

void ResponsesStreamAssembler::recordCompletion(const Json& event) {
    const auto response = event.find("response");
    require(response != event.end() && response->is_object(), "Missing completed OpenAI response");
    require(!response->contains("status") || response->at("status") == "completed", "OpenAI response is incomplete");
    require(!response->contains("error") || response->at("error").is_null(), "OpenAI generation failed");
    require(response->contains("output") && response->at("output").is_array(), "Invalid completed OpenAI output");
    finalResponse = *response;
    state = State::COMPLETED;
}

void ResponsesStreamAssembler::mergeFinalItems(Items& items, const Json& finalOutput) {
    std::set<std::string> finalIds;
    for (std::size_t index = 0; index < finalOutput.size(); ++index) {
        const auto& item = finalOutput[index];
        validateItem(item);
        const auto id = itemId(item);
        if (!id.empty()) require(finalIds.insert(id).second, "Duplicate OpenAI item in completed output");
        const auto known = id.empty() ? items.end() : std::find_if(items.begin(), items.end(),
            [&](const auto& entry) { return itemId(entry.second) == id; });
        // A partial final output can repeat only some items. Their stream indices
        // remain authoritative; the compact final array must not reorder them.
        const auto position = known == items.end() ? index : known->first;
        const auto existing = items.find(position);
        if (existing == items.end()) { items.emplace(position, item); continue; }
        const auto previousId = itemId(existing->second);
        require(previousId.empty() || id.empty() || previousId == id, "Conflicting OpenAI final output order");
        require(existing->second.at("type") == item.at("type"), "Conflicting OpenAI output item types");
        existing->second.update(item); // Final fields win; retain metadata omitted from the final copy.
    }
}

Json ResponsesStreamAssembler::assembleOutput() const {
    auto items = completedItems;
    mergeFinalItems(items, finalResponse.at("output"));
    for (const auto index : expectedIndices)
        require(items.contains(index), "OpenAI stream is missing a completed output item");
    auto output = Json::array();
    for (const auto& [index, item] : items) {
        require(index == output.size(), "OpenAI stream has gaps in its output indices");
        output.push_back(item);
    }
    return output;
}

Json ResponsesStreamAssembler::finish() const {
    require(state == State::COMPLETED, "OpenAI stream ended without a successful completed response");
    auto result = finalResponse;
    result["output"] = assembleOutput();
    return result;
}
