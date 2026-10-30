// Primary-venue microstructure module: trade and order-book features over
// tumbling 1s / 5s / 15s / 30s / 1m windows.
//
// All five windows persist by default under bounded retention, before their
// notification is delivered from the durable outbox. A deliberately omitted
// window remains live-only and cannot claim historical train/serve coverage.
//
// The application owns PostgreSQL, NATS and process lifecycle; this module owns
// only its calculation state and subscriptions. The same module drives the
// reference-venue 1m microstructure sweep.

#include "alttech/application/module_supervisor.hpp"
#include "venue_sweep.hpp"
#include "../ofi/ofi_windows.hpp"
#include "alttech/modules/microstructure.hpp"
#include "alttech/common/backoff.hpp"
#include "alttech/common/hash.hpp"
#include "alttech/common/shutdown.hpp"
#include "alttech/config/config.hpp"
#include "alttech/domain/types.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/persistence/pg.hpp"
#include "alttech/persistence/analytics_outbox.hpp"
#include "alttech/persistence/txn_log.hpp"
#include "alttech/time/time.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace alttech;

namespace {

constexpr std::string_view kModuleName = "microstructure";
constexpr const char* kMicroEngineVersion = "alttech-micro-1";
constexpr const char* kMicroSetName = "bitget_micro";
constexpr int kMicroSetVersion = 1;
constexpr const char* kMicroProfile = "bitget_full";
constexpr const char* kCheckpointModule = "microstructure";

// A trade counts as "large" when it exceeds this multiple of the window's own
// median trade size. Using the median keeps the threshold scale-free across
// instruments and regimes.
constexpr double kLargeTradeMedianMultiple = 3.0;
// Smoothing of the reference trade count the burst score is measured against.
constexpr double kBurstBaselineAlpha = 0.1;

// Upper bound on windows held in memory while writes are failing. Roughly an
// hour of the persisted windows across a normal universe. Without a bound a
// sustained write failure grows the queue until the process is OOM-killed —
// measured at 17 577 pending during one such outage.
constexpr std::size_t kMaxPendingWindows = 20'000;

const std::vector<std::string>& micro_feature_names() {
    static const std::vector<std::string> names = {
        "trade_count", "trade_intensity", "aggressive_buy_volume",
        "aggressive_sell_volume", "trade_imbalance", "signed_volume", "delta", "cvd",
        "average_trade_size", "median_trade_size", "max_trade_size",
        "large_trade_intensity", "buy_trade_count", "sell_trade_count", "vwap",
        "short_term_price_impact", "volume_acceleration", "burst_score",
        "book_spread_bps", "book_imbalance", "book_microprice_dev", "book_bid_depth",
        "book_ask_depth", "book_depth_ratio", "book_mid_return", "book_sample_count",
    };
    return names;
}

const std::vector<std::string>& ofi_feature_names() {
    static const std::vector<std::string> names = {
        "ofi_base", "ofi_depth_normalized", "ofi_event_count", "ofi_mean_depth_base"};
    return names;
}

struct MicroTrade {
    int64_t ts_ms = 0;
    double price = 0;
    double size = 0;
    bool is_buy = false;
};

struct WindowSpec {
    const char* label;
    int64_t span_ms;
};

constexpr WindowSpec kWindows[] = {
    {"1s", 1'000}, {"5s", 5'000}, {"15s", 15'000}, {"30s", 30'000}, {"1m", 60'000}};

// Partition class of a persisted window: the 1m family is retained for days,
// the sub-minute family only intraday.
[[nodiscard]] const char* part_class_for(int64_t span_ms) {
    return span_ms == 60'000 ? "bitget_1m" : "bitget_sub";
}

struct BookSample {
    int64_t ts_ms = 0;
    double mid = 0;
    double microprice = 0;
    double spread_bps = 0;
    double bid_depth = 0;
    double ask_depth = 0;
    double imbalance = 0;
};

struct WindowAccum {
    int64_t start_ms = 0;
    std::vector<MicroTrade> trades;
    std::vector<BookSample> books;
    double prev_window_volume = -1;
    double prev_burst_avg = -1;
};

struct InstrumentState {
    int64_t instrument_id = 0;
    std::string symbol;
    double cvd = 0;
    bool cvd_dirty = false;
    uint64_t cvd_generation = 0;
    std::array<WindowAccum, std::size(kWindows)> windows{};
    int64_t trades_seen = 0;
};

struct MicroVector {
    Venue venue = Venue::bitget;
    bool ofi = false;
    bool complete = true;
    std::string symbol;
    int64_t instrument_id = 0;
    std::string window_label;
    int64_t span_ms = 0;
    int64_t window_start_ms = 0;
    int64_t computed_at_ms = 0;
    std::vector<float> values;
    std::vector<uint8_t> validity;
};

std::optional<double> parse_finite_double(std::string_view text) noexcept {
    if (text.empty()) return std::nullopt;
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

Result<double> decimal_to_double(const Decimal& value, std::string_view field) {
    static constexpr std::array<double, 19> kScale = {
        1.0,
        10.0,
        100.0,
        1'000.0,
        10'000.0,
        100'000.0,
        1'000'000.0,
        10'000'000.0,
        100'000'000.0,
        1'000'000'000.0,
        10'000'000'000.0,
        100'000'000'000.0,
        1'000'000'000'000.0,
        10'000'000'000'000.0,
        100'000'000'000'000.0,
        1'000'000'000'000'000.0,
        10'000'000'000'000'000.0,
        100'000'000'000'000'000.0,
        1'000'000'000'000'000'000.0,
    };
    if (value.scale() < 0 || value.scale() > Decimal::kMaxScale) {
        return fail(ErrorCode::validation_error,
                    std::string(field) + " decimal scale is outside [0, 18]");
    }
    const double converted =
        static_cast<double>(value.mantissa()) / kScale[static_cast<size_t>(value.scale())];
    if (!std::isfinite(converted)) {
        return fail(ErrorCode::validation_error, std::string(field) + " is not finite");
    }
    return converted;
}

Result<void> validate_wire_decimal(const ::alttech::v1::Decimal& value, std::string_view field) {
    if (value.scale() < 0 || value.scale() > Decimal::kMaxScale) {
        return fail(ErrorCode::validation_error,
                    std::string(field) + " wire decimal scale is outside [0, 18]");
    }
    return {};
}

class MicroService {
public:
    MicroService(const YAML::Node& config, persistence::PgPool& pool,
                 messaging::NatsClient& nats)
        : pool_(pool), nats_(nats) {
        std::string joined;
        for (const auto& name : micro_feature_names()) {
            joined += name;
            joined += '\n';
        }
        schema_hash_ = sha256_hex(joined);
        joined.clear();
        for (const auto& name : ofi_feature_names()) joined += name + '\n';
        ofi_schema_hash_ = sha256_hex(joined);
        book_page_size_ = static_cast<size_t>(
            config::opt_int(config, "book_poll_page_size", 50'000));
        book_max_pages_ = static_cast<size_t>(
            config::opt_int(config, "book_poll_max_pages_per_tick", 4));
        book_overlap_ms_ = config::opt_int(config, "book_poll_overlap_ms", 2'000);
        store_batch_size_ = static_cast<size_t>(
            config::opt_int(config, "store_batch_size", 2'000));
        const std::string persisted =
            config::opt_str(config, "persist_windows", "1s,5s,15s,30s,1m");
        for (size_t begin = 0, end = 0; begin < persisted.size(); begin = end + 1) {
            end = persisted.find(',', begin);
            if (end == std::string::npos) end = persisted.size();
            std::string window = persisted.substr(begin, end - begin);
            while (!window.empty() && window.front() == ' ') window.erase(window.begin());
            while (!window.empty() && window.back() == ' ') window.pop_back();
            if (!window.empty()) persist_windows_.insert(window);
        }
    }

    Result<void> init() {
        auto set_id = ensure_set();
        if (!set_id) return std::unexpected(set_id.error());
        set_id_ = *set_id;
        auto ofi_set = ensure_set(true);
        if (!ofi_set) return std::unexpected(ofi_set.error());
        ofi_set_id_ = *ofi_set;

        std::vector<std::pair<int64_t, std::string>> instruments;
        auto result = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec(
                "SELECT ei.instrument_id, ei.exchange_symbol "
                "FROM global_observation_universe u "
                "JOIN canonical_pair_exchange_map m "
                "  ON m.canonical_pair_id = u.canonical_pair_id AND m.exchange_id = 1 "
                "JOIN exchange_instruments ei ON ei.id = m.exchange_instrument_id "
                "WHERE u.eligible AND ei.is_whitelisted AND ei.status = 'active' "
                "ORDER BY ei.exchange_symbol");
            for (const auto& row : rows) {
                instruments.emplace_back(row[0].as<int64_t>(), row[1].as<std::string>());
            }

            const auto checkpoints = txn.exec_params(
                "SELECT canonical_pair_id, state FROM alttech.module_checkpoints "
                "WHERE module = $1 AND exchange_id = 1 AND engine_version = $2",
                std::string(kCheckpointModule), std::string(kMicroEngineVersion));
            for (const auto& row : checkpoints) {
                const auto raw = row[1].as<std::basic_string<std::byte>>();
                if (raw.size() != sizeof(double)) continue;
                double value = 0;
                std::memcpy(&value, raw.data(), sizeof(double));
                if (std::isfinite(value)) {
                    cvd_restore_[row[0].as<int64_t>()] = value;
                } else {
                    log::warn("micro_checkpoint_invalid",
                              {{"instrument_id", row[0].as<int64_t>()},
                               {"message", "non-finite CVD checkpoint ignored"}});
                }
            }
        });
        if (!result) return std::unexpected(result.error());

        {
            std::lock_guard lock(mu_);
            for (const auto& [instrument_id, symbol] : instruments) {
                auto state = std::make_unique<InstrumentState>();
                state->instrument_id = instrument_id;
                state->symbol = symbol;
                if (const auto it = cvd_restore_.find(instrument_id);
                    it != cvd_restore_.end()) {
                    state->cvd = it->second;
                }
                by_symbol_[symbol] = state.get();
                by_iid_[instrument_id] = state.get();
                states_.push_back(std::move(state));
            }
        }

        // Live microstructure has no exchange backfill. Start at the current
        // database time and use a small overlap only on subsequent polls.
        book_highwater_ms_ = timeutil::now_ms();
        book_highwater_us_ = book_highwater_ms_ * 1'000;
        book_highwater_iid_ = std::numeric_limits<int64_t>::max();
        book_highwater_time_ = timeutil::to_iso8601(book_highwater_ms_);
        if (book_highwater_time_.empty()) {
            return fail(ErrorCode::internal_error, "failed to initialize order-book cursor");
        }

        log::info("micro_instruments",
                  {{"count", states_.size()}, {"cvd_restored", cvd_restore_.size()}});
        return {};
    }

    Result<void> on_trade(const std::string& symbol, const Trade& trade) {
        if (symbol.empty() || trade.exchange_ts_ms <= 0) {
            return fail(ErrorCode::validation_error, "trade has no symbol or timestamp");
        }
        if (trade.side != TradeSide::buy && trade.side != TradeSide::sell) {
            return fail(ErrorCode::validation_error, "trade has an unsupported side");
        }
        auto price = decimal_to_double(trade.price, "trade.price");
        if (!price) return std::unexpected(price.error());
        auto size = decimal_to_double(trade.size, "trade.size");
        if (!size) return std::unexpected(size.error());
        if (*price <= 0 || *size < 0) {
            return fail(ErrorCode::validation_error, "trade price/size is outside valid range");
        }

        const MicroTrade converted{trade.exchange_ts_ms, *price, *size,
                                   trade.side == TradeSide::buy};
        std::lock_guard lock(mu_);
        const auto it = by_symbol_.find(symbol);
        if (it == by_symbol_.end()) return {};
        InstrumentState& state = *it->second;
        ++state.trades_seen;
        for (auto& window : state.windows) window.trades.push_back(converted);
        return {};
    }

    Result<void> on_book_delta(const ::alttech::v1::Envelope& envelope) {
        const auto venue = venue_from_id(static_cast<int32_t>(envelope.exchange()));
        if (!venue || !is_reference(*venue) || !envelope.has_order_book_delta() ||
            envelope.instrument_id() <= 0 || envelope.local_receive_time_ms() <= 0 ||
            envelope.connection_id().empty()) {
            return fail(ErrorCode::validation_error, "OFI requires identified reference-venue delta and receipt time");
        }
        const auto& delta = envelope.order_book_delta();
        ofi::Event event;
        event.connection = envelope.connection_id();
        event.epoch = delta.book_epoch();
        event.counter = delta.delta_counter();
        // The family is explicitly receipt-windowed. Late exchange events
        // cannot rewrite a vector that was already available to a decision.
        event.received_ms = envelope.local_receive_time_ms();
        if (delta.has_ofi_base() && delta.has_bbo_depth_base() &&
            validate_wire_decimal(delta.ofi_base(), "ofi_base") &&
            validate_wire_decimal(delta.bbo_depth_base(), "bbo_depth_base")) {
            auto flow = decimal_to_double(messaging::from_proto(delta.ofi_base()), "ofi_base");
            auto depth = decimal_to_double(messaging::from_proto(delta.bbo_depth_base()), "bbo_depth_base");
            if (flow && depth && *depth > 0) {
                event.flow_base = *flow;
                event.depth_base = *depth;
                event.available = true;
            }
        }
        std::lock_guard lock(mu_);
        if (!by_iid_.contains(envelope.instrument_id())) return {};
        auto& state = ofi_states_[{venue_id(*venue), envelope.instrument_id()}];
        state.symbol = envelope.exchange_symbol();
        state.windows.add(event);
        return {};
    }

    void poll_books() noexcept {
        struct Row {
            int64_t iid = 0;
            int64_t ts_ms = 0;
            int64_t ts_us = 0;
            std::string cursor_time;
            std::optional<BookSample> sample;
        };

        try {
            std::string page_time = book_poll_started_
                                        ? timeutil::to_iso8601(
                                              book_highwater_ms_ - book_overlap_ms_)
                                        : book_highwater_time_;
            int64_t page_iid = book_poll_started_ ? 0 : book_highwater_iid_;
            if (page_time.empty()) {
                log::warn("micro_book_cursor_invalid", {});
                return;
            }

            int64_t accepted = 0;
            int64_t invalid = 0;
            for (size_t page = 0; page < book_max_pages_; ++page) {
                std::vector<Row> rows;
                auto result = pool_.with_txn([&](pqxx::work& txn) {
                    txn.exec("SET LOCAL TIME ZONE 'UTC'");
                    const auto db_rows = txn.exec_params(
                        "SELECT instrument_id, "
                        "       (extract(epoch FROM snapshot_time)*1000)::bigint, "
                        "       (extract(epoch FROM snapshot_time)*1000000)::bigint, "
                        "       snapshot_time::text, mid::text, "
                        "       COALESCE(microprice, mid)::text, "
                        "       COALESCE(spread_bps, 0)::text, bid_depth_n::text, "
                        "       ask_depth_n::text, COALESCE(imbalance_n, 0)::text "
                        "FROM market_orderbook_snapshots "
                        "WHERE exchange_id = 1 "
                        "  AND (snapshot_time, instrument_id) > ($1::timestamptz, $2::bigint) "
                        "ORDER BY snapshot_time, instrument_id LIMIT $3",
                        page_time, page_iid, static_cast<int64_t>(book_page_size_));

                    rows.reserve(db_rows.size());
                    for (const auto& db_row : db_rows) {
                        Row row;
                        row.iid = db_row[0].as<int64_t>();
                        row.ts_ms = db_row[1].as<int64_t>();
                        row.ts_us = db_row[2].as<int64_t>();
                        row.cursor_time = db_row[3].as<std::string>();

                        const auto mid = parse_finite_double(db_row[4].as<std::string>());
                        const auto micro = parse_finite_double(db_row[5].as<std::string>());
                        const auto spread = parse_finite_double(db_row[6].as<std::string>());
                        const auto bid_depth = parse_finite_double(db_row[7].as<std::string>());
                        const auto ask_depth = parse_finite_double(db_row[8].as<std::string>());
                        const auto imbalance = parse_finite_double(db_row[9].as<std::string>());
                        if (mid && micro && spread && bid_depth && ask_depth && imbalance &&
                            *mid > 0 && *micro > 0 && *spread >= 0 && *bid_depth >= 0 &&
                            *ask_depth >= 0) {
                            row.sample = BookSample{row.ts_ms, *mid, *micro, *spread,
                                                    *bid_depth, *ask_depth, *imbalance};
                        }
                        rows.push_back(std::move(row));
                    }
                });
                if (!result) {
                    log::warn("micro_book_poll_failed",
                              {{"message", result.error().message}});
                    return;
                }
                if (rows.empty()) break;

                {
                    std::lock_guard lock(mu_);
                    for (const auto& row : rows) {
                        const auto key = std::pair{row.iid, row.ts_us};
                        if (!seen_book_keys_.insert(key).second) continue;
                        if (!row.sample) {
                            ++invalid;
                            continue;
                        }
                        const auto state = by_iid_.find(row.iid);
                        if (state == by_iid_.end()) continue;
                        for (auto& window : state->second->windows) {
                            window.books.push_back(*row.sample);
                        }
                        ++accepted;
                    }
                }

                const Row& last = rows.back();
                page_time = last.cursor_time;
                page_iid = last.iid;
                if (last.ts_us > book_highwater_us_ ||
                    (last.ts_us == book_highwater_us_ && last.iid > book_highwater_iid_)) {
                    book_highwater_us_ = last.ts_us;
                    book_highwater_ms_ = last.ts_ms;
                    book_highwater_iid_ = last.iid;
                    book_highwater_time_ = last.cursor_time;
                }
                if (rows.size() < book_page_size_) break;
            }
            book_poll_started_ = true;

            const int64_t cutoff_us =
                book_highwater_us_ - std::max<int64_t>(book_overlap_ms_ * 2, 1'000) * 1'000;
            std::erase_if(seen_book_keys_,
                          [cutoff_us](const auto& key) { return key.second < cutoff_us; });
            books_polled_.fetch_add(accepted, std::memory_order_relaxed);
            if (invalid > 0) {
                log::warn("micro_book_rows_rejected", {{"count", invalid}});
            }
        } catch (const std::exception& error) {
            log::warn("micro_book_poll_exception", {{"message", error.what()}});
        } catch (...) {
            log::warn("micro_book_poll_exception",
                      {{"message", "non-standard exception"}});
        }
    }

    void tick(int64_t now_ms) {
        poll_books();
        {
            std::lock_guard lock(mu_);
            for (auto& [key, state] : ofi_states_) {
                for (const auto& window : state.windows.close(now_ms)) {
                    MicroVector vector;
                    vector.venue = static_cast<Venue>(key.first);
                    vector.ofi = true;
                    vector.complete = window.valid;
                    vector.symbol = state.symbol;
                    vector.instrument_id = key.second;
                    vector.window_start_ms = window.start_ms;
                    vector.span_ms = window.span_ms;
                    vector.computed_at_ms = now_ms;
                    for (const auto& spec : kWindows) if (spec.span_ms == window.span_ms) vector.window_label = spec.label;
                    vector.values = {static_cast<float>(window.flow_base), static_cast<float>(window.depth_normalized),
                                     static_cast<float>(window.updates), static_cast<float>(window.mean_depth_base)};
                    vector.validity = {static_cast<uint8_t>(window.valid ? 0x0f : 0)};
                    if (pending_store_.size() >= kMaxPendingWindows) { pending_store_.pop_front(); ++dropped_windows_; }
                    pending_store_.push_back(std::move(vector));
                }
            }
            for (auto& state_ptr : states_) {
                InstrumentState& state = *state_ptr;
                for (size_t index = 0; index < std::size(kWindows); ++index) {
                    const auto& spec = kWindows[index];
                    WindowAccum& accum = state.windows[index];
                    const int64_t window_start = (now_ms / spec.span_ms) * spec.span_ms;
                    if (accum.start_ms == 0) {
                        accum.start_ms = window_start;
                        continue;
                    }
                    // Process every completed bucket in order. Jumping the
                    // cursor to wall time silently discarded intermediate
                    // windows whenever a DB poll or publish delayed the tick.
                    size_t closed_windows = 0;
                    while (window_start > accum.start_ms && closed_windows++ < 128) {

                        const int64_t closed_start = accum.start_ms;
                        const int64_t closed_end = closed_start + spec.span_ms;
                        std::vector<MicroTrade> trades_in;
                        std::vector<MicroTrade> trades_rest;
                        for (const auto& trade : accum.trades) {
                            if (trade.ts_ms >= closed_start && trade.ts_ms < closed_end) {
                                trades_in.push_back(trade);
                            } else if (trade.ts_ms >= closed_end) {
                                trades_rest.push_back(trade);
                            }
                        }
                        accum.trades = std::move(trades_rest);

                        std::vector<BookSample> books_in;
                        std::vector<BookSample> books_rest;
                        for (const auto& book : accum.books) {
                            if (book.ts_ms >= closed_start && book.ts_ms < closed_end) {
                                books_in.push_back(book);
                            } else if (book.ts_ms >= closed_end) {
                                books_rest.push_back(book);
                            }
                        }
                        accum.books = std::move(books_rest);

                        std::sort(trades_in.begin(), trades_in.end(), [](const auto& a, const auto& b) { return a.ts_ms < b.ts_ms; });
                        std::sort(books_in.begin(), books_in.end(), [](const auto& a, const auto& b) { return a.ts_ms < b.ts_ms; });

                        MicroVector vector =
                            compute(state, spec, closed_start, trades_in, books_in, accum);
                        vector.computed_at_ms = now_ms;
                        accum.start_ms = closed_end;
                        if (spec.span_ms == 60'000) {
                            state.cvd_dirty = true;
                            ++state.cvd_generation;
                        }
                        // DB-first for persisted windows: they become publishable
                        // only after store() commits. Live-only windows go straight
                        // to the publication queue.
                        if (persist_windows_.contains(vector.window_label)) {
                            // Drop the oldest rather than grow without bound: the
                            // newest windows are the ones still worth publishing,
                            // and the database is the record of what was missed.
                            if (pending_store_.size() >= kMaxPendingWindows) {
                                pending_store_.pop_front();
                                ++dropped_windows_;
                            }
                            pending_store_.push_back(std::move(vector));
                        } else {
                            if (pending_publish_.size() >= kMaxPendingWindows) {
                                pending_publish_.pop_front();
                                ++dropped_windows_;
                            }
                            pending_publish_.push_back(std::move(vector));
                        }
                    }
                }
            }
        }
        flush_pending(now_ms, false);
    }

    void checkpoint_cvd() noexcept {
        struct Checkpoint {
            InstrumentState* state = nullptr;
            int64_t instrument_id = 0;
            double cvd = 0;
            uint64_t generation = 0;
        };
        try {
            std::vector<Checkpoint> snapshot;
            {
                std::lock_guard lock(mu_);
                for (auto& state : states_) {
                    if (state->cvd_dirty) {
                        snapshot.push_back(
                            {state.get(), state->instrument_id, state->cvd,
                             state->cvd_generation});
                    }
                }
            }
            if (snapshot.empty()) return;

            const bool committed = persistence::with_txn_logged(
                pool_, "micro_cvd_checkpoint", [&](pqxx::work& txn) {
                    for (const auto& checkpoint : snapshot) {
                        std::string hex = "\\x";
                        const auto* bytes = reinterpret_cast<const unsigned char*>(
                            &checkpoint.cvd);
                        static constexpr char digits[] = "0123456789abcdef";
                        for (size_t i = 0; i < sizeof(double); ++i) {
                            hex += digits[bytes[i] >> 4U];
                            hex += digits[bytes[i] & 0x0FU];
                        }
                        txn.exec_params(
                            "INSERT INTO alttech.module_checkpoints "
                            "(module, exchange_id, canonical_pair_id, timeframe, "
                            "engine_version, last_event_time, state) "
                            "VALUES ($4, 1, $1, '', $2, now(), $3::bytea) "
                            "ON CONFLICT (module, exchange_id, canonical_pair_id, timeframe, "
                            "engine_version) DO UPDATE SET last_event_time = now(), "
                            "state = EXCLUDED.state, updated_at = now()",
                            checkpoint.instrument_id, std::string(kMicroEngineVersion), hex,
                            std::string(kCheckpointModule));
                    }
                });
            if (!committed) return;

            std::lock_guard lock(mu_);
            for (const auto& checkpoint : snapshot) {
                if (checkpoint.state->cvd_generation == checkpoint.generation) {
                    checkpoint.state->cvd_dirty = false;
                }
            }
        } catch (const std::exception& error) {
            log::warn("micro_checkpoint_exception", {{"message", error.what()}});
        } catch (...) {
            log::warn("micro_checkpoint_exception",
                      {{"message", "non-standard exception"}});
        }
    }

    const std::set<std::string>& persisted_windows() const { return persist_windows_; }

    void log_stats() {
        log::info("micro_stats",
                  {{"windows_published", published_.exchange(0)},
                   {"book_snapshots_polled", books_polled_.exchange(0)},
                   {"pending_durable_windows", pending_store_.size()},
                   {"pending_publications", pending_publish_.size()},
                   {"dropped_windows", dropped_windows_.exchange(0)}});
    }

    void flush_pending_on_shutdown() noexcept {
        try {
            for (;;) {
                const size_t before = pending_store_.size();
                flush_pending(timeutil::now_ms(), true);
                if (pending_store_.empty() || pending_store_.size() == before) break;
            }
            flush_publications(timeutil::now_ms(), true);
        } catch (const std::exception& error) {
            log::warn("micro_shutdown_flush_exception", {{"message", error.what()}});
        } catch (...) {
            log::warn("micro_shutdown_flush_exception",
                      {{"message", "non-standard exception"}});
        }
    }

private:
    Result<int64_t> ensure_set(bool ofi = false) {
        const std::string name = ofi ? "book_ofi_receipt" : kMicroSetName;
        const std::string profile = ofi ? "sequence_validated_delta" : kMicroProfile;
        const std::string engine = ofi ? "alttech-ofi-1" : kMicroEngineVersion;
        const std::string& hash = ofi ? ofi_schema_hash_ : schema_hash_;
        int64_t id = 0;
        std::string existing_hash;
        auto result = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT id, schema_hash FROM alttech.feature_set_versions "
                "WHERE name = $1 AND version = $2",
                name, kMicroSetVersion);
            if (!rows.empty()) {
                id = rows[0][0].as<int64_t>();
                existing_hash = rows[0][1].as<std::string>();
                return;
            }

            const auto inserted = txn.exec_params(
                "INSERT INTO alttech.feature_set_versions "
                "(name, version, scope, profile, engine_version, schema_hash, frozen) "
                "VALUES ($1, $2, 'VENUE', $3, $4, $5, true) RETURNING id",
                name, kMicroSetVersion, profile, engine, hash);
            id = inserted[0][0].as<int64_t>();
            const auto& names = ofi ? ofi_feature_names() : micro_feature_names();
            for (size_t index = 0; index < names.size(); ++index) {
                const auto definition = txn.exec_params(
                    "INSERT INTO alttech.feature_definitions (name, family, scope) "
                    "VALUES ($1, 'microstructure', 'VENUE') "
                    "ON CONFLICT (name) DO UPDATE SET family = EXCLUDED.family RETURNING id",
                    names[index]);
                const auto parameters = txn.exec_params(
                    "INSERT INTO alttech.feature_parameter_sets (definition_id, parameters) "
                    "VALUES ($1, '{}') ON CONFLICT (definition_id, parameters) "
                    "DO UPDATE SET parameters = EXCLUDED.parameters RETURNING id",
                    definition[0][0].as<int64_t>());
                txn.exec_params(
                    "INSERT INTO alttech.feature_set_members "
                    "(feature_set_version_id, position, parameter_set_id, output_name) "
                    "VALUES ($1, $2, $3, $4)",
                    id, static_cast<int>(index), parameters[0][0].as<int64_t>(),
                    names[index]);
            }
        });
        if (!result) return std::unexpected(result.error());
        if (!existing_hash.empty() && existing_hash != hash) {
            return fail(ErrorCode::config_error,
                        "micro-trade set is frozen but its catalog changed");
        }
        return id;
    }

    MicroVector compute(InstrumentState& state, const WindowSpec& spec,
                        int64_t window_start, std::vector<MicroTrade>& trades,
                        std::vector<BookSample>& books, WindowAccum& accum) {
        const auto& names = micro_feature_names();
        MicroVector vector;
        vector.symbol = state.symbol;
        vector.instrument_id = state.instrument_id;
        vector.window_label = spec.label;
        vector.span_ms = spec.span_ms;
        vector.window_start_ms = window_start;
        vector.values.assign(names.size(), 0.0F);
        vector.validity.assign((names.size() + 7U) / 8U, 0);

        size_t index = 0;
        const auto put = [&](double value, bool valid) {
            if (valid && std::isfinite(value)) {
                vector.values[index] = static_cast<float>(value);
                vector.validity[index / 8U] |=
                    static_cast<uint8_t>(1U << (index % 8U));
            }
            ++index;
        };

        const double seconds = static_cast<double>(spec.span_ms) / 1000.0;
        const size_t count = trades.size();
        double buy_volume = 0;
        double sell_volume = 0;
        double volume = 0;
        double notional = 0;
        double max_size = 0;
        size_t buys = 0;
        for (const auto& trade : trades) {
            volume += trade.size;
            notional += trade.size * trade.price;
            max_size = std::max(max_size, trade.size);
            if (trade.is_buy) {
                buy_volume += trade.size;
                ++buys;
            } else {
                sell_volume += trade.size;
            }
        }
        const double delta = buy_volume - sell_volume;
        if (spec.span_ms == 60'000) state.cvd += delta;

        double median = 0;
        if (count > 0) {
            std::vector<double> sizes;
            sizes.reserve(count);
            for (const auto& trade : trades) sizes.push_back(trade.size);
            std::nth_element(sizes.begin(), sizes.begin() + static_cast<long>(count / 2U),
                             sizes.end());
            median = sizes[count / 2U];
        }
        size_t large = 0;
        if (median > 0) {
            for (const auto& trade : trades) {
                if (trade.size > kLargeTradeMedianMultiple * median) ++large;
            }
        }
        const double vwap = volume > 0 ? notional / volume : 0;

        // An ephemeral stream with no event does not prove a healthy zero.
        // Until a source coverage certificate is present, absence stays NA.
        put(static_cast<double>(count), count > 0);
        put(static_cast<double>(count) / seconds, count > 0);
        put(buy_volume, count > 0);
        put(sell_volume, count > 0);
        put(volume > 0 ? delta / volume : 0, volume > 0);
        put(delta, count > 0);
        put(delta, count > 0);
        put(state.cvd, count > 0);
        put(count > 0 ? volume / static_cast<double>(count) : 0, count > 0);
        put(median, count > 0);
        put(max_size, count > 0);
        put(count > 0 ? static_cast<double>(large) / static_cast<double>(count) : 0,
            count > 0);
        put(static_cast<double>(buys), count > 0);
        put(static_cast<double>(count - buys), count > 0);
        put(vwap, volume > 0);
        put(volume > 0 && !trades.empty() && vwap > 0
                ? (trades.back().price - vwap) / vwap
                : 0,
            volume > 0 && !trades.empty());
        const double previous_volume = accum.prev_window_volume;
        put(previous_volume > 0 ? volume / previous_volume : 0, previous_volume > 0);
        const double previous_average = accum.prev_burst_avg;
        put(previous_average > 0 ? static_cast<double>(count) / previous_average : 0,
            previous_average > 0);
        accum.prev_window_volume = volume;
        accum.prev_burst_avg =
            previous_average < 0
                ? static_cast<double>(count)
                : previous_average * (1.0 - kBurstBaselineAlpha) +
                      static_cast<double>(count) * kBurstBaselineAlpha;

        const size_t book_count = books.size();
        const bool has_book = book_count > 0;
        double spread_sum = 0;
        double imbalance_sum = 0;
        double microprice_deviation_sum = 0;
        double bid_sum = 0;
        double ask_sum = 0;
        for (const auto& book : books) {
            spread_sum += book.spread_bps;
            imbalance_sum += book.imbalance;
            microprice_deviation_sum +=
                book.mid > 0 ? (book.microprice - book.mid) / book.mid : 0;
            bid_sum += book.bid_depth;
            ask_sum += book.ask_depth;
        }
        const double divisor = has_book ? static_cast<double>(book_count) : 1.0;
        const double bid_average = bid_sum / divisor;
        const double ask_average = ask_sum / divisor;
        const double total_depth = bid_average + ask_average;
        put(spread_sum / divisor, has_book);
        put(imbalance_sum / divisor, has_book);
        put(microprice_deviation_sum / divisor, has_book);
        put(bid_average, has_book);
        put(ask_average, has_book);
        put(total_depth > 0 ? bid_average / total_depth : 0,
            has_book && total_depth > 0);
        const bool valid_mid = has_book && books.front().mid > 0;
        put(valid_mid ? books.back().mid / books.front().mid - 1.0 : 0, valid_mid);
        put(static_cast<double>(book_count), true);
        return vector;
    }

    Result<void> store(const std::vector<MicroVector>& rows) {
        if (rows.empty()) return {};
        return pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            std::string sql =
                "INSERT INTO alttech.venue_microstructure_vectors "
                "(part_class, exchange_id, canonical_pair_id, window_label, window_start, "
                "feature_set_version_id, profile, values, validity_bitmap, status, venue_quality, "
                "engine_version, computed_at) VALUES ";
            bool first = true;
            for (const auto& row : rows) {
                sql += first ? "(" : ",(";
                first = false;
                sql += txn.quote(std::string(is_reference(row.venue) ? "ref_1m" : part_class_for(row.span_ms))) + "," +
                       std::to_string(venue_id(row.venue)) + "," +
                       std::to_string(row.instrument_id) + "," +
                       txn.quote(row.window_label) + ",to_timestamp(" +
                       std::to_string(row.window_start_ms) + "::bigint / 1000.0)," +
                       std::to_string(row.ofi ? ofi_set_id_ : set_id_) + "," + txn.quote(std::string(row.ofi ? "sequence_validated_delta" : kMicroProfile)) +
                       ",'{";
                char buffer[32];
                for (size_t index = 0; index < row.values.size(); ++index) {
                    if (index != 0) sql += ',';
                    std::snprintf(buffer, sizeof(buffer), "%.9g",
                                  static_cast<double>(row.values[index]));
                    sql += buffer;
                }
                sql += "}'::real[],'\\x";
                static constexpr char digits[] = "0123456789abcdef";
                for (const uint8_t byte : row.validity) {
                    sql += digits[byte >> 4U];
                    sql += digits[byte & 0x0FU];
                }
                sql += "'::bytea," + txn.quote(std::string(row.complete ? "VALID" : "INCOMPLETE_INPUT")) + "," +
                       txn.quote(std::string(row.complete ? "VALID" : "DESYNCED")) + "," +
                       txn.quote(std::string(row.ofi ? "alttech-ofi-1" : kMicroEngineVersion)) + ",to_timestamp(" +
                       std::to_string(row.computed_at_ms) + "::bigint / 1000.0))";
            }
            sql +=
                " ON CONFLICT (part_class, exchange_id, canonical_pair_id, window_label, "
                "window_start, feature_set_version_id) DO UPDATE SET values = EXCLUDED.values, "
                "validity_bitmap = EXCLUDED.validity_bitmap, status = EXCLUDED.status, "
                "venue_quality = EXCLUDED.venue_quality, "
                "engine_version = EXCLUDED.engine_version, "
                "computed_at = EXCLUDED.computed_at";
            txn.exec(sql);
            for (const auto& row : rows) {
                const auto envelope = event_for(row);
                persistence::enqueue_analytics(txn, envelope.message_id(),
                    messaging::topology::venue_microfeature_subject(row.venue, row.symbol, row.window_label),
                    messaging::serialize(envelope));
            }
        });
    }

    ::alttech::v1::Envelope event_for(const MicroVector& vector) {
        messaging::EnvelopeMeta meta;
        meta.event_type = "microstructure.vector";
        meta.venue = vector.venue;
        meta.instrument_id = vector.instrument_id;
        meta.exchange_symbol = vector.symbol;
        meta.exchange_timestamp_ms = vector.window_start_ms;
        meta.source_service = "alttech-service";
        meta.message_id = "mf:" + vector.symbol + ":" + vector.window_label + ":" +
                          std::to_string(vector.window_start_ms);
        auto envelope = messaging::make_envelope(meta);
        envelope.set_quality_state(vector.complete ? ::alttech::v1::QUALITY_STATE_VALID : ::alttech::v1::QUALITY_STATE_DESYNCED);
        if (vector.ofi) {
            envelope.set_canonical_pair_id(vector.instrument_id);
            envelope.set_message_id("ofi:" + std::string(venue_code(vector.venue)) + ":" +
                std::to_string(vector.instrument_id) + ":" + vector.window_label + ":" + std::to_string(vector.window_start_ms));
            auto* payload = envelope.mutable_venue_microstructure_vector();
            payload->set_scope(::alttech::v1::ANALYTICS_SCOPE_VENUE);
            payload->set_exchange(envelope.exchange());
            payload->set_canonical_pair_id(vector.instrument_id);
            payload->set_instrument_id(vector.instrument_id);
            payload->set_window_label(vector.window_label);
            payload->set_window_start_ms(vector.window_start_ms);
            payload->set_profile("sequence_validated_delta");
            payload->set_feature_set_name("book_ofi_receipt");
            payload->set_feature_set_version(1);
            payload->set_schema_hash(ofi_schema_hash_);
            payload->set_engine_version("alttech-ofi-1");
            for (float value : vector.values) payload->add_values(value);
            payload->set_validity_bitmap(std::string(vector.validity.begin(), vector.validity.end()));
            payload->set_status(vector.complete ? ::alttech::v1::ANALYTICS_VECTOR_STATUS_VALID : ::alttech::v1::ANALYTICS_VECTOR_STATUS_INCOMPLETE_INPUT);
            payload->set_venue_quality(envelope.quality_state());
            payload->set_computed_at_ms(vector.computed_at_ms);
            return envelope;
        }
        auto* payload = envelope.mutable_microstructure_vector();
        payload->set_window_label(vector.window_label);
        payload->set_window_start_ms(vector.window_start_ms);
        payload->set_feature_set_name(kMicroSetName);
        payload->set_feature_set_version(kMicroSetVersion);
        payload->set_schema_hash(schema_hash_);
        payload->set_engine_version(kMicroEngineVersion);
        for (const float value : vector.values) payload->add_values(value);
        payload->set_validity_bitmap(
            std::string(vector.validity.begin(), vector.validity.end()));
        payload->set_status(::alttech::v1::MICROSTRUCTURE_STATUS_VALID);
        payload->set_computed_at_ms(vector.computed_at_ms);

        return envelope;
    }

    bool publish(const MicroVector& vector) noexcept {
        try {
            const auto envelope = event_for(vector);
            return static_cast<bool>(nats_.js_publish(messaging::topology::venue_microfeature_subject(
                vector.venue, vector.symbol, vector.window_label), messaging::serialize(envelope), envelope.message_id(), 500));
        } catch (const std::exception& error) {
            log::warn("microfeature_publish_exception", {{"message", error.what()}});
            return false;
        } catch (...) {
            log::warn("microfeature_publish_exception", {{"message", "non-standard exception"}});
            return false;
        }
    }

    void flush_pending(int64_t now_ms, bool force) {
        if (!pending_store_.empty() && (force || now_ms >= next_store_retry_ms_)) {
            const size_t count = std::min(store_batch_size_, pending_store_.size());
            std::vector<MicroVector> batch;
            batch.reserve(count);
            auto iterator = pending_store_.begin();
            for (size_t index = 0; index < count; ++index, ++iterator) {
                batch.push_back(*iterator);
            }

            auto stored = store(batch);
            if (!stored) {
                next_store_retry_ms_ = now_ms + store_backoff_.next().count();
                log::error("micro_store_failed",
                           {{"message", stored.error().message},
                            {"pending", pending_store_.size()},
                            {"retry_at_ms", next_store_retry_ms_}});
            } else {
                store_backoff_.reset();
                next_store_retry_ms_ = 0;
                for (size_t index = 0; index < count; ++index) {
                    pending_store_.pop_front();
                }
                // Durable outbox owns publication and restart replay for committed rows.
            }
        }
        flush_publications(now_ms, force);
        if (auto drained = persistence::drain_analytics_outbox(pool_, nats_); !drained) {
            log::warn("micro_outbox_drain_failed", {{"message", drained.error().message}});
        }
    }

    void flush_publications(int64_t now_ms, bool force) {
        if (pending_publish_.empty() || (!force && now_ms < next_publish_retry_ms_)) return;
        int64_t count = 0;
        while (!pending_publish_.empty()) {
            if (!publish(pending_publish_.front())) {
                next_publish_retry_ms_ = now_ms + publish_backoff_.next().count();
                break;
            }
            pending_publish_.pop_front();
            ++count;
        }
        if (pending_publish_.empty()) {
            publish_backoff_.reset();
            next_publish_retry_ms_ = 0;
        }
        published_.fetch_add(count, std::memory_order_relaxed);
    }

    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
    std::string schema_hash_;
    std::string ofi_schema_hash_;
    int64_t set_id_ = 0;
    int64_t ofi_set_id_ = 0;
    struct OfiState { std::string symbol; ofi::Windows windows; };
    std::map<std::pair<int32_t, int64_t>, OfiState> ofi_states_;

    std::mutex mu_;
    std::vector<std::unique_ptr<InstrumentState>> states_;
    std::map<std::string, InstrumentState*> by_symbol_;
    std::map<int64_t, InstrumentState*> by_iid_;
    std::map<int64_t, double> cvd_restore_;

    size_t book_page_size_ = 50'000;
    size_t book_max_pages_ = 4;
    int64_t book_overlap_ms_ = 2'000;
    bool book_poll_started_ = false;
    int64_t book_highwater_ms_ = 0;
    int64_t book_highwater_us_ = 0;
    int64_t book_highwater_iid_ = 0;
    std::string book_highwater_time_;
    std::set<std::pair<int64_t, int64_t>> seen_book_keys_;

    size_t store_batch_size_ = 2'000;
    std::set<std::string> persist_windows_;
    std::deque<MicroVector> pending_store_;
    std::deque<MicroVector> pending_publish_;
    Backoff store_backoff_{std::chrono::milliseconds(1'000),
                           std::chrono::milliseconds(30'000)};
    Backoff publish_backoff_{std::chrono::milliseconds(1'000),
                             std::chrono::milliseconds(30'000)};
    int64_t next_store_retry_ms_ = 0;
    int64_t next_publish_retry_ms_ = 0;
    std::atomic<int64_t> published_{0};
    std::atomic<int64_t> books_polled_{0};
    std::atomic<int64_t> dropped_windows_{0};
};

} // namespace

namespace alttech::modules {

Result<void> run_microstructure(const YAML::Node& service_config,
                                persistence::PgPool& pool,
                                messaging::NatsClient& nats,
                                application::ModuleSupervisor& supervisor) {
    const auto fail_module = [&](const Error& error) -> Result<void> {
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(error);
    };

    const int64_t page_size =
        config::opt_int(service_config, "book_poll_page_size", 50'000);
    const int64_t max_pages =
        config::opt_int(service_config, "book_poll_max_pages_per_tick", 4);
    const int64_t overlap_ms =
        config::opt_int(service_config, "book_poll_overlap_ms", 2'000);
    const int64_t store_batch =
        config::opt_int(service_config, "store_batch_size", 2'000);
    const int64_t tick_ms = config::opt_int(service_config, "tick_interval_ms", 500);
    if (page_size < 1 || page_size > 50'000 || max_pages < 1 || max_pages > 64 ||
        overlap_ms < 0 || overlap_ms > 60'000 || store_batch < 1 ||
        store_batch > 10'000 || tick_ms < 100 || tick_ms > 5'000) {
        const Error error{ErrorCode::config_error,
                          "invalid microstructure pagination, batch, overlap, or tick limits"};
        log::error("invalid_microstructure_module_config",
                   {{"book_poll_page_size", page_size},
                    {"book_poll_max_pages_per_tick", max_pages},
                    {"book_poll_overlap_ms", overlap_ms},
                    {"store_batch_size", store_batch},
                    {"tick_interval_ms", tick_ms}});
        return fail_module(error);
    }

    // Reference-venue 1m microstructure sweep. Declared outside the try so it is
    // joined after the catch has requested global shutdown.
    venuelite::SweepThreadJoiner venue_join;

    // Fail closed on a missing schema.
    if (auto probe = pool.with_txn([](pqxx::work& txn) {
            const auto row = txn.exec1(
                "SELECT to_regclass('global_observation_universe') IS NOT NULL "
                "AND to_regclass('canonical_pair_exchange_map') IS NOT NULL "
                "AND to_regclass('exchange_instruments') IS NOT NULL "
                "AND to_regclass('market_orderbook_snapshots') IS NOT NULL "
                "AND to_regclass('alttech.feature_set_versions') IS NOT NULL "
                "AND to_regclass('alttech.venue_microstructure_vectors') IS NOT NULL "
                "AND to_regclass('alttech.module_checkpoints') IS NOT NULL "
                "AND to_regclass('alttech.analytics_outbox') IS NOT NULL");
            if (!row[0].as<bool>()) {
                throw std::runtime_error(
                    "required market-data or alttech microstructure schema is missing");
            }
        });
        !probe) {
        log::critical("postgres_schema_unavailable", {{"message", probe.error().message}});
        return fail_module(probe.error());
    }

    try {
        MicroService service(service_config, pool, nats);
        if (auto initialized = service.init(); !initialized) {
            log::error("micro_init_failed",
                       {{"message", initialized.error().message}});
            return fail_module(initialized.error());
        }

        // Keep this RAII object alive for the complete module lifetime. It is
        // destroyed before `service`, so the callback cannot outlive its target.
        auto subscription = nats.subscribe(
            "md.bitget.*.trade", [&](messaging::Msg message) noexcept {
                try {
                    auto envelope = messaging::parse_envelope(message.data());
                    if (!envelope) {
                        log::warn("micro_trade_envelope_rejected",
                                  {{"subject", std::string(message.subject())},
                                   {"message", envelope.error().message}});
                        return;
                    }
                    if (!envelope->has_trade()) {
                        log::warn("micro_trade_payload_rejected",
                                  {{"subject", std::string(message.subject())},
                                   {"message", "Envelope does not contain Trade"}});
                        return;
                    }
                    if (auto valid =
                            validate_wire_decimal(envelope->trade().price(), "trade.price");
                        !valid) {
                        log::warn("micro_trade_payload_rejected",
                                  {{"symbol", envelope->exchange_symbol()},
                                   {"message", valid.error().message}});
                        return;
                    }
                    if (auto valid =
                            validate_wire_decimal(envelope->trade().size(), "trade.size");
                        !valid) {
                        log::warn("micro_trade_payload_rejected",
                                  {{"symbol", envelope->exchange_symbol()},
                                   {"message", valid.error().message}});
                        return;
                    }

                    auto trade = messaging::trade_from_proto(envelope->trade());
                    if (!trade) {
                        log::warn("micro_trade_payload_rejected",
                                  {{"symbol", envelope->exchange_symbol()},
                                   {"message", trade.error().message}});
                        return;
                    }
                    if (auto accepted =
                            service.on_trade(envelope->exchange_symbol(), *trade);
                        !accepted) {
                        log::warn("micro_trade_payload_rejected",
                                  {{"symbol", envelope->exchange_symbol()},
                                   {"message", accepted.error().message}});
                    }
                } catch (const std::exception& error) {
                    log::warn("micro_trade_callback_exception",
                              {{"message", error.what()}});
                } catch (...) {
                    log::warn("micro_trade_callback_exception",
                              {{"message", "non-standard exception"}});
                }
            });
        if (!subscription) {
            log::error("trade_subscribe_failed",
                       {{"message", subscription.error().message}});
            return fail_module(subscription.error());
        }

        auto book_subscription = nats.subscribe("md.*.*.book.delta", [&](messaging::Msg message) noexcept {
            try {
                auto envelope = messaging::parse_envelope(message.data());
                if (!envelope) return;
                if (auto accepted = service.on_book_delta(*envelope); !accepted) {
                    log::warn("ofi_delta_rejected", {{"message", accepted.error().message}});
                }
            } catch (const std::exception& error) {
                log::warn("ofi_callback_exception", {{"message", error.what()}});
            } catch (...) {
                log::warn("ofi_callback_exception", {{"message", "non-standard exception"}});
            }
        });
        if (!book_subscription) return fail_module(book_subscription.error());

        {
            std::string persisted;
            for (const auto& window : service.persisted_windows()) {
                if (!persisted.empty()) persisted += ',';
                persisted += window;
            }
            log::info("micro_module_ready", {{"persisted_windows", persisted}});
        }
        supervisor.mark_ready(kModuleName);
        venue_join.thread = std::thread(venuelite::run_venue_sweep, std::ref(pool),
                                        std::ref(nats), venuelite::store::VenueOutput::microstructure,
                                        service_config);

        auto& shutdown = ShutdownToken::instance();
        int64_t last_stats = timeutil::now_ms();
        int64_t last_checkpoint = last_stats;
        while (!shutdown.wait_for(std::chrono::milliseconds(tick_ms))) {
            supervisor.heartbeat(kModuleName, 300'000);
            const int64_t now = timeutil::now_ms();
            service.tick(now);
            if (now - last_checkpoint >= 60'000) {
                service.checkpoint_cvd();
                last_checkpoint = now;
            }
            if (now - last_stats >= 60'000) {
                service.log_stats();
                last_stats = now;
            }
        }

        service.flush_pending_on_shutdown();
        service.checkpoint_cvd();
        log::info("microstructure_module_stopped", {});
        return {};
    } catch (const std::exception& exception) {
        const Error error{ErrorCode::internal_error,
                          std::string("microstructure module exception: ") + exception.what()};
        return fail_module(error);
    } catch (...) {
        const Error error{ErrorCode::internal_error,
                          "microstructure module raised a non-standard exception"};
        return fail_module(error);
    }
}

} // namespace alttech::modules
