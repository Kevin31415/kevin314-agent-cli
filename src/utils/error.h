#pragma once

#include <string>
#include <variant>
#include <optional>
#include <stdexcept>

namespace goose {

enum class ErrorCode {
    Ok = 0,
    ProviderError,
    ExtensionError,
    ConfigError,
    SessionError,
    McpError,
    IoError,
    SerializeError,
    NetworkError,
    AuthError,
    RateLimitError,
    ContextLengthError,
    CreditsExhaustedError,
    ServerError,
    StreamEnd,
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    bool ok() const { return code == ErrorCode::Ok; }
    explicit operator bool() const { return ok(); }
};

inline Error make_error(ErrorCode code, const std::string& msg) {
    return Error{code, msg};
}

// ========== Result<T> ==========
template<typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)), has_value_(true) {}
    Result(Error error) : error_(std::move(error)), has_value_(false) {}

    Result(Result&& other) noexcept
        : data_(std::move(other.data_))
        , error_(std::move(other.error_))
        , has_value_(other.has_value_) {
        other.has_value_ = false;
    }
    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            data_ = std::move(other.data_);
            error_ = std::move(other.error_);
            has_value_ = other.has_value_;
            other.has_value_ = false;
        }
        return *this;
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    static Result ok(T value) { return Result(std::move(value)); }
    static Result err(Error error) { return Result(std::move(error)); }

    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value_; }

    T& value() {
        if (!has_value_) throw std::runtime_error("Result::value() called on error: " + error_.message);
        return *data_;
    }
    const T& value() const {
        if (!has_value_) throw std::runtime_error("Result::value() called on error: " + error_.message);
        return *data_;
    }
    Error& error() { return error_; }
    const Error& error() const { return error_; }
    T& operator*() {
        if (!has_value_) throw std::runtime_error("Result::operator*() called on error: " + error_.message);
        return *data_;
    }
    const T& operator*() const {
        if (!has_value_) throw std::runtime_error("Result::operator*() called on error: " + error_.message);
        return *data_;
    }
    T* operator->() {
        if (!has_value_) throw std::runtime_error("Result::operator->() called on error: " + error_.message);
        return &*data_;
    }
    const T* operator->() const {
        if (!has_value_) throw std::runtime_error("Result::operator->() called on error: " + error_.message);
        return &*data_;
    }

private:
    std::optional<T> data_;
    Error error_;
    bool has_value_;
};

// ========== Result<void> 特化 ==========
template<>
class Result<void> {
public:
    Result() : has_value_(true) {}
    Result(Error error) : error_(std::move(error)), has_value_(false) {}

    static Result ok() { return Result(); }
    static Result err(Error error) { return Result(std::move(error)); }

    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value_; }

    Error& error() { return error_; }
    const Error& error() const { return error_; }

private:
    Error error_;
    bool has_value_;
};

} // namespace goose
