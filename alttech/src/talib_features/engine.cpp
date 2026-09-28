#include "alttech/talib_features/engine.hpp"
#include "alttech/talib_features/types.hpp"
#include "alttech/common/hash.hpp"

#include <ta_abstract.h>
#include <ta_common.h>
#include <ta_defs.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>

namespace alttech::talib_features {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Every parameterisable indicator is evaluated across this ladder of periods
// (short / medium / long), not just TA-Lib's single default. Values outside a
// parameter's own [min,max] are clamped away, duplicates dropped.
constexpr int kPeriods[] = {5, 9, 14, 20, 50, 100, 200};

// Every moving-average flavour TA-Lib offers. Functions exposing an MAType knob
// (BBANDS, MA, APO, PPO, STOCH*, MACDEXT, …) are evaluated with each of them.
// When a function has several MAType knobs (STOCH: 2, MACDEXT: 3) they are set
// to the SAME flavour: full coverage of the types without a 9^N explosion.
struct MaFlavour { TA_MAType type; const char* label; };
constexpr MaFlavour kMaTypes[] = {
    {TA_MAType_SMA, "SMA"},   {TA_MAType_EMA, "EMA"},   {TA_MAType_WMA, "WMA"},
    {TA_MAType_DEMA, "DEMA"}, {TA_MAType_TEMA, "TEMA"}, {TA_MAType_TRIMA, "TRIMA"},
    {TA_MAType_KAMA, "KAMA"}, {TA_MAType_MAMA, "MAMA"}, {TA_MAType_T3, "T3"}};

// Functions excluded by name rather than by group, so a function TA-Lib adds is
// included by default instead of being silently dropped along with its group.
//
// Every entry here was measured on a real BTCUSDT 1m series (400 bars around
// 64 000) and produces nothing a consumer can use. These are element-wise math
// transforms of the price, not indicators: they have no period, no market
// interpretation, and on a price scale they degenerate.
//
//   ACOS, ASIN          domain is [-1,1]; a price is outside it -> NaN on every
//                       bar, 400/400 measured
//   COSH, SINH, EXP     overflow at a price of 64 000 -> +inf on every bar
//   ATAN, TANH          saturate; measured min == max, i.e. a constant column
//   SIN, COS, TAN       trigonometry of a price in radians is pure aliasing;
//                       TAN measured between -273 and +144 with no relation to
//                       the market
//   CEIL, FLOOR         the price, rounded — collinear with `close`
//   LN, LOG10, SQRT     monotone transforms of `close`, carrying no information
//                       the close itself does not already carry
//
// The Math Operators group is NOT excluded: MAX, MIN, MINMAX and their *INDEX
// variants over a period are Donchian bounds and time-since-extreme, and SUM is
// a rolling sum — all genuine rolling-window statistics.
constexpr std::string_view kExcludedFunctions[] = {
    "ACOS", "ASIN", "ATAN", "CEIL", "COS",  "COSH", "EXP", "FLOOR",
    "LN",   "LOG10", "SIN", "SINH", "SQRT", "TAN",  "TANH"};

bool excluded_function(const char* name) {
    if (name == nullptr) return true;
    for (const auto excluded : kExcludedFunctions) {
        if (excluded == name) return true;
    }
    return false;
}

// A second binding of the same TA-Lib function against a different OHLCV series.
// TA-Lib's single-input functions take one real series, which the engine feeds
// with `close`; but MAX over the highs and MIN over the lows are the Donchian
// channel, and SMA/STDDEV over volume are what a volume z-score is made of.
// Without these twins those readings simply would not exist in the catalog.
struct SeriesTwin {
    std::string_view function;
    int real_slot;        // 1 = high, 2 = low, 3 = volume
    std::string_view tag; // name infix, e.g. SMA_VOL_20_outReal
};
constexpr SeriesTwin kSeriesTwins[] = {
    {"SMA", 3, "VOL"},      {"STDDEV", 3, "VOL"},
    {"MAX", 1, "HIGH"},     {"MAXINDEX", 1, "HIGH"},
    {"MIN", 2, "LOW"},      {"MININDEX", 2, "LOW"}};

// TA_Initialize is global and not reentrant; do it once per process.
Result<void> init_talib_once() {
    static TA_RetCode rc = TA_UNKNOWN_ERR;
    static std::once_flag once;
    std::call_once(once, [] { rc = TA_Initialize(); });
    if (rc != TA_SUCCESS) {
        return std::unexpected(Error{ErrorCode::internal_error, "TA_Initialize failed"});
    }
    return {};
}

} // namespace

