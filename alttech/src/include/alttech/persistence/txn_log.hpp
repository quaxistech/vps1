#pragma once
// Internal AltTech transaction logging API.

#include "alttech/logging/logger.hpp"
#include "alttech/persistence/pg.hpp"

#include <utility>

namespace alttech::persistence {

// Fire-and-forget transactions must never swallow errors silently: failures
// are logged with a context tag. Returns true on commit.
template <typename Fn>
bool with_txn_logged(PgPool& pool, const char* context, Fn&& fn) {
    auto r = pool.with_txn(std::forward<Fn>(fn));
    if (!r) {
        log::error("db_txn_failed", {{"context", context},
                                     {"error_code", r.error().code_name()},
                                     {"message", r.error().message}});
        return false;
    }
    return true;
}

} // namespace alttech::persistence
