#ifndef SOS_CONVERSATION_HPP
#define SOS_CONVERSATION_HPP

#include <memory>
#include <vector>

#include "messages/message.hpp"
#include "provider_format.hpp"
#include "transformers/message_provider_transformer.hpp"

class Conversation {
 public:
  explicit Conversation(ProviderFormat provider);

  void append(const Message& message);
  void append(const nlohmann::json& providerResponse);
  // Display metadata does not participate in the cached provider input.
   void setLastGenerationMetadata(GenerationMetadata metadata);
    void setLastElapsedMs(std::uint64_t elapsedMs);
    void setLastModelCall(ModelCallMetadata metadata);
   // Rebuild only the wire context; keep the original messages for history/replay UI.
   void rebuildProviderContext(std::size_t portableBefore);

  ProviderFormat getProvider() const;
  const std::vector<Message>& getMessages() const;
  const nlohmann::json& getProviderFormat() const;

 private:
  ProviderFormat provider;
  std::vector<Message> messages;
  nlohmann::json providerFormat = nlohmann::json::array();
  std::unique_ptr<MessageProviderTransformer> messageTransformer;
};

#endif  // SOS_CONVERSATION_HPP