// One TA-Lib function bound to our OHLCV layout.
struct Engine::Spec {
    const TA_FuncHandle* handle = nullptr;
    std::string func;
    unsigned nb_input = 0;
    unsigned nb_output = 0;
    // Per input slot: price-style (fed from OHLCV) or a plain real series.
    struct In {
        TA_InputParameterType type = TA_Input_Price;
        int real_slot = 0; // for TA_Input_Real: 0=close, 1=high, 2=low, 3=volume
    };
    std::vector<In> inputs;
    std::vector<TA_OutputParameterType> out_types;
    size_t first_col = 0; // offset of this function's first output column
    bool needs_volume = false;
    int period_idx = -1;  // opt-input index of optInTimePeriod, -1 = keep defaults
    int period = 0;       // value applied to that opt input
    std::vector<unsigned> ma_idx; // opt-input indices of every MAType knob
    int ma_type = -1;             // flavour applied to all of them, -1 = default
    // When >= 0, feed the single real input from this OHLCV slot instead of the
    // default close: 1 = high, 2 = low, 3 = volume.
    int forced_slot = -1;
};

Engine::Engine() = default;
Engine::~Engine() = default;

size_t Engine::function_count() const { return specs_.size(); }

Result<std::unique_ptr<Engine>> Engine::create() {
    if (auto r = init_talib_once(); !r) return std::unexpected(r.error());

    auto eng = std::unique_ptr<Engine>(new Engine());

    TA_StringTable* groups = nullptr;
    if (TA_GroupTableAlloc(&groups) != TA_SUCCESS || groups == nullptr) {
        return std::unexpected(Error{ErrorCode::internal_error, "TA_GroupTableAlloc failed"});
    }

    for (unsigned gi = 0; gi < groups->size; ++gi) {
        const char* group = groups->string[gi];
        TA_StringTable* funcs = nullptr;
        if (TA_FuncTableAlloc(group, &funcs) != TA_SUCCESS || funcs == nullptr) continue;

        for (unsigned fi = 0; fi < funcs->size; ++fi) {
            const char* fname = funcs->string[fi];
            if (excluded_function(fname)) continue;
            const TA_FuncHandle* handle = nullptr;
            if (TA_GetFuncHandle(fname, &handle) != TA_SUCCESS || handle == nullptr) continue;

            const TA_FuncInfo* info = nullptr;
            if (TA_GetFuncInfo(handle, &info) != TA_SUCCESS || info == nullptr) continue;

            Spec spec;
            spec.handle = handle;
            spec.func = fname;
            spec.nb_input = info->nbInput;
            spec.nb_output = info->nbOutput;

            // Bind inputs. Integer inputs cannot be served from OHLCV -> skip
            // the function entirely rather than invent data.
            bool usable = true;
            int real_slot = 0;
            for (unsigned i = 0; i < info->nbInput; ++i) {
                const TA_InputParameterInfo* in = nullptr;
                if (TA_GetInputParameterInfo(handle, i, &in) != TA_SUCCESS || in == nullptr) {
                    usable = false;
                    break;
                }
                Spec::In slot;
                slot.type = in->type;
                if (in->type == TA_Input_Price) {
                    if ((in->flags & TA_IN_PRICE_VOLUME) != 0) spec.needs_volume = true;
                } else if (in->type == TA_Input_Real) {
                    slot.real_slot = real_slot++;
                    if (slot.real_slot == 3) spec.needs_volume = true;
                } else {
                    usable = false; // TA_Input_Integer
                    break;
                }
                spec.inputs.push_back(slot);
            }
            if (!usable || spec.nb_output == 0) continue;

            // Collect output metadata once; names get the period suffix below.
            std::vector<std::string> out_names;
            for (unsigned o = 0; o < info->nbOutput; ++o) {
                const TA_OutputParameterInfo* out = nullptr;
                if (TA_GetOutputParameterInfo(handle, o, &out) != TA_SUCCESS || out == nullptr) {
                    usable = false;
                    break;
                }
                spec.out_types.push_back(out->type);
                out_names.emplace_back(out->paramName);
            }
            if (!usable) continue;

            // Exactly one "optInTimePeriod" is swept; functions with several
            // period-like knobs (MACD fast/slow/signal, ULTOSC) keep defaults so
            // the catalog cannot explode combinatorially.
            int period_idx = -1;
            int period_min = 0;
            int period_max = 0;
            unsigned period_hits = 0;
            std::vector<unsigned> ma_indices;
            for (unsigned k = 0; k < info->nbOptInput; ++k) {
                const TA_OptInputParameterInfo* oi = nullptr;
                if (TA_GetOptInputParameterInfo(handle, k, &oi) != TA_SUCCESS || oi == nullptr) {
                    continue;
                }
                if (oi->paramName != nullptr &&
                    std::string_view(oi->paramName).find("MAType") != std::string_view::npos) {
                    ma_indices.push_back(k);
                }
                if (oi->type == TA_OptInput_IntegerRange && oi->paramName != nullptr &&
                    std::string_view(oi->paramName).find("TimePeriod") != std::string_view::npos) {
                    ++period_hits;
                    period_idx = static_cast<int>(k);
                    if (const auto* r = static_cast<const TA_IntegerRange*>(oi->dataSet)) {
                        period_min = r->min;
                        period_max = r->max;
                    }
                }
            }

            std::vector<int> periods;
            if (period_hits == 1 && period_max >= period_min && period_max > 0) {
                for (int p : kPeriods) {
                    const int c = p < period_min ? period_min : (p > period_max ? period_max : p);
                    if (std::find(periods.begin(), periods.end(), c) == periods.end()) {
                        periods.push_back(c);
                    }
                }
            } else {
                period_idx = -1;
                periods.push_back(0); // single variant, library defaults
            }

            // Flavour ladder: every MA type when the function exposes one, else
            // a single pass on library defaults.
            std::vector<int> ma_variants;
            if (!ma_indices.empty()) {
                for (const auto& m : kMaTypes) ma_variants.push_back(static_cast<int>(m.type));
            } else {
                ma_variants.push_back(-1);
            }

            // A twin binding of this function against another OHLCV series, when
            // one is declared and the function actually takes a single real
            // input over a period.
            const SeriesTwin* twin_spec = nullptr;
            if (spec.inputs.size() == 1 && spec.inputs[0].type == TA_Input_Real &&
                period_idx >= 0) {
                for (const auto& candidate : kSeriesTwins) {
                    if (candidate.function == spec.func) {
                        twin_spec = &candidate;
                        break;
                    }
                }
            }

            for (int p : periods) {
                for (int mt : ma_variants) {
                    Spec var = spec;
                    var.period_idx = period_idx;
                    var.period = p;
                    var.ma_idx = ma_indices;
                    var.ma_type = mt;
                    var.first_col = eng->names_.size();

                    std::string tag(fname);
                    if (period_idx >= 0) tag += "_" + std::to_string(p);
                    if (mt >= 0) {
                        for (const auto& m : kMaTypes) {
                            if (static_cast<int>(m.type) == mt) { tag += std::string("_") + m.label; break; }
                        }
                    }
                    for (const auto& on : out_names) eng->names_.push_back(tag + "_" + on);

                    // Warm-up for this exact parameterisation.
                    TA_ParamHolder* params = nullptr;
                    if (TA_ParamHolderAlloc(handle, &params) == TA_SUCCESS && params != nullptr) {
                        if (period_idx >= 0) {
                            TA_SetOptInputParamInteger(params, static_cast<unsigned>(period_idx), p);
                        }
                        for (unsigned mi : ma_indices) {
                            if (mt >= 0) TA_SetOptInputParamInteger(params, mi, mt);
                        }
                        TA_Integer lb = 0;
                        if (TA_GetLookback(params, &lb) == TA_SUCCESS && lb > eng->max_lookback_) {
                            eng->max_lookback_ = lb;
                        }
                        TA_ParamHolderFree(params);
                    }
                    // Build the twin from a copy taken BEFORE `var` is moved
                    // into the catalog — copying a moved-from Spec leaves its
                    // input/output vectors empty and reads out of bounds.
                    std::optional<Spec> twin;
                    if (twin_spec != nullptr && mt < 0) {
                        Spec other = var;
                        other.forced_slot = twin_spec->real_slot;
                        other.needs_volume = twin_spec->real_slot == 3;
                        twin = std::move(other);
                    }
                    eng->specs_.push_back(std::move(var));

                    if (twin) {
                        twin->first_col = eng->names_.size();
                        for (const auto& on : out_names) {
                            eng->names_.push_back(std::string(fname) + "_" +
                                                  std::string(twin_spec->tag) + "_" +
                                                  std::to_string(p) + "_" + on);
                        }
                        eng->specs_.push_back(std::move(*twin));
                    }
                }
            }
        }
        TA_FuncTableFree(funcs);
    }
    TA_GroupTableFree(groups);

    if (eng->specs_.empty()) {
        return std::unexpected(Error{ErrorCode::internal_error, "no TA-Lib functions bound"});
    }
    return eng;
}

