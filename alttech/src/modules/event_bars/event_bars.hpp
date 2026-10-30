#pragma once

// DT-03 (2026-10-30): causal alternative bars from closed OHLCV bars and real
// traded flow. Pure: no clock, no I/O, no state outside the call. Every
// generator reads only bars that are closed at the moment it emits, so a bar
// it returns was knowable when its source bar closed.
//
//   heikin_ashi   recursive: HA_open(t) = (HA_open(t-1) + HA_close(t-1)) / 2,
//                 HA_close(t) = (O + H + L + C) / 4 of the closed source bar.
//   renko         fixed brick size on the close; a reversal needs two bricks.
//   range         a bar closes when high - low of its accumulated sources
//                 reaches the range.
//   volume / dollar   a bar closes when the cumulative base / quote volume of
//                 closed source bars reaches the threshold.
//   kagi          a line reverses when the close moves against it by the
//                 reversal amount.
//   point_figure  boxes of `box`, a column reverses after `reversal` boxes.
//
// What OHLC cannot say is not invented: within one source bar the order of
// high and low is unknown, so renko and range advance on the CLOSE only and
// never fill a brick at a price the bar did not close at.

#include <cmath>
#include <cstdint>
#include <vector>

namespace alttech::event_bars {

struct SourceBar {
    int64_t close_ms = 0;          // when the bar closed (its knowable time)
    double open = 0, high = 0, low = 0, close = 0, volume = 0, quote = 0;
};

struct EventBar {
    int64_t close_ms = 0;          // the source bar whose close completed it
    double open = 0, high = 0, low = 0, close = 0, volume = 0, quote = 0;
    int direction = 0;             // +1 up, -1 down, 0 none (renko/kagi/p&f)
};

[[nodiscard]] inline bool finite_bar(const SourceBar& b) {
    return std::isfinite(b.open) && std::isfinite(b.high) && std::isfinite(b.low) && std::isfinite(b.close) &&
           b.high >= b.low && b.close > 0.0;
}

[[nodiscard]] inline std::vector<EventBar> heikin_ashi(const std::vector<SourceBar>& bars) {
    std::vector<EventBar> out;
    out.reserve(bars.size());
    for (const auto& b : bars) {
        if (!finite_bar(b)) { out.clear(); continue; }   // a gap restarts the recursion
        EventBar h;
        h.close_ms = b.close_ms;
        h.close = (b.open + b.high + b.low + b.close) / 4.0;
        h.open = out.empty() ? (b.open + b.close) / 2.0 : (out.back().open + out.back().close) / 2.0;
        h.high = std::fmax(b.high, std::fmax(h.open, h.close));
        h.low = std::fmin(b.low, std::fmin(h.open, h.close));
        h.volume = b.volume;
        h.quote = b.quote;
        h.direction = h.close > h.open ? 1 : (h.close < h.open ? -1 : 0);
        out.push_back(h);
    }
    return out;
}

[[nodiscard]] inline std::vector<EventBar> renko(const std::vector<SourceBar>& bars, double brick) {
    std::vector<EventBar> out;
    if (!(brick > 0.0) || bars.empty()) return out;
    double anchor = bars.front().close;
    int last = 0;
    for (const auto& b : bars) {
        if (!finite_bar(b)) continue;
        for (;;) {
            const double up = anchor + (last == -1 ? 2.0 : 1.0) * brick;
            const double down = anchor - (last == 1 ? 2.0 : 1.0) * brick;
            if (b.close >= up) {
                const double from = last == -1 ? anchor + brick : anchor;
                out.push_back({b.close_ms, from, from + brick, from, from + brick, 0, 0, 1});
                anchor = from + brick;
                last = 1;
            } else if (b.close <= down) {
                const double from = last == 1 ? anchor - brick : anchor;
                out.push_back({b.close_ms, from, from, from - brick, from - brick, 0, 0, -1});
                anchor = from - brick;
                last = -1;
            } else {
                break;
            }
        }
    }
    return out;
}

[[nodiscard]] inline std::vector<EventBar> range_bars(const std::vector<SourceBar>& bars, double range) {
    std::vector<EventBar> out;
    if (!(range > 0.0)) return out;
    bool open = false;
    EventBar cur;
    for (const auto& b : bars) {
        if (!finite_bar(b)) { open = false; continue; }
        if (!open) {
            cur = {b.close_ms, b.open, b.high, b.low, b.close, 0, 0, 0};
            open = true;
        }
        cur.high = std::fmax(cur.high, b.high);
        cur.low = std::fmin(cur.low, b.low);
        cur.close = b.close;
        cur.close_ms = b.close_ms;
        cur.volume += b.volume;
        cur.quote += b.quote;
        if (cur.high - cur.low >= range) {
            cur.direction = cur.close > cur.open ? 1 : (cur.close < cur.open ? -1 : 0);
            out.push_back(cur);
            open = false;
        }
    }
    return out;
}

// by_quote: dollar bars (quote volume) instead of volume bars (base volume).
[[nodiscard]] inline std::vector<EventBar> flow_bars(const std::vector<SourceBar>& bars, double threshold,
                                                     bool by_quote) {
    std::vector<EventBar> out;
    if (!(threshold > 0.0)) return out;
    bool open = false;
    EventBar cur;
    for (const auto& b : bars) {
        if (!finite_bar(b) || !std::isfinite(b.volume) || !std::isfinite(b.quote) || b.volume < 0 || b.quote < 0) {
            open = false;   // flow unknown: the bar in progress is not completed with a guess
            continue;
        }
        if (!open) {
            cur = {b.close_ms, b.open, b.high, b.low, b.close, 0, 0, 0};
            open = true;
        }
        cur.high = std::fmax(cur.high, b.high);
        cur.low = std::fmin(cur.low, b.low);
        cur.close = b.close;
        cur.close_ms = b.close_ms;
        cur.volume += b.volume;
        cur.quote += b.quote;
        if ((by_quote ? cur.quote : cur.volume) >= threshold) {
            cur.direction = cur.close > cur.open ? 1 : (cur.close < cur.open ? -1 : 0);
            out.push_back(cur);
            open = false;
        }
    }
    return out;
}

[[nodiscard]] inline std::vector<EventBar> kagi(const std::vector<SourceBar>& bars, double reversal) {
    std::vector<EventBar> out;
    if (!(reversal > 0.0) || bars.empty()) return out;
    double start = bars.front().close, extreme = start;
    int dir = 0;
    for (const auto& b : bars) {
        if (!finite_bar(b)) continue;
        if (dir >= 0 && b.close > extreme) { extreme = b.close; if (dir == 0 && extreme - start >= reversal) dir = 1; }
        if (dir <= 0 && b.close < extreme) { extreme = b.close; if (dir == 0 && start - extreme >= reversal) dir = -1; }
        if (dir == 1 && extreme - b.close >= reversal) {
            out.push_back({b.close_ms, start, extreme, start, extreme, 0, 0, 1});
            start = extreme; extreme = b.close; dir = -1;
        } else if (dir == -1 && b.close - extreme >= reversal) {
            out.push_back({b.close_ms, start, start, extreme, extreme, 0, 0, -1});
            start = extreme; extreme = b.close; dir = 1;
        }
    }
    return out;
}

// One emitted bar per completed column: open = first box, close = last box.
[[nodiscard]] inline std::vector<EventBar> point_figure(const std::vector<SourceBar>& bars, double box, int reversal) {
    std::vector<EventBar> out;
    if (!(box > 0.0) || reversal < 1 || bars.empty()) return out;
    double top = std::floor(bars.front().close / box) * box, bottom = top, col_start = top;
    int dir = 0;
    for (const auto& b : bars) {
        if (!finite_bar(b)) continue;
        if (dir >= 0 && b.close >= top + box) {
            top = std::floor(b.close / box) * box;
            if (dir == 0) dir = 1;
        } else if (dir <= 0 && b.close <= bottom - box) {
            bottom = std::ceil(b.close / box) * box;
            if (dir == 0) dir = -1;
        }
        if (dir == 1 && b.close <= top - reversal * box) {
            out.push_back({b.close_ms, col_start, top, col_start, top, 0, 0, 1});
            col_start = top - box; bottom = std::ceil(b.close / box) * box; dir = -1;
        } else if (dir == -1 && b.close >= bottom + reversal * box) {
            out.push_back({b.close_ms, col_start, col_start, bottom, bottom, 0, 0, -1});
            col_start = bottom + box; top = std::floor(b.close / box) * box; dir = 1;
        }
    }
    return out;
}

} // namespace alttech::event_bars
