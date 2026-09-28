#pragma once

#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/data/store.hpp"
#include "gateway.pb.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace altsignalgateway::components {

// The protocol-2 query path: one journalled signal, a bounded page of the
// history, or the extended market context of a signal.
//
// It runs on its OWN thread with its own connections from the shared pool.
// The delivery pump's database thread is the one thing this service must never
// stall: a FULL context read is three foreign-server round trips per venue,
// and putting that behind a trade delivery would be a self-inflicted outage.
// Queries are read-only by construction -- there is no frame that writes --
// and every one of them is bounded before it reaches the database.

struct QueryJob {
    std::string session_id;
    std::string client_id;
    altsignalgateway::v1::QueryRequest request;
    // Delivered on the query thread; the session posts it onto its strand.
    std::function<void(altsignalgateway::v1::QueryResponse)> reply;
};

// Refuse what the server cannot honour BEFORE any database work: page sizes
// above the ceiling, an unparseable cursor, a GetSignal that names both or
// neither identity, an unspecified context detail, a filter token outside
// the vocabulary. Pure, so tests/unit/test_query.cpp exercises every branch.
[[nodiscard]] std::optional<altsignalgateway::v1::QueryError> validate_query(
    const altsignalgateway::v1::QueryRequest& req, const config::QueryConfig& cfg);

// The history cursor is an opaque token to the client and a sequence number
// here. Both directions are total functions of each other; a malformed token
// parses to nothing rather than to page one.
[[nodiscard]] std::string encode_cursor(int64_t before_sequence);
[[nodiscard]] std::optional<int64_t> decode_cursor(const std::string& token);

class QueryService {
public:
    QueryService(std::shared_ptr<data::JournalStore> store, const config::RuntimeConfig& cfg,
                 std::string gateway_version);
    ~QueryService();

    void start();
    void stop();

    // Enqueue. Returns false when the queue is at its bound; the caller then
    // answers ERR_QUERY_LIMIT itself without touching the database.
    bool submit(QueryJob job);

    // The response for one job, computed synchronously. Public so the admin
    // CLI and the selftest can drive it without a thread.
    altsignalgateway::v1::QueryResponse execute(const altsignalgateway::v1::QueryRequest& req);

private:
    void run();
    altsignalgateway::v1::QueryResponse get_signal(const altsignalgateway::v1::GetSignal& q);
    altsignalgateway::v1::QueryResponse get_history(const altsignalgateway::v1::GetHistory& q);
    altsignalgateway::v1::QueryResponse get_context(const altsignalgateway::v1::GetContext& q);
    void fill_signal_message(altsignalgateway::v1::SignalMessage* out,
                             const data::JournalRecord& rec) const;

    std::shared_ptr<data::JournalStore> store_;
    config::RuntimeConfig cfg_;
    std::string gateway_version_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<QueryJob> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace altsignalgateway::components
