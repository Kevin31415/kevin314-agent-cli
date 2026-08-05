#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <memory>

#include "../core/types.h"
#include "../provider/model_config.h"

namespace goose {

struct Session {
    std::string id;
    std::string name;
    std::optional<Conversation> conversation;
    std::optional<ModelConfig> model_config;
    std::string provider_name;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
};

class SessionManager {
public:
    static SessionManager& instance();

    Result<Session> get_session(const std::string& id, bool load_conversation = false);

    Result<Session> create_session(const std::filesystem::path& cwd,
                                    const std::string& name);

    Result<void> add_message(const std::string& session_id, const Message& msg);

    Result<void> update_session(const std::string& session_id,
                                 const std::optional<std::string>& provider_name);

    // 累加会话累计 token 用量（跨进程持久化）。
    Result<void> add_usage(const std::string& session_id,
                           int64_t input_tokens, int64_t output_tokens);

    Result<std::vector<Session>> list_sessions();

    Result<void> delete_session(const std::string& session_id);

    Result<void> clear_messages(const std::string& session_id);

    Result<void> replace_messages(const std::string& session_id,
                                  const std::vector<Message>& messages);

    Result<void> compact_messages(const std::string& session_id, size_t keep_last);

private:
    SessionManager() = default;
    bool initialized_ = false;
    void ensure_initialized();
};

} // namespace goose
