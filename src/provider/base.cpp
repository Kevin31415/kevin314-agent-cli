#include "provider/base.h"

namespace goose {

Result<Message> Provider::complete(
    const ModelConfig& model_config,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const std::vector<Tool>& tools) {

    auto stream_result = stream(model_config, system_prompt, messages, tools);
    if (!stream_result) {
        return Result<Message>::err(stream_result.error());
    }

    auto stream_fn = *stream_result;
    Message final_message;

    while (true) {
        auto chunk_result = stream_fn();
        if (!chunk_result) {
            if (chunk_result.error().code == ErrorCode::StreamEnd) break;
            return Result<Message>::err(chunk_result.error());
        }
        if (chunk_result->message) {
            final_message = *chunk_result->message;
        }
    }

    if (final_message.id.empty()) {
        final_message.with_generated_id_if_missing();
    }
    return Result<Message>::ok(std::move(final_message));
}

} // namespace goose
