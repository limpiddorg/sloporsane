#include <agent-runtime/providers/image_support.hpp>
#include <agent-runtime/providers/model_info.hpp>
#include <gtest/gtest.h>

TEST(ImageSupportTest, RequiresAnExplicitValidInputDeclaration) {
    using nlohmann::json;
    EXPECT_EQ(declaredImageSupport(json::array({"text", "image"})), ImageSupport::SUPPORTED);
    EXPECT_EQ(declaredImageSupport(json::array({"text"})), ImageSupport::UNSUPPORTED);
    EXPECT_EQ(declaredImageSupport(json::array()), ImageSupport::UNSUPPORTED);
    EXPECT_EQ(declaredImageSupport(nullptr), ImageSupport::UNKNOWN);
    EXPECT_EQ(declaredImageSupport("image"), ImageSupport::UNKNOWN);
    EXPECT_EQ(declaredImageSupport(json::array({"image", 42})), ImageSupport::UNKNOWN);
    EXPECT_EQ(ModelInfo{}.imageInput, ImageSupport::UNKNOWN);
}

TEST(ImageSupportTest, UnknownAndUnsupportedAreBothRefused) {
    EXPECT_NO_THROW(requireImageSupport(ImageSupport::SUPPORTED));
    EXPECT_THROW(requireImageSupport(ImageSupport::UNSUPPORTED), ImageInputError);
    EXPECT_THROW(requireImageSupport(ImageSupport::UNKNOWN), ImageInputError);
    EXPECT_EQ(std::string(imageSupportName(ImageSupport::SUPPORTED)), "supported");
    EXPECT_EQ(std::string(imageSupportName(ImageSupport::UNSUPPORTED)), "unsupported");
    EXPECT_EQ(std::string(imageSupportName(ImageSupport::UNKNOWN)), "unknown");
}

TEST(ImageSupportTest, OpenAIReferenceDoesNotGuessFamiliesOrFutureSnapshots) {
    EXPECT_EQ(openAIImageSupport("gpt-4.1"), ImageSupport::SUPPORTED);
    EXPECT_EQ(openAIImageSupport("gpt-4.1-2025-04-14"), ImageSupport::SUPPORTED);
    EXPECT_EQ(openAIImageSupport("gpt-5.4"), ImageSupport::SUPPORTED);
    EXPECT_EQ(openAIImageSupport("o3-mini"), ImageSupport::UNSUPPORTED);
    EXPECT_EQ(openAIImageSupport("o3-mini-2025-01-31"), ImageSupport::UNSUPPORTED);
    EXPECT_EQ(openAIImageSupport("gpt-4.1-2099-01-01"), ImageSupport::UNKNOWN);
    EXPECT_EQ(openAIImageSupport("gpt-5.4-codex"), ImageSupport::UNKNOWN);
    EXPECT_EQ(openAIImageSupport("vendor/gpt-4.1"), ImageSupport::UNKNOWN);
}
