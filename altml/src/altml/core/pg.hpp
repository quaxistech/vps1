#pragma once

#include "altml/core/error.hpp"

#include <pqxx/pqxx>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace altml::persistence {

// Bounded PostgreSQL connection pool. pqxx::connection is not thread-safe,
// so connections are leased exclusively. Broken connections are discarded
// and lazily re-established on the next lease.
class PgPool {
public:
    PgPool(std::string dsn, size_t size);
    ~PgPool();
    PgPool(const PgPool&) = delete;
    PgPool& operator=(const PgPool&) = delete;

    // Shutdown: cancel the statement running on every leased connection of
    // every pool in the process (libpq's PQcancel is safe from another
    // thread). A component blocked in a minutes-long query -- the trainer's
    // data load, the FDW archive backfill -- then gets an error back at once
    // and sees the stop, instead of holding the process past systemd's stop
    // timeout until it is SIGKILLed (every AltML stop on 2026-09-25 was).
    static void cancel_all_in_flight();

    class Lease {
    public:
        Lease(PgPool& pool, std::unique_ptr<pqxx::connection> conn)
            : pool_(&pool), conn_(std::move(conn)) {
            pool_->track(conn_.get(), true);
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& o) noexcept : pool_(o.pool_), conn_(std::move(o.conn_)) { o.pool_ = nullptr; }
        ~Lease();

        pqxx::connection& conn() { return *conn_; }
        // Mark connection as broken; it will not be returned to the pool.
        void discard() {
            if (pool_ != nullptr && conn_ != nullptr) pool_->track(conn_.get(), false);
            conn_.reset();
        }

    private:
        PgPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    // Blocks until a connection is available or timeout; creates lazily up to `size`.
    Result<Lease> acquire(std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // Convenience: run `fn(pqxx::work&)` in a transaction with commit.
    // On connection failure the connection is discarded and the error returned.
    template <typename Fn>
    Result<void> with_txn(Fn&& fn) {
        auto lease = acquire();
        if (!lease) return std::unexpected(lease.error());
        try {
            pqxx::work txn(lease->conn());
            fn(txn);
            txn.commit();
            return {};
        } catch (const pqxx::broken_connection& e) {
            lease->discard();
            return fail(ErrorCode::database_error, std::string("connection lost: ") + e.what(), true);
        } catch (const pqxx::sql_error& e) {
            return fail(ErrorCode::database_error,
                        std::string("SQL error: ") + e.what() + " [" + e.query().substr(0, 200) + "]");
        } catch (const std::exception& e) {
            return fail(ErrorCode::database_error, std::string("database error: ") + e.what());
        }
    }

private:
    friend class Lease;
    void release(std::unique_ptr<pqxx::connection> conn);
    void track(pqxx::connection* c, bool leased);

    std::string dsn_;
    size_t size_;
    size_t created_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
    std::mutex leased_mu_;
    std::set<pqxx::connection*> leased_;
};

// Quote a timestamptz literal from Unix milliseconds (ISO-8601 UTC).
std::string ts_literal(pqxx::transaction_base& txn, int64_t ms);

} // namespace altml::persistence
