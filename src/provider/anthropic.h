#pragma once

#include "base.h"
#include "http_client.h"

namespace goose {

class AnthropicProvider : public Provider {
public:
    // base_url 为空时依次回退 ANTHROPIC_BASE_URL 环境变量与官方默认地址
    AnthropicProvider(std::string name = "anthropic", std::string base_url = "");

    const std::string& get_name() const override { return name_; }

    Result<MessageStream>
    stream(const ModelConfig& model_config,
           const std::string& system_prompt,
           const std::vector<Message>& messages,
           const std::vector<Tool>& tools) override;

private:
    std::string name_ = "anthropic";
    std::string base_url_ = "https://api.anthropic.com";
    HttpClient http_;

    nlohmann::json build_request(const ModelConfig& model_config,
                                  const std::string& system_prompt,
                                  const std::vector<Message>& messages,
                                  const std::vector<Tool>& tools);
};

} // namespace goose
