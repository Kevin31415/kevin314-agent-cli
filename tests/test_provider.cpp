#include <gtest/gtest.h>
#include "provider/model_config.h"
#include "provider/provider_usage.h"

using namespace goose;

TEST(ModelConfigTest, DefaultConfig) {
    ModelConfig mc;
    mc.model_name = "gpt-4o";
    EXPECT_EQ(mc.model_name, "gpt-4o");
}

TEST(ModelConfigTest, WithTemperature) {
    ModelConfig mc;
    mc.model_name = "gpt-4o";
    mc.temperature = 0.7;
    EXPECT_FLOAT_EQ(*mc.temperature, 0.7f);
}

TEST(ProviderUsageTest, ZeroUsage) {
    Usage u = Usage::zero();
    EXPECT_EQ(*u.input_tokens, 0);
    EXPECT_EQ(*u.output_tokens, 0);
}
