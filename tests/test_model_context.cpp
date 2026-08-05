#include <gtest/gtest.h>

#include <cstdlib>

#include "provider/model_context.h"

namespace goose {

TEST(ModelContext, KnownModelMapping) {
    unsetenv("GOOSE_CONTEXT_LIMIT");
    EXPECT_EQ(resolve_context_limit("gpt-oss-120b"), 128000);
    EXPECT_EQ(resolve_context_limit("gpt-4o"), 128000);
    EXPECT_EQ(resolve_context_limit("o3-mini"), 200000);
    EXPECT_EQ(resolve_context_limit("claude-sonnet-4"), 200000);
    EXPECT_EQ(resolve_context_limit("openai/gpt-oss-120b"), 128000);
}

TEST(ModelContext, UnknownModelFallsBackToDefault) {
    unsetenv("GOOSE_CONTEXT_LIMIT");
    EXPECT_EQ(resolve_context_limit("some-custom-model-xyz"), 128000);
}

TEST(ModelContext, EnvOverrideWins) {
    setenv("GOOSE_CONTEXT_LIMIT", "64000", 1);
    EXPECT_EQ(resolve_context_limit("gpt-4o"), 64000);
    unsetenv("GOOSE_CONTEXT_LIMIT");
}

} // namespace goose
