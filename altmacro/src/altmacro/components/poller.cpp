#include "altmacro/components/poller.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"
#include "altmacro/domain/validation.hpp"

#include <unordered_set>

namespace altmacro::components {
namespace {
constexpr int64_t kHourMs = 3'600'000;
constexpr int64_t kDayMs = 86'400'000;

domain::SanityBand band_of(const data::SourceBinding& b) {
    domain::SanityBand band;
    if (b.sanity_min && b.sanity_max) { band.min = *b.sanity_min; band.max = *b.sanity_max; }
    return band;
}
} // namespace

bool Poller::due(const std::string& key, int32_t interval_sec) {
    const int64_t now = timeutil::now_ms();
    const auto it = last_run_ms_.find(key);
    if (it != last_run_ms_.end() && now - it->second < interval_sec * 1000LL) return false;
    last_run_ms_[key] = now;
    return true;
}

Poller::Tick Poller::poll_once(const std::function<bool()>& should_stop) {
    Tick tick;
    const int64_t now = timeutil::now_ms();

    for (const auto& b : registry_.active()) {
        // Consulted at the top of every binding: this is both the shutdown
        // check and, in the running service, the watchdog heartbeat.
        if (should_stop && should_stop()) break;

        auto provider = registry_.get(b.provider_code);
        if (!provider) continue;

        const bool market = b.series_class == "MARKET";
        const std::string key = b.instrument_code + "|" + b.provider_code + "|" +
                                domain::to_string(b.price_basis);
        if (!due(key, market ? cfg_.poll.intraday_interval_sec
                             : cfg_.poll.reference_interval_sec)) {
            continue;
        }

        providers::InstrumentDescriptor desc;
        desc.native_symbol = b.native_symbol;
        desc.display_name = b.instrument_code;
        desc.price_basis = b.price_basis;
        desc.series_class = market ? providers::SeriesClass::market
                                   : providers::SeriesClass::official_reference;

        const auto band = band_of(b);

        if (market) {
            // Re-read the recent window a day at a time. The upsert makes a
            // repeat harmless, and a bar that was still forming when we last
            // saw it is replaced by its finished form.
            const int64_t from = now - cfg_.poll.intraday_lookback_hours * kHourMs;
            int64_t cursor = (from / kDayMs) * kDayMs;
            while (cursor < now) {
                if (should_stop && should_stop()) break;
                auto page = provider->fetch_historical_bars(desc, "1m",
                                                            providers::Window{cursor, now + 1},
                                                            should_stop);
                if (!page) {
                    ++tick.provider_failures;
                    auto n = store_.note_provider_attempt(b.provider_id, false,
                                                          page.error().message);
                    (void)n;
                    log::warn("poll_provider_failed",
                              {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                               {"error", page.error().message}});
                    break;
                }
                auto n = store_.note_provider_attempt(b.provider_id, true, "");
                (void)n;

                auto bars = page->bars;
                const auto report = domain::validate_bars(bars, band);
                if (!report.clean()) {
                    log::error("poll_bars_rejected",
                               {{"instrument", b.instrument_code}, {"rejected", report.rejected}});
                }
                if (!bars.empty()) {
                    auto w = store_.upsert_bars(b, "1m", bars);
                    if (w) {
                        tick.bars_written += static_cast<int64_t>(w->size());
                        // Publish exactly what changed. Most of the re-read
                        // window is already stored unchanged, and sending it
                        // again every pass would lean on the broker's
                        // deduplication window to hide the repeats.
                        if (publisher_ && !w->empty()) {
                            const std::unordered_set<int64_t> changed(w->begin(), w->end());
                            std::vector<domain::PriceBar> fresh;
                            fresh.reserve(changed.size());
                            for (const auto& bar : bars) {
                                if (changed.count(bar.event_time_ms) != 0) fresh.push_back(bar);
                            }
                            auto p = publisher_->publish_bars(b, "1m", fresh, should_stop);
                            if (p) tick.published += *p;
                            else log::warn("publish_failed",
                                           {{"instrument", b.instrument_code},
                                            {"error", p.error().message}});
                        }
                    } else {
                        log::error("poll_store_failed", {{"instrument", b.instrument_code},
                                                         {"error", w.error().message}});
                    }
                }
                if (!page->next_cursor_ms) break;
                cursor = *page->next_cursor_ms;
            }
        } else {
            const int64_t from = now - cfg_.poll.reference_lookback_days * kDayMs;
            auto page = provider->fetch_historical_reference(desc,
                                                             providers::Window{from, now + kDayMs},
                                                             should_stop);
            if (!page) {
                ++tick.provider_failures;
                auto n = store_.note_provider_attempt(b.provider_id, false, page.error().message);
                (void)n;
                log::warn("poll_provider_failed",
                          {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                           {"error", page.error().message}});
                continue;
            }
            auto n = store_.note_provider_attempt(b.provider_id, true, "");
            (void)n;

            auto values = page->values;
            const auto report = domain::validate_reference(values, band);
            if (!report.clean()) {
                log::error("poll_reference_rejected",
                           {{"instrument", b.instrument_code}, {"rejected", report.rejected}});
            }
            if (!values.empty()) {
                auto w = store_.upsert_reference(b, values);
                if (w) {
                    tick.reference_written += static_cast<int64_t>(w->size());
                    if (publisher_ && !w->empty()) {
                        const std::unordered_set<int64_t> changed(w->begin(), w->end());
                        std::vector<domain::ReferenceValue> fresh;
                        fresh.reserve(changed.size());
                        for (const auto& v : values) {
                            if (changed.count(v.event_time_ms) != 0) fresh.push_back(v);
                        }
                        auto p = publisher_->publish_reference(b, fresh, should_stop);
                        if (p) tick.published += *p;
                        else log::warn("publish_failed", {{"instrument", b.instrument_code},
                                                          {"error", p.error().message}});
                    }
                } else {
                    log::error("poll_store_failed", {{"instrument", b.instrument_code},
                                                     {"error", w.error().message}});
                }
            }
        }
    }
    return tick;
}

} // namespace altmacro::components
