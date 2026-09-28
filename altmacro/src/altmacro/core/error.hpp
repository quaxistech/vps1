#pragma once

#include <expected>
#include <string>
#include <utility>

namespace altmacro {

enum class ErrorCode {
    ok = 0,
    invalid_argument,
    parse_error,
    validation_error,
    config_error,
    network_error,
    timeout,
    rate_limited,
    not_found,
    // The provider answered correctly and the answer is "no observation here",
    // which is data, not failure. Kept distinct from not_found so that a
    // closed market is never retried as though it were an outage.
    no_observation,
    database_error,
    messaging_error,
    shutdown_requested,
    internal_error,
};

[[nodiscard]] constexpr const char* error_code_name(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::ok: return "ok";
        case ErrorCode::invalid_argument: return "invalid_argument";
        case ErrorCode::parse_error: return "parse_error";
        case ErrorCode::validation_error: return "validation_error";
        case ErrorCode::config_error: return "config_error";
        case ErrorCode::network_error: return "network_error";
        case ErrorCode::timeout: return "timeout";
        case ErrorCode::rate_limited: return "rate_limited";
        case ErrorCode::not_found: return "not_found";
        case ErrorCode::no_observation: return "no_observation";
        case ErrorCode::database_error: return "database_error";
        case ErrorCode::messaging_error: return "messaging_error";
        case ErrorCode::shutdown_requested: return "shutdown_requested";
        case ErrorCode::internal_error: return "internal_error";
    }
    return "unknown";
}

struct Error {
    ErrorCode code = ErrorCode::internal_error;
    std::string message;
    bool retryable = false;

    [[nodiscard]] const char* code_name() const noexcept { return error_code_name(code); }
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(ErrorCode code, std::string message, bool retryable = false) {
    return std::unexpected(Error{code, std::move(message), retryable});
}

} // namespace altmacro