Result<void> Engine::compute(const Ohlcv& in, std::vector<std::vector<double>>& rows) const {
    if (!in.consistent()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "OHLCV columns differ in length"});
    }
    const size_t n = in.size();
    rows.assign(n, std::vector<double>(names_.size(), kNaN));
    if (n == 0) return {};

    const bool have_volume = !in.volume.empty();
    const double* series[4] = {in.close.data(), in.high.data(), in.low.data(),
                               have_volume ? in.volume.data() : nullptr};

    for (const auto& spec : specs_) {
        if (spec.needs_volume && !have_volume) continue; // stays NaN: never faked

        TA_ParamHolder* params = nullptr;
        if (TA_ParamHolderAlloc(spec.handle, &params) != TA_SUCCESS || params == nullptr) continue;

        bool ok = true;
        if (spec.period_idx >= 0) {
            ok = TA_SetOptInputParamInteger(params, static_cast<unsigned>(spec.period_idx),
                                            spec.period) == TA_SUCCESS;
        }
        for (unsigned mi : spec.ma_idx) {
            if (!ok || spec.ma_type < 0) break;
            ok = TA_SetOptInputParamInteger(params, mi, spec.ma_type) == TA_SUCCESS;
        }
        for (unsigned i = 0; i < spec.inputs.size() && ok; ++i) {
            const auto& slot = spec.inputs[i];
            if (slot.type == TA_Input_Price) {
                ok = TA_SetInputParamPricePtr(params, i, in.open.data(), in.high.data(),
                                              in.low.data(), in.close.data(),
                                              have_volume ? in.volume.data() : nullptr,
                                              nullptr) == TA_SUCCESS;
            } else {
                const double* s =
                    spec.forced_slot >= 0 ? series[spec.forced_slot] : series[slot.real_slot];
                ok = s != nullptr &&
                     TA_SetInputParamRealPtr(params, i, s) == TA_SUCCESS;
            }
        }

        // One output at a time keeps buffers small; TA-Lib requires every output
        // to be bound, so unused ones point at the same scratch space.
        std::vector<std::vector<double>> reals(spec.nb_output);
        std::vector<std::vector<TA_Integer>> ints(spec.nb_output);
        for (unsigned o = 0; o < spec.nb_output && ok; ++o) {
            if (spec.out_types[o] == TA_Output_Real) {
                reals[o].assign(n, 0.0);
                ok = TA_SetOutputParamRealPtr(params, o, reals[o].data()) == TA_SUCCESS;
            } else {
                ints[o].assign(n, 0);
                ok = TA_SetOutputParamIntegerPtr(params, o, ints[o].data()) == TA_SUCCESS;
            }
        }

        TA_Integer beg = 0;
        TA_Integer count = 0;
        if (ok && TA_CallFunc(params, 0, static_cast<TA_Integer>(n) - 1, &beg, &count) ==
                      TA_SUCCESS) {
            for (unsigned o = 0; o < spec.nb_output; ++o) {
                const size_t col = spec.first_col + o;
                for (TA_Integer k = 0; k < count; ++k) {
                    const size_t row = static_cast<size_t>(beg) + static_cast<size_t>(k);
                    if (row >= n) break;
                    const double v = spec.out_types[o] == TA_Output_Real
                                         ? reals[o][static_cast<size_t>(k)]
                                         : static_cast<double>(ints[o][static_cast<size_t>(k)]);
                    if (std::isfinite(v)) rows[row][col] = v;
                }
            }
        }
        TA_ParamHolderFree(params);
    }
    return {};
}

// One process-wide engine: enumeration is deterministic and immutable, so the
// catalog can be shared. A failed enumeration yields an empty catalog and the
// caller (service start-up) refuses to run.
const Engine* shared_engine() {
    static std::unique_ptr<Engine> eng = [] -> std::unique_ptr<Engine> {
        auto e = Engine::create();
        if (!e) return nullptr;
        return std::move(*e);
    }();
    return eng.get();
}

const std::vector<std::string>& core_feature_names() {
    static const std::vector<std::string> empty;
    const Engine* e = shared_engine();
    return e ? e->names() : empty;
}

size_t feature_count() { return core_feature_names().size(); }

// Above TA-Lib's longest lookback across the swept period ladder (~1194 bars at
// period 200): every indicator is converged before a value is published.
size_t warmup_bars() { return 2048; }

std::string core_schema_fingerprint() {
    std::string joined;
    for (const auto& n : core_feature_names()) {
        joined += n;
        joined += '\n';
    }
    return sha256_hex(joined);
}

} // namespace alttech::talib_features
