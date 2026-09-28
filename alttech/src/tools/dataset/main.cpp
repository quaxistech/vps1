// alttech-dataset: historical reconstruction of talib-core vectors.
//
// Reads ONE series of raw 1m candles as CSV on stdin
//     open_time_ms,open,high,low,close,base_volume,quote_volume,ok
// (ordered, one venue and instrument), aggregates it to --tf exactly as
// AltSpotter does, runs AltTech's engine over it exactly as the backfill does,
// and writes the requested columns of every emitted bar as CSV on stdout:
//     open_time_ms,ok,status,<column>...
// An invalid value is written empty. Nothing is read from or written to any
// database: the tool is a pure function of its input, which is what makes a
// dataset built with it reproducible from a raw snapshot.
//
// It exists because training on the operational tables limits a model to
// their retention (14 days of vectors); the raw candles reach back months.

#include "dataset_logic.hpp"

#include "alttech/talib_features/feature_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

using alttech::dataset::MinuteBar;
using alttech::dataset::TfBar;
using alttech::talib_features::FBar;
using alttech::talib_features::FeatureEngine;

void usage() {
    std::cerr << "usage: alttech-dataset --tf <1m|5m|15m|30m|1h|4h|1d> --columns <file>\n"
                 "                       [--every-min N] [--from-ms T] [--to-ms T] [--chunk N]\n"
                 "  stdin : open_time_ms,open,high,low,close,base_volume,quote_volume,ok (1m, ordered)\n"
                 "  stdout: open_time_ms,ok,status,<columns> for every emitted bar\n";
}

int tf_minutes(const std::string& tf) {
    static const std::map<std::string, int> m = {{"1m", 1},   {"5m", 5},   {"15m", 15}, {"30m", 30},
                                                 {"1h", 60},  {"4h", 240}, {"1d", 1440}};
    const auto it = m.find(tf);
    return it == m.end() ? 0 : it->second;
}

} // namespace

int main(int argc, char** argv) {
    std::string tf, columns_file;
    int64_t every_min = 0, from_ms = 0, to_ms = std::numeric_limits<int64_t>::max();
    size_t chunk = 5000;   // the service's backfill_chunk
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--tf") tf = next();
        else if (a == "--columns") columns_file = next();
        else if (a == "--every-min") every_min = std::atoll(next().c_str());
        else if (a == "--from-ms") from_ms = std::atoll(next().c_str());
        else if (a == "--to-ms") to_ms = std::atoll(next().c_str());
        else if (a == "--chunk") chunk = static_cast<size_t>(std::atoll(next().c_str()));
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::cerr << "unknown argument: " << a << "\n"; usage(); return 2; }
    }
    const int minutes = tf_minutes(tf);
    if (minutes == 0 || columns_file.empty() || chunk == 0) { usage(); return 2; }

    // Requested columns, resolved against the frozen catalog by NAME.
    const auto& names = alttech::talib_features::core_feature_names();
    std::vector<std::string> columns;
    std::vector<size_t> positions;
    {
        std::ifstream in(columns_file);
        std::string name;
        while (std::getline(in, name)) {
            if (name.empty()) continue;
            const auto it = std::find(names.begin(), names.end(), name);
            if (it == names.end()) {
                std::cerr << "column not in the talib-core catalog: " << name << "\n";
                return 2;
            }
            columns.push_back(name);
            positions.push_back(static_cast<size_t>(it - names.begin()));
        }
    }
    if (columns.empty()) { std::cerr << "no columns\n"; return 2; }

    std::vector<MinuteBar> minutes_in;
    {
        std::string line;
        while (std::getline(std::cin, line)) {
            MinuteBar m;
            int ok = 1;
            if (std::sscanf(line.c_str(), "%ld,%lf,%lf,%lf,%lf,%lf,%lf,%d", &m.open_time_ms, &m.open,
                            &m.high, &m.low, &m.close, &m.base_volume, &m.quote_volume, &ok) < 7) {
                continue;   // header or malformed
            }
            m.ok = ok != 0;
            if (!minutes_in.empty() && m.open_time_ms <= minutes_in.back().open_time_ms) {
                std::cerr << "input not strictly ordered at " << m.open_time_ms << "\n";
                return 1;
            }
            minutes_in.push_back(m);
        }
    }
    const auto bars = alttech::dataset::aggregate(minutes_in, minutes);
    const int64_t span = static_cast<int64_t>(minutes) * 60'000;
    const int64_t every = every_min * 60'000;

    std::printf("open_time_ms,ok,status");
    for (const auto& c : columns) std::printf(",%s", c.c_str());
    std::printf("\n");

    std::deque<FBar> history;
    std::vector<TfBar> run;
    const auto flush = [&]() {
        if (run.empty()) return;
        std::vector<FBar> series(history.begin(), history.end());
        const size_t ctx = series.size();
        for (const auto& b : run) series.push_back(b.bar);
        const auto vecs = FeatureEngine::process_series(series);
        for (size_t k = 0; k < run.size(); ++k) {
            const auto& b = run[k];
            if (alttech::dataset::emit_bar(b.bar.open_time_ms, span, every, from_ms, to_ms)) {
                const auto& v = vecs[ctx + k];
                std::printf("%ld,%d,%s", b.bar.open_time_ms, b.ok ? 1 : 0,
                            b.ok ? alttech::talib_features::status_label(v.status) : "INCOMPLETE_INPUT");
                for (const size_t p : positions) {
                    if (b.ok && v.is_valid(p) && std::isfinite(v.values[p])) {
                        std::printf(",%.9g", static_cast<double>(v.values[p]));
                    } else {
                        std::printf(",");
                    }
                }
                std::printf("\n");
            }
        }
        for (const auto& b : run) {
            history.push_back(b.bar);
            if (history.size() > FeatureEngine::kWarmupBars) history.pop_front();
        }
        run.clear();
    };

    int64_t last = -1;
    for (const auto& b : bars) {
        if (last >= 0 && b.bar.open_time_ms != last + span) {   // gap: context is gone
            flush();
            history.clear();
        }
        last = b.bar.open_time_ms;
        run.push_back(b);
        if (run.size() >= chunk) flush();
    }
    flush();
    return 0;
}
