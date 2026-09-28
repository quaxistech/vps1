#include "macro_compute.hpp"

#include "macro_availability.hpp"
#include "macro_features.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace alttech::modules::macro {
namespace {

constexpr int64_t kMinute = 60'000;
constexpr int64_t kHour = 3'600'000;
constexpr int64_t kDay = 86'400'000;

// Index a 1m series by timestamp so a lookback is a hash probe rather than a
// scan. The series are 400k points each and there are six lookbacks per minute.
using MinuteIndex = std::unordered_map<int64_t, size_t>;

MinuteIndex index_minutes(const std::vector<IntradayPoint>& s) {
    MinuteIndex idx;
    idx.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) idx.emplace(s[i].t_ms, i);
    return idx;
}

const IntradayPoint* at(const std::vector<IntradayPoint>& s, const MinuteIndex& idx, int64_t t) {
    const auto it = idx.find(t);
    return it == idx.end() ? nullptr : &s[it->second];
}

// A return is published only when BOTH ends are real quotes. Across a weekend
// the feed repeats Friday's price at zero volume, so a "1-day return" spanning
// it would be exactly zero -- a number that looks like a measured calm market
// and is nothing of the kind.
std::optional<double> log_return(const IntradayPoint* now, const IntradayPoint* then) {
    if (now == nullptr || then == nullptr) return std::nullopt;
    if (!now->traded || !then->traded) return std::nullopt;
    if (now->mid <= 0.0 || then->mid <= 0.0) return std::nullopt;
    return std::log(now->mid / then->mid);
}

// Walks a daily series forward, holding the newest observation whose publication
// lag has elapsed. One pointer per series, advanced as the grid advances.
class DailyCursor {
public:
    DailyCursor(const std::vector<DailyPoint>& points, std::string_view code)
        : points_(points), code_(code) {}

    void advance_to(int64_t t0_ms) {
        while (next_ < points_.size() &&
               available_at_ms(code_, points_[next_].observation_start_ms) <= t0_ms) {
            previous_ = current_;
            current_ = next_;
            ++next_;
        }
    }

    [[nodiscard]] bool has_value() const { return current_ != kNone; }
    [[nodiscard]] double value() const { return points_[current_].value; }
    [[nodiscard]] bool has_previous() const { return previous_ != kNone; }
    [[nodiscard]] double previous() const { return points_[previous_].value; }

    // How long ago the newest usable observation became usable. Published as a
    // feature so a consumer can see how old the daily side of a spread is
    // instead of assuming it is current.
    [[nodiscard]] double age_hours(int64_t t0_ms) const {
        if (!has_value()) return 0.0;
        const int64_t usable_at = available_at_ms(code_, points_[current_].observation_start_ms);
        return static_cast<double>(t0_ms - usable_at) / static_cast<double>(kHour);
    }

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);
    const std::vector<DailyPoint>& points_;
    std::string_view code_;
    size_t next_ = 0;
    size_t current_ = kNone;
    size_t previous_ = kNone;
};

// Rolling standard deviation of 1m log returns over a fixed stretch of TIME.
//
// Only real (traded-to-traded) returns enter it, so a weekend does not deflate
// volatility toward zero. The window used to be a count of returns, and that
// made a closed market look open: with no new returns arriving, the buffer
// kept Friday's last sixty minutes and published them as the "1h realized
// volatility" through the whole weekend -- a measurement of a market that was
// not trading, marked valid. The window is now the last `window` MINUTES of
// clock time; returns older than that fall out whether or not anything new
// arrived, so a closed market drains the window and the value goes absent.
class RollingVol {
public:
    explicit RollingVol(size_t window_minutes) : window_(window_minutes) {}

    // One call per minute of the grid, traded or not: the call is what moves
    // the window forward in time.
    void push(int64_t t_ms, std::optional<double> r) {
        if (r.has_value()) {
            buffer_.emplace_back(t_ms, *r);
            sum_ += *r;
            sum_sq_ += *r * *r;
        }
        const int64_t horizon = t_ms - static_cast<int64_t>(window_) * kMinute;
        while (!buffer_.empty() && buffer_.front().first <= horizon) {
            const double old = buffer_.front().second;
            buffer_.pop_front();
            sum_ -= old;
            sum_sq_ -= old * old;
        }
    }

    // Needs most of the window before it means anything; a stddev over three
    // points is noise wearing the name of a measurement. "Most of the window"
    // is now most of the window's MINUTES, so a market that traded for ten of
    // the last sixty minutes does not publish a one-hour volatility.
    [[nodiscard]] std::optional<double> value() const {
        if (buffer_.size() < window_ / 2 || buffer_.size() < 2) return std::nullopt;
        const double n = static_cast<double>(buffer_.size());
        const double mean = sum_ / n;
        const double var = std::max(0.0, sum_sq_ / n - mean * mean);
        return std::sqrt(var);
    }

private:
    size_t window_;
    std::deque<std::pair<int64_t, double>> buffer_;   // (minute, return)
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
};

