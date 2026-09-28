#include "altsignalgateway/components/query_service.hpp"

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/domain/canonical.hpp"
#include "altsignalgateway/domain/delivery_state.hpp"

#include <charconv>
#include <chrono>

namespace altsignalgateway::components {
namespace pb = altsignalgateway::v1;
namespace as = altstrategy::v1;

namespace {

constexpr size_t kMaxQueuedQueries = 64;
constexpr size_t kMaxSignalIdBytes = 128;
constexpr size_t kMaxFilterTokenBytes = 64;

pb::QueryResponse error_response(const std::string& request_id, pb::ErrorCode code,
                                 const std::string& message) {
    pb::QueryResponse out;
    out.set_request_id(request_id);
    out.mutable_error()->set_code(code);
    out.mutable_error()->set_message(message);
    return out;
}

bool token_ok(const std::string& s) {
    if (s.size() > kMaxFilterTokenBytes) return false;
    for (const unsigned char c : s) {
        if (!(std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' || c == ':')) return false;
    }
    return true;
}

const char* side_token(as::Side side) {
    switch (side) {
        case as::SIDE_LONG: return "long";
        case as::SIDE_SHORT: return "short";
        default: return "";
    }
}

template <typename Has, typename Set, typename T>
void put_opt(pb::DerivativesSnapshot* m, Has has, Set set, const std::optional<T>& v) {
    if (v.has_value()) {
        (m->*has)(true);
        (m->*set)(*v);
    }
}

void fill_derivatives(pb::DerivativesSnapshot* m, const domain::DerivativesSnapshot& d) {
    m->set_exchange(d.exchange);
    m->set_open_time_ms(d.open_time_ms);
    m->set_age_ms(d.age_ms);
    put_opt(m, &pb::DerivativesSnapshot::set_has_mark_price, &pb::DerivativesSnapshot::set_mark_price,
            d.mark_price);
    put_opt(m, &pb::DerivativesSnapshot::set_has_index_price,
            &pb::DerivativesSnapshot::set_index_price, d.index_price);
    put_opt(m, &pb::DerivativesSnapshot::set_has_funding_rate,
            &pb::DerivativesSnapshot::set_funding_rate, d.funding_rate);
    put_opt(m, &pb::DerivativesSnapshot::set_has_next_funding_time_ms,
            &pb::DerivativesSnapshot::set_next_funding_time_ms, d.next_funding_time_ms);
    put_opt(m, &pb::DerivativesSnapshot::set_has_open_interest_base,
            &pb::DerivativesSnapshot::set_open_interest_base, d.open_interest_base);
    put_opt(m, &pb::DerivativesSnapshot::set_has_open_interest_quote,
            &pb::DerivativesSnapshot::set_open_interest_quote, d.open_interest_quote);
    put_opt(m, &pb::DerivativesSnapshot::set_has_best_bid, &pb::DerivativesSnapshot::set_best_bid,
            d.best_bid);
    put_opt(m, &pb::DerivativesSnapshot::set_has_best_ask, &pb::DerivativesSnapshot::set_best_ask,
            d.best_ask);
    put_opt(m, &pb::DerivativesSnapshot::set_has_spread, &pb::DerivativesSnapshot::set_spread,
            d.spread);
    put_opt(m, &pb::DerivativesSnapshot::set_has_turnover_24h,
            &pb::DerivativesSnapshot::set_turnover_24h, d.turnover_24h);
    m->set_quality_status(d.quality_status);
    m->set_source_table(d.source_table);
}

void fill_book(pb::BookSnapshot* m, const domain::BookSnapshot& b) {
    m->set_exchange(b.exchange);
    m->set_open_time_ms(b.open_time_ms);
    m->set_age_ms(b.age_ms);
    const auto put = [m](void (pb::BookSnapshot::*has)(bool), void (pb::BookSnapshot::*set)(double),
                         const std::optional<double>& v) {
        if (v.has_value()) {
            (m->*has)(true);
            (m->*set)(*v);
        }
    };
    put(&pb::BookSnapshot::set_has_best_bid, &pb::BookSnapshot::set_best_bid, b.best_bid);
    put(&pb::BookSnapshot::set_has_best_ask, &pb::BookSnapshot::set_best_ask, b.best_ask);
    put(&pb::BookSnapshot::set_has_mid_price, &pb::BookSnapshot::set_mid_price, b.mid_price);
    put(&pb::BookSnapshot::set_has_spread, &pb::BookSnapshot::set_spread, b.spread);
    put(&pb::BookSnapshot::set_has_relative_spread, &pb::BookSnapshot::set_relative_spread,
        b.relative_spread);
    put(&pb::BookSnapshot::set_has_bid_depth_5, &pb::BookSnapshot::set_bid_depth_5, b.bid_depth_5);
    put(&pb::BookSnapshot::set_has_ask_depth_5, &pb::BookSnapshot::set_ask_depth_5, b.ask_depth_5);
    put(&pb::BookSnapshot::set_has_bid_depth_10, &pb::BookSnapshot::set_bid_depth_10,
        b.bid_depth_10);
    put(&pb::BookSnapshot::set_has_ask_depth_10, &pb::BookSnapshot::set_ask_depth_10,
        b.ask_depth_10);
    put(&pb::BookSnapshot::set_has_book_imbalance_5, &pb::BookSnapshot::set_book_imbalance_5,
        b.book_imbalance_5);
    put(&pb::BookSnapshot::set_has_book_imbalance_10, &pb::BookSnapshot::set_book_imbalance_10,
        b.book_imbalance_10);
    put(&pb::BookSnapshot::set_has_microprice, &pb::BookSnapshot::set_microprice, b.microprice);
    m->set_snapshot_count(b.snapshot_count);
    m->set_quality_status(b.quality_status);
    m->set_source_table(b.source_table);
}

void fill_flow(pb::FlowSnapshot* m, const domain::FlowSnapshot& f) {
    m->set_exchange(f.exchange);
    m->set_open_time_ms(f.open_time_ms);
    m->set_age_ms(f.age_ms);
    m->set_trade_count(f.trade_count);
    m->set_buy_trade_count(f.buy_trade_count);
    m->set_sell_trade_count(f.sell_trade_count);
    const auto put = [m](void (pb::FlowSnapshot::*has)(bool), void (pb::FlowSnapshot::*set)(double),
                         const std::optional<double>& v) {
        if (v.has_value()) {
            (m->*has)(true);
            (m->*set)(*v);
        }
    };
    put(&pb::FlowSnapshot::set_has_base_volume, &pb::FlowSnapshot::set_base_volume, f.base_volume);
    put(&pb::FlowSnapshot::set_has_quote_volume, &pb::FlowSnapshot::set_quote_volume,
        f.quote_volume);
    put(&pb::FlowSnapshot::set_has_aggressive_buy_volume,
        &pb::FlowSnapshot::set_aggressive_buy_volume, f.aggressive_buy_volume);
    put(&pb::FlowSnapshot::set_has_aggressive_sell_volume,
        &pb::FlowSnapshot::set_aggressive_sell_volume, f.aggressive_sell_volume);
    put(&pb::FlowSnapshot::set_has_signed_volume, &pb::FlowSnapshot::set_signed_volume,
        f.signed_volume);
    put(&pb::FlowSnapshot::set_has_vwap, &pb::FlowSnapshot::set_vwap, f.vwap);
    put(&pb::FlowSnapshot::set_has_largest_trade, &pb::FlowSnapshot::set_largest_trade,
        f.largest_trade);
    m->set_quality_status(f.quality_status);
    m->set_source_table(f.source_table);
}

void fill_candles(pb::CandleWindow* m, const domain::CandleWindow& w) {
    m->set_exchange(w.exchange);
    m->set_timeframe(w.timeframe);
    m->set_requested_bars(w.requested_bars);
    m->set_source_table(w.source_table);
    for (const auto& b : w.bars) {
        auto* bar = m->add_bars();
        bar->set_open_time_ms(b.open_time_ms);
        bar->set_open(b.open);
        bar->set_high(b.high);
        bar->set_low(b.low);
        bar->set_close(b.close);
        bar->set_base_volume(b.base_volume);
        bar->set_quote_volume(b.quote_volume);
        bar->set_trade_count(b.trade_count);
        if (b.taker_buy_base_volume) {
            bar->set_has_taker_buy_base_volume(true);
            bar->set_taker_buy_base_volume(*b.taker_buy_base_volume);
        }
        bar->set_is_closed(b.is_closed);
        bar->set_quality_status(b.quality_status);
    }
}

} // namespace

// ---------------------------------------------------------------------------

std::string encode_cursor(int64_t before_sequence) {
    return "s:" + std::to_string(before_sequence);
}

std::optional<int64_t> decode_cursor(const std::string& token) {
    if (token.size() < 3 || token.size() > 24 || token[0] != 's' || token[1] != ':') {
        return std::nullopt;
    }
    int64_t v = 0;
    const auto* begin = token.data() + 2;
    const auto* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc{} || ptr != end || v <= 0) return std::nullopt;
    return v;
}

std::optional<pb::QueryError> validate_query(const pb::QueryRequest& req,
                                             const config::QueryConfig& cfg) {
    const auto err = [](pb::ErrorCode code, std::string message) {
        pb::QueryError e;
        e.set_code(code);
        e.set_message(std::move(message));
        return e;
    };
    if (req.request_id().empty() || req.request_id().size() > 64) {
        return err(pb::ERR_INVALID_QUERY, "request_id must be 1..64 bytes");
    }
    switch (req.query_case()) {
        case pb::QueryRequest::kGetSignal: {
            const auto& q = req.get_signal();
            const bool by_id = !q.signal_id().empty();
            const bool by_seq = q.sequence() != 0;
            if (by_id == by_seq) {
                return err(pb::ERR_INVALID_QUERY, "GetSignal names exactly one of signal_id, sequence");
            }
            if (by_id && (q.signal_id().size() > kMaxSignalIdBytes || !token_ok(q.signal_id()))) {
                return err(pb::ERR_INVALID_QUERY, "signal_id is not a valid identifier");
            }
            if (by_seq && q.sequence() < 0) {
                return err(pb::ERR_INVALID_QUERY, "sequence must be positive");
            }
            return std::nullopt;
        }
        case pb::QueryRequest::kGetHistory: {
            const auto& q = req.get_history();
            if (q.page_size() < 0 || q.page_size() > cfg.max_page_size) {
                return err(pb::ERR_INVALID_QUERY,
                           "page_size must be 0 (default) or 1.." + std::to_string(cfg.max_page_size));
            }
            if (!q.cursor().empty() && !decode_cursor(q.cursor())) {
                return err(pb::ERR_INVALID_QUERY, "cursor is not a token this server issued");
            }
            const auto& f = q.filter();
            for (const auto* s : {&f.symbol(), &f.exchange(), &f.strategy_id()}) {
                if (!token_ok(*s)) return err(pb::ERR_INVALID_QUERY, "filter token is not a valid identifier");
            }
            if (f.canonical_pair_id() < 0) {
                return err(pb::ERR_INVALID_QUERY, "canonical_pair_id must be positive");
            }
            if (f.side() != as::SIDE_UNSPECIFIED && f.side() != as::SIDE_LONG &&
                f.side() != as::SIDE_SHORT) {
                return err(pb::ERR_INVALID_QUERY, "side must be SIDE_LONG or SIDE_SHORT");
            }
            if (!f.delivery_state().empty() && !domain::parse_delivery_state(f.delivery_state())) {
                return err(pb::ERR_INVALID_QUERY, "delivery_state is not a journal state");
            }
            if (f.from_created_at_ms() < 0 || f.to_created_at_ms() < 0 ||
                (f.from_created_at_ms() > 0 && f.to_created_at_ms() > 0 &&
                 f.to_created_at_ms() <= f.from_created_at_ms())) {
                return err(pb::ERR_INVALID_QUERY, "to_created_at_ms must be after from_created_at_ms");
            }
            return std::nullopt;
        }
        case pb::QueryRequest::kGetContext: {
            const auto& q = req.get_context();
            if (q.signal_id().empty() || q.signal_id().size() > kMaxSignalIdBytes ||
                !token_ok(q.signal_id())) {
                return err(pb::ERR_INVALID_QUERY, "signal_id is required");
            }
            if (q.detail() != pb::CONTEXT_STANDARD && q.detail() != pb::CONTEXT_RICH &&
                q.detail() != pb::CONTEXT_FULL) {
                return err(pb::ERR_INVALID_QUERY,
                           "detail must be CONTEXT_STANDARD, CONTEXT_RICH or CONTEXT_FULL");
            }
            return std::nullopt;
        }
        case pb::QueryRequest::QUERY_NOT_SET:
            break;
    }
    return err(pb::ERR_INVALID_QUERY, "query carries no body");
}

// ---------------------------------------------------------------------------

QueryService::QueryService(std::shared_ptr<data::JournalStore> store,
                           const config::RuntimeConfig& cfg, std::string gateway_version)
    : store_(std::move(store)), cfg_(cfg), gateway_version_(std::move(gateway_version)) {}

QueryService::~QueryService() { stop(); }

void QueryService::start() {
    std::lock_guard lock(mu_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void QueryService::stop() {
    {
        std::lock_guard lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool QueryService::submit(QueryJob job) {
    {
        std::lock_guard lock(mu_);
        if (stopping_ || queue_.size() >= kMaxQueuedQueries) return false;
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void QueryService::run() {
    log::set_thread_component("query");
    for (;;) {
        QueryJob job;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        const auto t0 = std::chrono::steady_clock::now();
        pb::QueryResponse response;
        try {
            response = execute(job.request);
        } catch (const std::exception& e) {
            log::error("query_failed", {{"session_id", job.session_id},
                                        {"client_id", job.client_id},
                                        {"request_id", job.request.request_id()},
                                        {"message", e.what()}});
            response = error_response(job.request.request_id(), pb::ERR_INTERNAL,
                                      "query failed; see the server log");
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        log::info("query_served", {{"session_id", job.session_id},
                                   {"client_id", job.client_id},
                                   {"request_id", job.request.request_id()},
                                   {"kind", static_cast<int>(job.request.query_case())},
                                   {"result", static_cast<int>(response.result_case())},
                                   {"latency_us", us}});
        if (job.reply) job.reply(std::move(response));
    }
}

void QueryService::fill_signal_message(pb::SignalMessage* out,
                                       const data::JournalRecord& rec) const {
    domain::EnvelopeSource src;
    src.schema_version = static_cast<uint32_t>(rec.schema_version);
    src.message_id = rec.message_id;
    src.signal_id = rec.signal_id;
    src.payload_sha256 = rec.payload_sha256;
    src.sequence = rec.sequence;
    src.created_at_ms = rec.created_at_ms;
    src.expires_at_ms = rec.expires_at_ms;
    src.journaled_at_ms = rec.journaled_at_ms;
    src.producer_version = gateway_version_;
    // A queried copy is a replay by definition: the envelope says so, and the
    // attempt counter is the journal's, not incremented -- a query is not a
    // delivery and moves no cursor.
    *out->mutable_envelope() = domain::build_envelope(
        src, /*replay=*/true, static_cast<uint32_t>(rec.delivery_attempts), timeutil::now_ms());
    // Parsed from the journalled canonical bytes and re-serialized by the same
    // deterministic serializer on the way out: for a payload with no unknown
    // fields that round trip is byte-stable, so the consumer's hash check
    // against envelope.payload_sha256 holds on a queried copy exactly as it
    // does on a delivered one. Asserted at runtime by the reference client.
    if (!out->mutable_payload()->ParseFromString(rec.canonical_payload)) {
        throw std::runtime_error("journalled payload for " + rec.signal_id + " does not parse");
    }
}

pb::QueryResponse QueryService::execute(const pb::QueryRequest& req) {
    if (const auto bad = validate_query(req, cfg_.query)) {
        return error_response(req.request_id(), bad->code(), bad->message());
    }
    pb::QueryResponse out;
    switch (req.query_case()) {
        case pb::QueryRequest::kGetSignal: out = get_signal(req.get_signal()); break;
        case pb::QueryRequest::kGetHistory: out = get_history(req.get_history()); break;
        case pb::QueryRequest::kGetContext: out = get_context(req.get_context()); break;
        default: return error_response(req.request_id(), pb::ERR_INVALID_QUERY, "query carries no body");
    }
    out.set_request_id(req.request_id());
    // The response must fit the frame bound the endpoint advertises. A page
    // that does not is refused whole, never truncated: a consumer that got
    // fewer rows than the page claims would silently miss signals.
    const auto bytes = messaging::canonical_serialize(out);
    if (!bytes) return error_response(req.request_id(), pb::ERR_INTERNAL, bytes.error().message);
    if (bytes->size() + 64 > cfg_.websocket.max_message_bytes) {
        return error_response(req.request_id(), pb::ERR_QUERY_LIMIT,
                              "response of " + std::to_string(bytes->size()) +
                                  " bytes exceeds max_message_bytes; narrow the query");
    }
    return out;
}

pb::QueryResponse QueryService::get_signal(const pb::GetSignal& q) {
    auto rec = q.signal_id().empty() ? store_->load_record_by_sequence(q.sequence())
                                     : store_->load_record(q.signal_id());
    if (!rec) throw std::runtime_error(rec.error().message);
    pb::QueryResponse out;
    if (!rec->has_value()) {
        out.mutable_error()->set_code(pb::ERR_NOT_FOUND);
        out.mutable_error()->set_message("no journalled signal with that identity");
        return out;
    }
    auto* res = out.mutable_signal();
    fill_signal_message(res->mutable_signal(), **rec);
    res->set_delivery_state(std::string(domain::delivery_state_name((*rec)->delivery_state)));
    return out;
}

pb::QueryResponse QueryService::get_history(const pb::GetHistory& q) {
    data::JournalStore::HistoryQuery hq;
    const auto& f = q.filter();
    hq.symbol = f.symbol();
    hq.canonical_pair_id = f.canonical_pair_id();
    hq.exchange = f.exchange();
    hq.strategy_id = f.strategy_id();
    hq.side = side_token(f.side());
    hq.delivery_state = f.delivery_state();
    hq.from_created_at_ms = f.from_created_at_ms();
    hq.to_created_at_ms = f.to_created_at_ms();
    hq.before_sequence = q.cursor().empty() ? 0 : decode_cursor(q.cursor()).value_or(0);
    hq.page_size = q.page_size() == 0 ? cfg_.query.default_page_size : q.page_size();

    auto page = store_->history(hq);
    if (!page) throw std::runtime_error(page.error().message);

    pb::QueryResponse out;
    auto* h = out.mutable_history();
    *h->mutable_filter() = f;
    h->set_page_size(hq.page_size);
    h->set_query_time_ms(timeutil::now_ms());
    h->set_count(static_cast<int32_t>(page->records.size()));
    h->set_next_cursor(page->next_before_sequence > 0 ? encode_cursor(page->next_before_sequence)
                                                      : std::string());
    h->set_journal_head_sequence(page->head_sequence);
    for (const auto& rec : page->records) {
        auto* e = h->add_entries();
        pb::SignalMessage msg;
        fill_signal_message(&msg, rec);
        *e->mutable_envelope() = msg.envelope();
        *e->mutable_summary() = domain::summarize_payload(
            msg.payload(), domain::delivery_state_name(rec.delivery_state), rec.last_ack_at_ms);
    }
    return out;
}

pb::QueryResponse QueryService::get_context(const pb::GetContext& q) {
    auto rec = store_->load_record(q.signal_id());
    if (!rec) throw std::runtime_error(rec.error().message);
    pb::QueryResponse out;
    if (!rec->has_value()) {
        out.mutable_error()->set_code(pb::ERR_NOT_FOUND);
        out.mutable_error()->set_message("no journalled signal with that identity");
        return out;
    }
    const data::JournalRecord& r = **rec;
    auto* res = out.mutable_context();
    fill_signal_message(res->mutable_signal(), r);
    res->set_delivery_state(std::string(domain::delivery_state_name(r.delivery_state)));
    if (q.detail() == pb::CONTEXT_STANDARD) return out;

    auto* ext = res->mutable_extended();
    ext->set_detail(q.detail());
    ext->set_t0_ms(r.created_at_ms);
    ext->set_read_at_ms(timeutil::now_ms());

    // Macro: the journalled block when the payload carries one (schema 2);
    // otherwise read as of T0 now. Both are the same as-of rule.
    const auto& payload = res->signal().payload();
    if (payload.macro().present()) {
        *ext->mutable_macro() = payload.macro();
    } else {
        auto m = store_->load_macro_asof(
            r.created_at_ms, static_cast<int64_t>(cfg_.query.macro_max_age_sec) * 1000);
        if (!m) {
            domain::MacroSnapshot failed;
            failed.present = false;
            failed.absence_reason = "MACRO_READ_FAILED";
            domain::fill_macro(ext->mutable_macro(), failed);
            ext->add_absent("macro:MACRO_READ_FAILED");
            log::error("macro_context_read_failed",
                       {{"signal_id", r.signal_id}, {"message", m.error().message}});
        } else {
            domain::fill_macro(ext->mutable_macro(), *m);
            if (!m->present) ext->add_absent("macro:" + m->absence_reason);
        }
    }

    const bool full = q.detail() == pb::CONTEXT_FULL;
    std::vector<data::JournalStore::CandleWindowSpec> windows;
    if (full) {
        windows = {{"1m", cfg_.query.candles_1m}, {"5m", cfg_.query.candles_5m},
                   {"1h", cfg_.query.candles_1h}};
    }
    auto x = store_->load_extended(payload.trade_plan().canonical_pair_id(), r.created_at_ms,
                                   full, windows);
    if (!x) {
        ext->add_absent("market:READ_FAILED");
        log::error("extended_context_read_failed",
                   {{"signal_id", r.signal_id}, {"message", x.error().message}});
        return out;
    }
    for (const auto& d : x->derivatives) fill_derivatives(ext->add_derivatives(), d);
    for (const auto& b : x->book) fill_book(ext->add_book(), b);
    for (const auto& f : x->flow) fill_flow(ext->add_flow(), f);
    for (const auto& w : x->candles) fill_candles(ext->add_candles(), w);
    for (const auto& a : x->absent) ext->add_absent(a);
    return out;
}

} // namespace altsignalgateway::components
