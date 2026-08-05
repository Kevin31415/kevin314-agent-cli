#pragma once

#include "base.h"
#include "http_client.h"

namespace goose {

class OpenAiProvider : public Provider {
public:
    // name 用于标识自定义提供商（如 deepseek），base_url 为空时依次回退
    // OPENAI_BASE_URL 环境变量与官方默认地址
    OpenAiProvider(std::string name = "openai", std::string base_url = "");

    const std::string& get_name() const override { return name_; }

    Result<MessageStream>
    stream(const ModelConfig& model_config,
           const std::string& system_prompt,
           const std::vector<Message>& messages,
           const std::vector<Tool>& tools) override;

private:
    std::string name_ = "openai";
    std::string base_url_ = "https://api.openai.com/v1/chat/completions";
    HttpClient http_;

    nlohmann::json build_request(const ModelConfig& model_config,
                                  const std::string& system_prompt,
                                  const std::vector<Message>& messages,
                                  const std::vector<Tool>& tools);
};

} // namespace goose