// Rolling z-score over the last N daily observations of a quantity.
class RollingZ {
public:
    explicit RollingZ(size_t window) : window_(window) {}

    void observe(double v) {
        buffer_.push_back(v);
        sum_ += v;
        sum_sq_ += v * v;
        while (buffer_.size() > window_) {
            const double old = buffer_.front();
            buffer_.pop_front();
            sum_ -= old;
            sum_sq_ -= old * old;
        }
    }

    [[nodiscard]] std::optional<double> z(double v) const {
        if (buffer_.size() < window_ / 2 || buffer_.size() < 3) return std::nullopt;
        const double n = static_cast<double>(buffer_.size());
        const double mean = sum_ / n;
        const double var = std::max(0.0, sum_sq_ / n - mean * mean);
        const double sd = std::sqrt(var);
        if (sd <= 0.0) return std::nullopt;
        return (v - mean) / sd;
    }

private:
    size_t window_;
    std::deque<double> buffer_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
};

// Writes a value and marks it usable. A feature left untouched keeps its zero
// and a clear bit, which is what says "do not read this".
struct Writer {
    MacroVector& v;
    size_t pos = 0;

    void skip() { ++pos; }

    void put(std::optional<double> value) {
        if (value.has_value() && std::isfinite(*value)) {
            v.values[pos] = static_cast<float>(*value);
            v.validity[pos / 8] |= static_cast<uint8_t>(1u << (pos % 8));
        }
        ++pos;
    }
};

} // namespace

