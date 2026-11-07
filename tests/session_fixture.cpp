// Seed integration-test sessions through the production journal and indexes.
#include <daemon/session/session_journal.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("Usage: session-fixture DIRECTORY SEED.json");
        std::ifstream input(argv[2]);
        const auto seed = nlohmann::json::parse(input);
        SessionInfo info;
        info.id = seed.at("id");
        info.title = seed.at("title");
        info.workspace = seed.at("workspace").get<std::string>();
        info.model = "gpt-4.1";
        info.provider = "openai";
        info.createdAt = info.updatedAt = 1;
        SessionJournal(argv[1]).save(seed.at("messages").get<std::vector<Message>>(), info);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
