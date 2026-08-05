#include "session/session_manager.h"
#include "config/paths.h"
#include "utils/format.h"
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <mutex>

namespace goose {

static sqlite3* g_db = nullptr;
static std::mutex g_db_mutex;

static std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? text : "";
}

static int exec_sql(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(g_db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string error = err ? err : "unknown error";
        sqlite3_free(err);
        spdlog::error("SQLite error: {}", error);
        return rc;
    }
    return SQLITE_OK;
}

SessionManager& SessionManager::instance() {
    static SessionManager manager;
    return manager;
}

void SessionManager::ensure_initialized() {
    if (initialized_) return;

    std::filesystem::create_directories(paths::session_dir());

    auto db_path = paths::session_dir() / "kacli.db";
    int rc = sqlite3_open(db_path.string().c_str(), &g_db);
    if (rc != SQLITE_OK) {
        spdlog::error("Failed to open database: {}", db_path.string());
        if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
        return;
    }

    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA foreign_keys=ON;");

    utils::set_file_permissions_private(db_path.string());

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            provider_name TEXT DEFAULT '',
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            input_tokens INTEGER DEFAULT 0,
            output_tokens INTEGER DEFAULT 0
        );
    )");

    // 兼容旧库：列不存在时补加。
    {
        bool has_input = false, has_output = false;
        const char* info_sql = "PRAGMA table_info(sessions);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(g_db, info_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string col = safe_column_text(stmt, 1);
                if (col == "input_tokens") has_input = true;
                if (col == "output_tokens") has_output = true;
            }
            sqlite3_finalize(stmt);
        }
        if (!has_input) exec_sql("ALTER TABLE sessions ADD COLUMN input_tokens INTEGER DEFAULT 0;");
        if (!has_output) exec_sql("ALTER TABLE sessions ADD COLUMN output_tokens INTEGER DEFAULT 0;");
    }

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id TEXT NOT NULL,
            role TEXT NOT NULL,
            content TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
    )");

    exec_sql("CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id);");

    initialized_ = true;
    spdlog::info("SessionManager initialized, db: {}", db_path.string());
}

Result<Session> SessionManager::get_session(const std::string& id, bool load_conversation) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<Session>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    const char* sql = "SELECT id, name, provider_name, created_at, updated_at, input_tokens, output_tokens FROM sessions WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<Session>::err(make_error(ErrorCode::SessionError, "SQL 准备失败"));
    }

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);

    Session session;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        session.id = safe_column_text(stmt, 0);
        session.name = safe_column_text(stmt, 1);
        std::string provider = safe_column_text(stmt, 2);
        if (!provider.empty()) session.provider_name = provider;
        session.created_at = sqlite3_column_int64(stmt, 3);
        session.updated_at = sqlite3_column_int64(stmt, 4);
        session.input_tokens = sqlite3_column_int64(stmt, 5);
        session.output_tokens = sqlite3_column_int64(stmt, 6);
    } else {
        sqlite3_finalize(stmt);
        return Result<Session>::err(make_error(ErrorCode::SessionError, "会话未找到: " + id));
    }
    sqlite3_finalize(stmt);

    if (load_conversation) {
        const char* msg_sql = "SELECT role, content FROM messages WHERE session_id = ? ORDER BY id;";
        sqlite3_stmt* msg_stmt = nullptr;
        rc = sqlite3_prepare_v2(g_db, msg_sql, -1, &msg_stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(msg_stmt, 1, id.c_str(), -1, SQLITE_STATIC);

            Conversation conv;
            while (sqlite3_step(msg_stmt) == SQLITE_ROW) {
                std::string role_str = safe_column_text(msg_stmt, 0);
                std::string content_json = safe_column_text(msg_stmt, 1);

                try {
                    Message msg;
                    msg.role = role_from_string(role_str);
                    auto j = nlohmann::json::parse(content_json);
                    from_json(j, msg);
                    conv.push(std::move(msg));
                } catch (const std::exception& e) {
                    spdlog::warn("Corrupt message in session {}: {}", id, e.what());
                }
            }
            sqlite3_finalize(msg_stmt);
            session.conversation = std::move(conv);
        }
    }

    return Result<Session>::ok(std::move(session));
}

Result<Session> SessionManager::create_session(
    const std::filesystem::path& /*cwd*/,
    const std::string& name) {

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<Session>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto id = utils::generate_uuid();

    const char* sql = "INSERT INTO sessions (id, name, created_at, updated_at) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<Session>::err(make_error(ErrorCode::SessionError, "SQL 准备失败"));
    }

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_int64(stmt, 4, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Result<Session>::err(make_error(ErrorCode::SessionError, "插入失败"));
    }

    Session session;
    session.id = id;
    session.name = name;
    session.conversation = Conversation{};
    session.created_at = now;
    session.updated_at = now;

    spdlog::info("Created session: {}", id);
    return Result<Session>::ok(std::move(session));
}

Result<void> SessionManager::add_message(const std::string& session_id, const Message& msg) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    nlohmann::json content_json;
    to_json(content_json, msg);
    std::string content_str = content_json.dump();

    const char* sql = "INSERT INTO messages (session_id, role, content, created_at) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "SQL 准备失败"));
    }

    std::string role_str = to_string(msg.role);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, role_str.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content_str.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, msg.created_at);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "插入消息失败"));
    }

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const char* update_sql = "UPDATE sessions SET updated_at = ? WHERE id = ?;";
    rc = sqlite3_prepare_v2(g_db, update_sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return Result<void>::ok();
}