std::vector<MacroVector> compute_macro_series(const MacroInputs& in, int64_t from_ms,
                                              int64_t to_ms) {
    const size_t n_features = macro_feature_count();
    const size_t validity_bytes = (n_features + 7) / 8;

    const MinuteIndex cnh_idx = index_minutes(in.usdcnh);
    const MinuteIndex eur_idx = index_minutes(in.eurusd);
    const MinuteIndex jpy_idx = index_minutes(in.usdjpy);
    const MinuteIndex xau_idx = index_minutes(in.xauusd);

    DailyCursor fixing(in.fixing, "REF.USDCNY.PBOC");
    DailyCursor h10(in.h10_cny, "REF.USDCNY.FED");
    DailyCursor gbp(in.gbpusd, "REF.GBPUSD.FED");
    DailyCursor aud(in.audusd, "REF.AUDUSD.FED");
    DailyCursor krw(in.usdkrw, "REF.USDKRW.FED");
    DailyCursor vix(in.vix, "IDX.VIX");
    DailyCursor spx(in.spx, "IDX.SPX");
    DailyCursor ndx(in.ndx, "IDX.NDX");
    DailyCursor us2y(in.us2y, "RATE.US.2Y");
    DailyCursor us10y(in.us10y, "RATE.US.10Y");
    DailyCursor us10yr(in.us10y_real, "RATE.US.10Y.REAL");

    RollingVol cnh_vol_1h(60), cnh_vol_1d(1440);
    RollingVol eur_vol_1h(60), jpy_vol_1h(60);
    RollingVol xau_vol_1h(60), xau_vol_1d(1440);

    // Daily-cadence z-scores, updated once per observation rather than once per
    // minute: feeding the same figure 1440 times would collapse the variance
    // and make every z-score enormous.
    RollingZ cnh_fix_gap_z(20);
    RollingZ vix_z(20);

    // Prime both from daily history BEFORE the window. Twenty daily
    // observations is three trading weeks; a minute loop over a live window of
    // hours cannot supply that, and without priming the z-scores never become
    // valid at all. Only observations already published by the window's start
    // are used -- the priming is as-of, like everything else here.
    {
        DailyCursor prime_fix(in.fixing, "REF.USDCNY.PBOC");
        for (const auto& d : in.cnh_daily_mid) {
            // Just after the parity for that day becomes usable here.
            const int64_t at = available_at_ms("REF.USDCNY.PBOC", d.observation_start_ms) + kHour;
            if (at >= from_ms) break;
            prime_fix.advance_to(at);
            if (prime_fix.has_value() && d.value > 0.0) {
                cnh_fix_gap_z.observe(basis_points(d.value, prime_fix.value()));
            }
        }
        for (const auto& d : in.vix) {
            if (available_at_ms("IDX.VIX", d.observation_start_ms) >= from_ms) break;
            vix_z.observe(d.value);
        }
    }
    // The gap as it stood on the previous day, for the one-day change. Taken
    // once per day: comparing a minute against the same day's earlier minute
    // would be an intraday move wearing a daily name.
    std::optional<double> prev_day_fix_gap;
    std::optional<double> today_first_fix_gap;
    int64_t last_z_day = -1;

    std::vector<MacroVector> out;
    out.reserve(static_cast<size_t>((to_ms - from_ms) / kMinute) + 1);

    // The intraday side is read as-of too. `tick` is the newest 1m bar the
    // feed could have delivered by t (see intraday_cutoff_ms); every intraday
    // quantity below is taken at `tick`, never at t. The rolling volatilities
    // are fed as the cutoff advances, one completed hour at a time, so they
    // too contain only bars that had arrived.
    int64_t vol_fed_until = intraday_cutoff_ms((from_ms / kMinute) * kMinute) - 1440 * kMinute;

    for (int64_t t = (from_ms / kMinute) * kMinute; t < to_ms; t += kMinute) {
        fixing.advance_to(t); h10.advance_to(t); gbp.advance_to(t); aud.advance_to(t);
        krw.advance_to(t); vix.advance_to(t); spx.advance_to(t); ndx.advance_to(t);
        us2y.advance_to(t); us10y.advance_to(t); us10yr.advance_to(t);

        const int64_t cutoff = intraday_cutoff_ms(t);
        const int64_t tick = cutoff - kMinute;
        for (int64_t m = vol_fed_until + kMinute; m < cutoff; m += kMinute) {
            cnh_vol_1h.push(m, log_return(at(in.usdcnh, cnh_idx, m), at(in.usdcnh, cnh_idx, m - kMinute)));
            cnh_vol_1d.push(m, log_return(at(in.usdcnh, cnh_idx, m), at(in.usdcnh, cnh_idx, m - kMinute)));
            eur_vol_1h.push(m, log_return(at(in.eurusd, eur_idx, m), at(in.eurusd, eur_idx, m - kMinute)));
            jpy_vol_1h.push(m, log_return(at(in.usdjpy, jpy_idx, m), at(in.usdjpy, jpy_idx, m - kMinute)));
            xau_vol_1h.push(m, log_return(at(in.xauusd, xau_idx, m), at(in.xauusd, xau_idx, m - kMinute)));
            xau_vol_1d.push(m, log_return(at(in.xauusd, xau_idx, m), at(in.xauusd, xau_idx, m - kMinute)));
        }
        if (cutoff - kMinute > vol_fed_until) vol_fed_until = cutoff - kMinute;

        const IntradayPoint* cnh = at(in.usdcnh, cnh_idx, tick);
        const IntradayPoint* eur = at(in.eurusd, eur_idx, tick);
        const IntradayPoint* jpy = at(in.usdjpy, jpy_idx, tick);
        const IntradayPoint* xau = at(in.xauusd, xau_idx, tick);

        // The offshore-versus-fix gap, in basis points. Both are CNY/CNH per
        // USD, so the difference is meaningful; they are different series and
        // this is the measurement of how different.
        std::optional<double> fix_gap;
        if (cnh != nullptr && cnh->mid > 0.0 && fixing.has_value()) {
            fix_gap = basis_points(cnh->mid, fixing.value());
        }

        const int64_t day = t / kDay;
        if (day != last_z_day) {
            // Roll the day over BEFORE anything reads the daily quantities, so
            // "yesterday" means yesterday and not a few minutes ago.
            prev_day_fix_gap = today_first_fix_gap;
            today_first_fix_gap.reset();
            if (fix_gap.has_value()) cnh_fix_gap_z.observe(*fix_gap);
            if (vix.has_value()) vix_z.observe(vix.value());
            last_z_day = day;
        }
        if (!today_first_fix_gap.has_value() && fix_gap.has_value()) {
            today_first_fix_gap = fix_gap;
        }

        MacroVector v;
        v.t_ms = t;
        v.values.assign(n_features, 0.0f);
        v.validity.assign(validity_bytes, 0);
        Writer w{v};

        // --- China ----------------------------------------------------------
        w.put(log_return(cnh, at(in.usdcnh, cnh_idx, tick - 5 * kMinute)));
        w.put(log_return(cnh, at(in.usdcnh, cnh_idx, tick - kHour)));
        w.put(log_return(cnh, at(in.usdcnh, cnh_idx, tick - kDay)));
        w.put(cnh_vol_1h.value());
        w.put(cnh_vol_1d.value());
        w.put(cnh != nullptr && cnh->traded ? std::optional<double>(cnh->spread_bps)
                                            : std::nullopt);
        w.put(fix_gap);
        w.put(fix_gap.has_value() && prev_day_fix_gap.has_value()
                  ? std::optional<double>(*fix_gap - *prev_day_fix_gap)
                  : std::nullopt);
        w.put(fix_gap.has_value() ? cnh_fix_gap_z.z(*fix_gap) : std::nullopt);
        w.put(cnh != nullptr && cnh->mid > 0.0 && h10.has_value()
                  ? std::optional<double>(basis_points(cnh->mid, h10.value()))
                  : std::nullopt);
        w.put(fixing.has_value() && h10.has_value()
                  ? std::optional<double>(basis_points(fixing.value(), h10.value()))
                  : std::nullopt);
        w.put(fixing.has_value() && fixing.has_previous()
                  ? std::optional<double>(basis_points(fixing.value(), fixing.previous()))
                  : std::nullopt);
        w.put(fixing.has_value() ? std::optional<double>(fixing.age_hours(t)) : std::nullopt);

        // --- FX and metals ---------------------------------------------------
        w.put(log_return(eur, at(in.eurusd, eur_idx, tick - kHour)));
        w.put(log_return(eur, at(in.eurusd, eur_idx, tick - kDay)));
        w.put(eur_vol_1h.value());
        w.put(log_return(jpy, at(in.usdjpy, jpy_idx, tick - kHour)));
        w.put(log_return(jpy, at(in.usdjpy, jpy_idx, tick - kDay)));
        w.put(jpy_vol_1h.value());
        w.put(log_return(xau, at(in.xauusd, xau_idx, tick - kHour)));
        w.put(log_return(xau, at(in.xauusd, xau_idx, tick - kDay)));
        w.put(xau_vol_1h.value());
        w.put(xau_vol_1d.value());

        // --- daily-only FX: one change per observation, no invented intraday --
        auto daily_ret = [](const DailyCursor& c) -> std::optional<double> {
            if (!c.has_value() || !c.has_previous() || c.previous() <= 0.0) return std::nullopt;
            return std::log(c.value() / c.previous());
        };
        w.put(daily_ret(gbp));
        w.put(daily_ret(aud));
        w.put(daily_ret(krw));
        w.put(krw.has_value() ? std::optional<double>(krw.age_hours(t)) : std::nullopt);

        // --- equity and volatility -------------------------------------------
        w.put(vix.has_value() ? std::optional<double>(vix.value()) : std::nullopt);
        w.put(vix.has_value() && vix.has_previous()
                  ? std::optional<double>(vix.value() - vix.previous())
                  : std::nullopt);
        w.put(vix.has_value() ? vix_z.z(vix.value()) : std::nullopt);
        w.put(daily_ret(spx));
        w.put(daily_ret(ndx));
        // The age of the block is the age of its OLDEST input. It was VIX's age
        // alone, while spx_ret_1d and ndx_ret_1d are separate FRED series whose
        // newest usable close can be a different day: a consumer reading this
        // column to judge how current the index returns are was told about a
        // different series. Absent when none of the three is usable.
        {
            std::optional<double> oldest;
            for (const DailyCursor* c : {&vix, &spx, &ndx}) {
                if (!c->has_value()) continue;
                const double age = c->age_hours(t);
                if (!oldest.has_value() || age > *oldest) oldest = age;
            }
            w.put(oldest);
        }

        // --- rates ------------------------------------------------------------
        w.put(us2y.has_value() ? std::optional<double>(us2y.value()) : std::nullopt);
        w.put(us10y.has_value() ? std::optional<double>(us10y.value()) : std::nullopt);
        w.put(us10y.has_value() && us10y.has_previous()
                  ? std::optional<double>(us10y.value() - us10y.previous())
                  : std::nullopt);
        w.put(us10yr.has_value() ? std::optional<double>(us10yr.value()) : std::nullopt);
        w.put(us2y.has_value() && us10y.has_value()
                  ? std::optional<double>(us10y.value() - us2y.value())
                  : std::nullopt);
        w.put(us10y.has_value() && us10yr.has_value()
                  ? std::optional<double>(us10y.value() - us10yr.value())
                  : std::nullopt);
        w.put(us10y.has_value() ? std::optional<double>(us10y.age_hours(t)) : std::nullopt);

        size_t set_bits = 0;
        for (size_t i = 0; i < n_features; ++i) {
            if (v.is_valid(i)) ++set_bits;
        }
        // A vector with nothing usable is WARMING rather than valid: early in
        // the window the lookbacks and the publication lags have not caught up.
        v.status = set_bits == 0            ? VectorStatus::warming
                   : set_bits < n_features  ? VectorStatus::incomplete_input
                                            : VectorStatus::valid;
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace alttech::modules::macro
