#include <agent-runtime/image.hpp>
#include <agent-runtime/agent.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <agent-runtime/conversation/transformers/openai/openai_responses_api_message_provider_transformer.hpp>
#include <agent-runtime/tools/common/view_image.hpp>
#include <gtest/gtest.h>

namespace {
Image pixel() {
    return validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"});
}
class ImageProvider : public Provider {
public:
    ImageSupport support = ImageSupport::UNKNOWN;
    int calls = 0;
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    ModelInfo getModelInfo(const std::string& id) override { return {id, {}, {}, {}, {}, support}; }
    nlohmann::json generate(const nlohmann::json& input, const std::string&, const std::vector<ToolDefinition>&, const std::string&) override {
        ++calls;
        EXPECT_NE(input.dump().find("input_image"), std::string::npos);
        return {{"id", "reply"}, {"output", nlohmann::json::array()}, {"status", "completed"}};
    }
};
}
TEST(ImageTest, ValidatesContentAndDerivesStableIdentity) {
    const auto image = pixel();
    EXPECT_EQ(image.width, 1);
    EXPECT_EQ(image.height, 1);
    EXPECT_EQ(image.id.size(), 68);
    auto renamed = image;
    renamed.name = "renamed.png";
    EXPECT_EQ(validateImage(renamed).id, image.id);
    renamed.mime = "image/jpeg";
    EXPECT_THROW(validateImage(renamed), std::invalid_argument);
    renamed = image; renamed.data += "bad";
    EXPECT_THROW(validateImage(renamed), std::invalid_argument);
    std::vector<Image> tooMany(9, image);
    EXPECT_THROW(validateImages(tooMany), std::invalid_argument);
}
TEST(ImageTest, SerializesReferencesAndSendsPixelsAsMultimodalInput) {
    Message message{"user", MessageRole::USER, {TextContent{"What is this?"}}};
    message.images = {pixel()};
    const auto roundtrip = nlohmann::json(message).get<Message>();
    EXPECT_EQ(roundtrip.images.front().data, pixel().data);
    OpenAiResponsesApiMessageProviderTransformer transformer;
    auto items = transformer.toProviderFormat(roundtrip);
    EXPECT_EQ(items.back()["content"][1]["type"], "input_image");
    EXPECT_EQ(items.back()["content"][1]["image_url"], pixel().url());
    message.images.front().data.clear();
    EXPECT_FALSE(nlohmann::json(message)["images"][0].contains("data"));
    EXPECT_THROW(transformer.toProviderFormat(message), std::invalid_argument);
}
TEST(ImageTest, UnknownAndUnsupportedNeverGenerate) {
    for (auto support : {ImageSupport::UNKNOWN, ImageSupport::UNSUPPORTED, ImageSupport::SUPPORTED}) {
        auto provider = std::make_unique<ImageProvider>();
        auto* observed = provider.get();
        observed->support = support;
        Agent agent(std::move(provider), "model");
        Message message{"user", MessageRole::USER, {}};
        message.images = {pixel()};
        agent.append(message);
        if (support == ImageSupport::SUPPORTED) EXPECT_NO_THROW(agent.run().get());
        else EXPECT_THROW(agent.run().get(), ImageInputError);
        EXPECT_EQ(observed->calls, support == ImageSupport::SUPPORTED ? 1 : 0);
    }
}
TEST(ImageTest, ViewImageUsesOnlySessionReaderAndReturnsPixels) {
    ToolContext context;
    context.readImage = [](const std::string& id) {
        if (id != pixel().id) throw std::invalid_argument("Not in this session");
        return pixel();
    };
    ViewImageTool tool;
    auto result = tool.execute({{"image_id", pixel().id}}, context);
    ASSERT_EQ(result.images.size(), 1);
    EXPECT_EQ(result.images.front().data, pixel().data);
    EXPECT_EQ(result.output.find(pixel().data), std::string::npos);
    EXPECT_THROW(tool.execute({{"image_id", "../other/session"}}, context), std::invalid_argument);
}