Result<void> SessionManager::add_usage(const std::string& session_id,
                                       int64_t input_tokens, int64_t output_tokens) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }
    if (input_tokens <= 0 && output_tokens <= 0) return Result<void>::ok();

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE sessions SET input_tokens = input_tokens + ?, output_tokens = output_tokens + ?, "
        "updated_at = ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, input_tokens);
        sqlite3_bind_int64(stmt, 2, output_tokens);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_STATIC);
        int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            spdlog::error("Failed to add usage: {}", sqlite3_errmsg(g_db));
        }
        sqlite3_finalize(stmt);
    }

    return Result<void>::ok();
}

Result<std::vector<Session>> SessionManager::list_sessions() {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<std::vector<Session>>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    const char* sql = "SELECT id, name, provider_name, created_at, updated_at, input_tokens, output_tokens FROM sessions ORDER BY updated_at DESC;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<std::vector<Session>>::err(make_error(ErrorCode::SessionError, "SQL 准备失败"));
    }

    std::vector<Session> sessions;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Session s;
        s.id = safe_column_text(stmt, 0);
        s.name = safe_column_text(stmt, 1);
        std::string provider = safe_column_text(stmt, 2);
        if (!provider.empty()) s.provider_name = provider;
        s.created_at = sqlite3_column_int64(stmt, 3);
        s.updated_at = sqlite3_column_int64(stmt, 4);
        s.input_tokens = sqlite3_column_int64(stmt, 5);
        s.output_tokens = sqlite3_column_int64(stmt, 6);
        sessions.push_back(std::move(s));
    }
    sqlite3_finalize(stmt);

    return Result<std::vector<Session>>::ok(std::move(sessions));
}

Result<void> SessionManager::delete_session(const std::string& session_id_or_name) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    std::string actual_id = session_id_or_name;

    const char* lookup = "SELECT id FROM sessions WHERE id = ? OR name = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, lookup, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, session_id_or_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, session_id_or_name.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            actual_id = safe_column_text(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    const char* del_msgs = "DELETE FROM messages WHERE session_id = ?;";
    rc = sqlite3_prepare_v2(g_db, del_msgs, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, actual_id.c_str(), -1, SQLITE_STATIC);
        int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return Result<void>::err(make_error(ErrorCode::SessionError, "删除消息失败"));
        }
        sqlite3_finalize(stmt);
    }

    const char* del_session = "DELETE FROM sessions WHERE id = ?;";
    rc = sqlite3_prepare_v2(g_db, del_session, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, actual_id.c_str(), -1, SQLITE_STATIC);
        int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return Result<void>::err(make_error(ErrorCode::SessionError, "删除会话失败"));
        }
        if (sqlite3_changes(g_db) == 0) {
            sqlite3_finalize(stmt);
            return Result<void>::err(make_error(ErrorCode::SessionError, "会话未找到: " + session_id_or_name));
        }
        sqlite3_finalize(stmt);
    }

    spdlog::info("Deleted session: {}", actual_id);
    return Result<void>::ok();
}

Result<void> SessionManager::update_session(const std::string& session_id,
                                             const std::optional<std::string>& provider_name) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    if (!provider_name) return Result<void>::ok();

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "UPDATE sessions SET provider_name = ?, updated_at = ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, provider_name->c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_STATIC);
        int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            spdlog::error("Failed to update session: {}", sqlite3_errmsg(g_db));
        }
        sqlite3_finalize(stmt);
    }

    return Result<void>::ok();
}

Result<void> SessionManager::clear_messages(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM messages WHERE session_id = ?;";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
        int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            spdlog::error("Failed to clear messages: {}", sqlite3_errmsg(g_db));
        }
        sqlite3_finalize(stmt);
    }

    return Result<void>::ok();
}

Result<void> SessionManager::replace_messages(const std::string& session_id,
                                              const std::vector<Message>& messages) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    const char* delete_sql = "DELETE FROM messages WHERE session_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, delete_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "SQL 准备失败"));
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step_rc != SQLITE_DONE) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "删除旧消息失败"));
    }

    for (const auto& msg : messages) {
        nlohmann::json content_json;
        to_json(content_json, msg);
        std::string content_str = content_json.dump();

        const char* insert_sql =
            "INSERT INTO messages (session_id, role, content, created_at) VALUES (?, ?, ?, ?);";
        rc = sqlite3_prepare_v2(g_db, insert_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return Result<void>::err(make_error(ErrorCode::SessionError, "SQL 准备失败"));
        }
        std::string role_str = to_string(msg.role);
        sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, role_str.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, content_str.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, msg.created_at);
        step_rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (step_rc != SQLITE_DONE) {
            return Result<void>::err(make_error(ErrorCode::SessionError, "插入消息失败"));
        }
    }

    return Result<void>::ok();
}

Result<void> SessionManager::compact_messages(const std::string& session_id, size_t keep_last) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    ensure_initialized();
    if (!g_db) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "数据库未初始化"));
    }

    sqlite3_stmt* stmt = nullptr;
    const char* count_sql = "SELECT COUNT(*) FROM messages WHERE session_id = ?;";
    int rc = sqlite3_prepare_v2(g_db, count_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<void>::err(make_error(ErrorCode::SessionError, "prepare count failed"));
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);

    size_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (total <= keep_last) return Result<void>::ok();

    size_t delete_count = total - keep_last;
    const char* delete_sql =
        "DELETE FROM messages WHERE id IN ("
        "  SELECT id FROM messages WHERE session_id = ? ORDER BY id ASC LIMIT ?"
        ");";
    rc = sqlite3_prepare_v2(g_db, delete_sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(delete_count));
        int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            spdlog::error("Failed to compact messages: {}", sqlite3_errmsg(g_db));
        }
        sqlite3_finalize(stmt);
    }

    return Result<void>::ok();
}

} // namespace goose
