#include "altsignalquality/data/store.hpp"

#include "altsignalquality/core/hash.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/time.hpp"
#include "altsignalquality/data/pg_array.hpp"
#include "altsignalquality/domain/labeling.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace altsignalquality::data {

namespace {

using nlohmann::json;

// Ids are sha256-derived from the natural key so the SAME signal always yields
// the SAME id, on any host, after any restart, in any order.
// nlohmann's non-throwing parse returns a DISCARDED value on bad input, and
// dumping a discarded value throws — so a corrupt JSONB column would turn a
// status query into an exception. Report the corruption as data instead.
json parse_json_column(const std::string& text) {
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        return json{{"parse_error", "column is not valid JSON"},
                    {"raw_prefix", text.substr(0, 120)}};
    }
    return parsed;
}

std::string derive_id(const char* prefix, const std::string& natural_key) {
    return std::string(prefix) + sha256_hex(std::string(prefix) + ":" + natural_key).substr(0, 32);
}

std::optional<double> opt_double(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    const double v = f.as<double>();
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

// Copy the named numeric members of one JSON object into a name -> value map,
// skipping anything that is not a finite number. A string or null where a
// number was expected is upstream corruption, and must become a MISSING
// feature rather than a silent zero.
void collect_numeric(const json& node, std::map<std::string, double>& out) {
    if (!node.is_object()) return;
    for (const auto& [k, v] : node.items()) {
        if (!v.is_number()) continue;
        const double d = v.get<double>();
        if (std::isfinite(d)) out[k] = d;
    }
}

std::optional<double> json_number(const json& node, const char* key) {
    if (!node.is_object()) return std::nullopt;
    const auto it = node.find(key);
    if (it == node.end() || !it->is_number()) return std::nullopt;
    const double d = it->get<double>();
    if (!std::isfinite(d)) return std::nullopt;
    return d;
}

std::vector<std::string> parse_text_array(const std::string& literal) {
    // PostgreSQL text[] output form: {a,"b,c",NULL}
    std::vector<std::string> out;
    if (literal.size() < 2 || literal.front() != '{' || literal.back() != '}') return out;
    const std::string body = literal.substr(1, literal.size() - 2);
    std::string cur;
    bool in_quotes = false, have = false;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (in_quotes) {
            if (c == '\\' && i + 1 < body.size()) {
                cur += body[++i];
            } else if (c == '"') {
                in_quotes = false;
            } else {
                cur += c;
            }
            have = true;
        } else if (c == '"') {
            in_quotes = true;
            have = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have || !cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

std::string make_evaluation_id(const std::string& signal_proposal_id) {
    return derive_id("sqev_", signal_proposal_id);
}

std::string make_final_signal_id(const std::string& signal_proposal_id) {
    return derive_id("sqfin_", signal_proposal_id);
}

std::string make_example_id(const std::string& signal_proposal_id,
                            const std::string& feature_schema_version,
                            const std::string& label_spec_version) {
    return derive_id("sqex_", signal_proposal_id + "|" + feature_schema_version + "|" +
                                 label_spec_version);
}

// ---------------------------------------------------------------------------
// startup
// ---------------------------------------------------------------------------

Result<void> Store::register_contracts() {
    const std::string fs_version(domain::kFeatureSchemaVersion);
    const std::string fs_hash = domain::feature_schema_hash();
    const auto fs_count = static_cast<int32_t>(domain::kFeatureCount);
    const std::string ls_version(domain::kLabelSpecVersion);
    const std::string ls_json = domain::label_spec_json();
    const std::string ls_hash = sha256_hex(ls_json);

    std::string conflict;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altsignalquality.feature_schemas"
            " (feature_schema_version, schema_hash, feature_count, features)"
            " VALUES ($1,$2,$3,$4::jsonb) ON CONFLICT (feature_schema_version) DO NOTHING",
            fs_version, fs_hash, fs_count, domain::feature_contract_json());
        const auto fs = txn.exec_params(
            "SELECT schema_hash, feature_count FROM altsignalquality.feature_schemas"
            " WHERE feature_schema_version = $1",
            fs_version);
        if (!fs.empty() &&
            (fs[0][0].as<std::string>() != fs_hash || fs[0][1].as<int>() != fs_count)) {
            conflict = "feature schema " + fs_version + " is already registered with a different "
                       "hash/size: this binary redefined a contract that historical rows and "
                       "shipped models are pinned to — bump kFeatureSchemaVersion instead";
            return;
        }
        txn.exec_params(
            "INSERT INTO altsignalquality.label_specs"
            " (label_spec_version, label_spec_hash, definition)"
            " VALUES ($1,$2,$3::jsonb) ON CONFLICT (label_spec_version) DO NOTHING",
            ls_version, ls_hash, ls_json);
        const auto ls = txn.exec_params(
            "SELECT label_spec_hash FROM altsignalquality.label_specs WHERE label_spec_version = $1",
            ls_version);
        if (!ls.empty() && ls[0][0].as<std::string>() != ls_hash) {
            conflict = "label spec " + ls_version + " is already registered with a different "
                       "definition — bump kLabelSpecVersion instead";
        }
    });
    if (!res) return std::unexpected(res.error());
    if (!conflict.empty()) return fail(ErrorCode::validation_error, conflict);
    return {};
}

Result<void> Store::verify_upstream_readable() {
    // LIMIT 0 executes the plan and resolves every column without moving rows,
    // so a renamed or dropped upstream column fails here rather than at 03:00
    // inside a training cycle.
    const std::vector<std::string> probes = {
        "SELECT signal_proposal_id, candidate_id, canonical_pair_id, strategy_id, "
        "strategy_version, parameter_set_id, side, proposal_grade, recommended_for_execution, "
        "created_at_ms, valid_until_ms, time_stop_at_ms, context_revision "
        "FROM " + altstrategy_ + ".trade_signal_proposals LIMIT 0",
        "SELECT candidate_id, context_id, context_revision FROM " + altstrategy_ +
        ".strategy_candidates LIMIT 0",
        "SELECT signal_proposal_id, outcome, realized_net_return, resolved_time_ms, mfe, mae, "
        "holding_time_ms, is_counterfactual FROM " + altstrategy_ + ".market_outcomes LIMIT 0",
        "SELECT context_id, event_time_ms, available_at_ms, data_quality, bitget_regime, "
        "global_regime, venue_mask, missing_venue_mask, regime_divergence, snapshot "
        "FROM " + altstrategy_ + ".decision_context_snapshots LIMIT 0",
        "SELECT signal_proposal_id, ordinal FROM " + altstrategy_ +
        ".proposal_warnings LIMIT 0",
    };
    std::string failure;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& sql : probes) {
            try {
                txn.exec(sql);
            } catch (const std::exception& e) {
                failure = e.what();
                return;
            }
        }
    });
    if (!res) return std::unexpected(res.error());
    if (!failure.empty()) {
        return fail(ErrorCode::database_error,
                    "AltStrategy schema '" + altstrategy_ + "' is not readable as expected: " +
                        failure);
    }
    return {};
}

// ---------------------------------------------------------------------------
// inference path
// ---------------------------------------------------------------------------

namespace {

// The column list every context read shares, and the parser for it. One list,
// one parser: a context loaded on the inference path and one loaded during a
// backfill are then provably the same object.
constexpr const char* kContextColumns =
    " cs.context_id, cs.event_time_ms, cs.available_at_ms, cs.data_quality,"
    " COALESCE(cs.bitget_regime,''), COALESCE(cs.global_regime,''),"
    " cs.venue_mask, cs.missing_venue_mask, cs.regime_divergence,"
    " cs.fees_bps, cs.funding_rate, cs.mark_price, cs.spread_bps, cs.top_depth_usdt,"
    " cs.liquidity_rank, cs.expected_slippage_bps, cs.snapshot::text ";

// `base` is the column offset of the block above inside a wider result, so one
// parser serves both the standalone lookup and the batch join.
domain::ContextSnapshot parse_context_row(const pqxx::row& r, int base = 0) {
    const auto at = [&](int i) { return r[base + i]; };
    domain::ContextSnapshot ctx;
    ctx.present = true;
    ctx.event_time_ms = at(1).as<int64_t>();
    ctx.available_at_ms = at(2).as<int64_t>();
    ctx.data_quality = at(3).as<std::string>();
    ctx.bitget_regime = at(4).as<std::string>();
    ctx.global_regime = at(5).as<std::string>();
    ctx.venue_mask = static_cast<uint32_t>(at(6).as<int64_t>());
    ctx.missing_venue_mask = static_cast<uint32_t>(at(7).as<int64_t>());
    ctx.regime_divergence = at(8).as<bool>();
    ctx.fees_bps = opt_double(at(9));
    ctx.funding_rate = opt_double(at(10));
    ctx.mark_price = opt_double(at(11));
    ctx.spread_bps = opt_double(at(12));
    ctx.top_depth_usdt = opt_double(at(13));
    ctx.liquidity_rank = opt_double(at(14));
    ctx.expected_slippage_bps = opt_double(at(15));

    // The point-in-time analytics AltStrategy fed its own strategies. The
    // column exists on every deployment; it is only populated where AltStrategy
    // persists the snapshot, and an empty object simply leaves the dependent
    // features MISSING rather than fabricating them.
    if (!at(16).is_null()) {
        try {
            const json snap = json::parse(at(16).as<std::string>());
            collect_numeric(snap.value("bitget_features", json::object()), ctx.bitget_features);
            collect_numeric(snap.value("microstructure", json::object()), ctx.microstructure);
            collect_numeric(snap.value("cross_venue_features", json::object()), ctx.cross_venue);
            // Higher-timeframe context, keyed by bar length in minutes. The
            // SHORTEST available grid above the execution one is taken: it is
            // the closest higher frame to the decision and the one most likely
            // to be fresh. AltStrategy admits each grid only once its own bar
            // has CLOSED, so nothing here can be later than T0.
            //
            // This block was not read at all: AltStrategy carries the grids
            // (measured 27/27 contexts on VPS1) and the quality model never saw
            // them, so "does the higher frame agree with this signal" could not
            // even be asked.
            if (const auto grids = snap.value("context_features", json::object());
                grids.is_object() && !grids.empty()) {
                int32_t chosen = 0;
                for (const auto& [key, blob] : grids.items()) {
                    int32_t tf = 0;
                    try { tf = static_cast<int32_t>(std::stoi(key)); } catch (...) { continue; }
                    if (tf <= 0 || !blob.is_object()) continue;
                    if (chosen == 0 || tf < chosen) {
                        const json vals = blob.value("values", json::object());
                        if (vals.is_object() && !vals.empty()) {
                            ctx.htf_features.clear();
                            collect_numeric(vals, ctx.htf_features);
                            ctx.htf_minutes = tf;
                            chosen = tf;
                        }
                    }
                }
            }
            const json regime = snap.value("global_regime_view", json::object());
            ctx.regime_confidence = json_number(regime, "confidence");
            ctx.regime_trend_strength = json_number(regime, "trend_strength");
            ctx.regime_volatility = json_number(regime, "volatility_regime");
            ctx.regime_liquidity_quality = json_number(regime, "liquidity_quality");
        } catch (const json::exception&) {
            // Malformed snapshot JSON is a producer-side problem. The scalar
            // columns above are still authoritative, so the context stays usable
            // and only the analytics block goes MISSING.
            log::warn("context_snapshot_json_invalid",
                      {{"context_id", at(0).as<std::string>()}});
        }
    }
    return ctx;
}

} // namespace

Result<domain::ContextSnapshot> Store::load_context(const std::string& context_id) {
    domain::ContextSnapshot ctx;
    if (context_id.empty()) return ctx;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            std::string("SELECT") + kContextColumns +
                "FROM " + altstrategy_ + ".decision_context_snapshots cs WHERE cs.context_id = $1",
            context_id);
        if (rows.empty()) return;
        ctx = parse_context_row(rows[0]);
    });
    if (!res) return std::unexpected(res.error());
    return ctx;
}

Result<Store::ResolvedContext> Store::load_context_for_candidate(const std::string& candidate_id) {
    ResolvedContext out;
    if (candidate_id.empty()) return out;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        // The candidate row is written by AltStrategy before the proposal it
        // produced is published, so by the time this service sees the signal the
        // join is committed. If retention has already removed the context, the
        // snapshot stays absent and the dependent features go MISSING — which is
        // reported, not filled in.
        const auto rows = txn.exec_params(
            std::string("SELECT") + kContextColumns +
                "FROM " + altstrategy_ + ".strategy_candidates c"
                " LEFT JOIN " + altstrategy_ + ".decision_context_snapshots cs"
                "   ON cs.context_id = c.context_id"
                " WHERE c.candidate_id = $1",
            candidate_id);
        if (rows.empty() || rows[0][0].is_null()) return;
        out.context_id = rows[0][0].as<std::string>();
        out.snapshot = parse_context_row(rows[0]);
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::optional<ExistingEvaluation>> Store::claim_evaluation(const EvaluationRow& row) {
    std::optional<ExistingEvaluation> existing;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const auto inserted = txn.exec_params(
            "INSERT INTO altsignalquality.signal_evaluations ("
            " signal_proposal_id, evaluation_id, candidate_id, canonical_pair_id, symbol,"
            " strategy_id, strategy_version, parameter_set_id, side, proposal_grade,"
            " upstream_recommended, regime, context_id, context_revision, signal_created_at_ms,"
            " evaluated_at_ms, feature_schema_version, feature_schema_hash, features,"
            " validity_bitmap, missing_feature_count, has_quality_score, raw_score,"
            " quality_score, decision_threshold, decision, decision_basis, reason_codes,"
            " model_id, calibration_version, inference_latency_us, final_signal_id, published, candidate_payload)"
            " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,"
            "         $19::real[],$20::bytea,$21,$22,$23,$24,$25,$26,$27,$28::text[],$29,$30,"
            "         $31,$32,false,$33::bytea)"
            " ON CONFLICT (signal_proposal_id) DO NOTHING"
            " RETURNING evaluation_id",
            row.signal_proposal_id, row.evaluation_id, row.candidate_id, row.canonical_pair_id,
            row.symbol, row.strategy_id, row.strategy_version, row.parameter_set_id, row.side,
            row.proposal_grade, row.upstream_recommended, row.regime, row.context_id,
            row.context_revision, row.signal_created_at_ms, row.evaluated_at_ms,
            row.feature_schema_version, row.feature_schema_hash,
            pgarray::float_array_literal(row.features.values),
            pgarray::bytea_hex_literal(row.features.validity), row.features.missing_count,
            row.has_quality_score,
            row.has_quality_score ? std::optional<double>(row.raw_score) : std::nullopt,
            row.has_quality_score ? std::optional<double>(row.quality_score) : std::nullopt,
            row.has_quality_score ? std::optional<double>(row.decision_threshold) : std::nullopt,
            row.decision, row.decision_basis, pgarray::text_array_literal(row.reason_codes),
            row.model_id.empty() ? std::optional<std::string>() : std::optional(row.model_id),
            row.calibration_version.empty() ? std::optional<std::string>()
                                            : std::optional(row.calibration_version),
            row.inference_latency_us,
            row.final_signal_id.empty() ? std::optional<std::string>()
                                        : std::optional(row.final_signal_id),
            row.candidate_payload.empty()
                ? std::optional<std::string>()
                : std::optional(pgarray::bytea_hex_literal(
                      std::vector<uint8_t>(row.candidate_payload.begin(),
                                           row.candidate_payload.end()))));
        if (!inserted.empty()) return;  // fresh claim

        const auto prior = txn.exec_params(
            "SELECT evaluation_id, decision, COALESCE(final_signal_id,''), published,"
            "       evaluated_at_ms"
            " FROM altsignalquality.signal_evaluations WHERE signal_proposal_id = $1",
            row.signal_proposal_id);
        if (prior.empty()) return;
        ExistingEvaluation e;
        e.evaluation_id = prior[0][0].as<std::string>();
        e.decision = prior[0][1].as<std::string>();
        e.final_signal_id = prior[0][2].as<std::string>();
        e.published = prior[0][3].as<bool>();
        e.evaluated_at_ms = prior[0][4].as<int64_t>();
        existing = e;
    });
    if (!res) return std::unexpected(res.error());
    return existing;
}

Result<void> Store::mark_published(const std::string& signal_proposal_id, int64_t published_at_ms) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            // The payload exists only to survive the publish gap; once the
            // signal is on the bus it is dead weight.
            "UPDATE altsignalquality.signal_evaluations"
            " SET published = true, published_at_ms = $2, candidate_payload = NULL"
            " WHERE signal_proposal_id = $1 AND NOT published",
            signal_proposal_id, published_at_ms);
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

Result<std::vector<EvaluationRow>> Store::load_unpublished(int limit) {
    std::vector<EvaluationRow> out;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& r : txn.exec_params(
                 "SELECT signal_proposal_id, evaluation_id, candidate_id, canonical_pair_id,"
                 "       symbol, strategy_id, strategy_version, side, proposal_grade,"
                 "       upstream_recommended, context_id, context_revision,"
                 "       signal_created_at_ms, evaluated_at_ms, feature_schema_version,"
                 "       feature_schema_hash, has_quality_score, COALESCE(raw_score,0),"
                 "       COALESCE(quality_score,0), COALESCE(decision_threshold,0), decision,"
                 "       decision_basis, reason_codes::text, COALESCE(model_id,''),"
                 "       COALESCE(calibration_version,''), inference_latency_us,"
                 "       COALESCE(final_signal_id,''), missing_feature_count,"
                 "       COALESCE(candidate_payload,''::bytea)::text"
                 " FROM altsignalquality.signal_evaluations"
                 " WHERE decision = 'ACCEPTED' AND NOT published"
                 " ORDER BY signal_created_at_ms LIMIT $1",
                 limit)) {
            EvaluationRow e;
            e.signal_proposal_id = r[0].as<std::string>();
            e.evaluation_id = r[1].as<std::string>();
            e.candidate_id = r[2].as<std::string>();
            e.canonical_pair_id = r[3].as<int64_t>();
            e.symbol = r[4].as<std::string>();
            e.strategy_id = r[5].as<std::string>();
            e.strategy_version = r[6].as<std::string>();
            e.side = r[7].as<std::string>();
            e.proposal_grade = r[8].as<std::string>();
            e.upstream_recommended = r[9].as<bool>();
            e.context_id = r[10].as<std::string>();
            e.context_revision = r[11].as<int64_t>();
            e.signal_created_at_ms = r[12].as<int64_t>();
            e.evaluated_at_ms = r[13].as<int64_t>();
            e.feature_schema_version = r[14].as<std::string>();
            e.feature_schema_hash = r[15].as<std::string>();
            e.has_quality_score = r[16].as<bool>();
            e.raw_score = r[17].as<double>();
            e.quality_score = r[18].as<double>();
            e.decision_threshold = r[19].as<double>();
            e.decision = r[20].as<std::string>();
            e.decision_basis = r[21].as<std::string>();
            e.reason_codes = parse_text_array(r[22].as<std::string>());
            e.model_id = r[23].as<std::string>();
            e.calibration_version = r[24].as<std::string>();
            e.inference_latency_us = r[25].as<int64_t>();
            e.final_signal_id = r[26].as<std::string>();
            e.features.missing_count = r[27].as<int>();
            try {
                const auto bytes = pgarray::parse_hex_bytea(r[28].as<std::string>());
                e.candidate_payload.assign(bytes.begin(), bytes.end());
            } catch (const std::exception&) {
                // An unreadable payload is reported by the caller as an
                // unrecoverable signal, never published as a partial one.
                e.candidate_payload.clear();
            }
            out.push_back(std::move(e));
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

// ---------------------------------------------------------------------------
// priors
// ---------------------------------------------------------------------------

Result<int64_t> Store::load_prior_watermark() {
    int64_t watermark = 0;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows =
            txn.exec("SELECT matured_watermark_ms FROM altsignalquality.prior_state "
                     "WHERE singleton");
        if (!rows.empty()) watermark = rows[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return watermark;
}

Result<void> Store::load_prior_cells(domain::PriorAccumulator& acc) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& r : txn.exec(
                 "SELECT scope_kind, scope_key, sample_count, win_count, sum_net_return,"
                 "       decayed_count, decayed_wins, decayed_return, last_matured_at_ms"
                 " FROM altsignalquality.outcome_priors")) {
            domain::PriorCounters c;
            c.sample_count = r[2].as<int64_t>();
            c.win_count = r[3].as<int64_t>();
            c.sum_net_return = r[4].as<double>();
            c.decayed_count = r[5].as<double>();
            c.decayed_wins = r[6].as<double>();
            c.decayed_return = r[7].as<double>();
            c.last_matured_at_ms = r[8].as<int64_t>();
            c.decay_anchor_ms = c.last_matured_at_ms;
            const std::string kind = r[0].as<std::string>();
            const std::string key = r[1].as<std::string>();
            using S = domain::PriorScope;
            const S scope = kind == "GLOBAL"            ? S::global
                            : kind == "STRATEGY"        ? S::strategy
                            : kind == "PAIR"            ? S::pair
                            : kind == "REGIME"          ? S::regime
                            : kind == "STRATEGY_REGIME" ? S::strategy_regime
                            : kind == "STRATEGY_SIDE"   ? S::strategy_side
                                                        : S::grade;
            acc.load_cell(scope, key, c);
        }
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

Result<void> Store::save_prior_cells(const domain::PriorAccumulator& acc, int64_t watermark_ms) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& [key, c] : acc.cells()) {
            const auto [kind, scope_key] = domain::PriorAccumulator::split_key(key);
            txn.exec_params(
                "INSERT INTO altsignalquality.outcome_priors ("
                " scope_kind, scope_key, sample_count, win_count, sum_net_return,"
                " decayed_count, decayed_wins, decayed_return, last_matured_at_ms, updated_at)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9, now())"
                " ON CONFLICT (scope_kind, scope_key) DO UPDATE SET"
                "  sample_count = EXCLUDED.sample_count, win_count = EXCLUDED.win_count,"
                "  sum_net_return = EXCLUDED.sum_net_return,"
                "  decayed_count = EXCLUDED.decayed_count, decayed_wins = EXCLUDED.decayed_wins,"
                "  decayed_return = EXCLUDED.decayed_return,"
                "  last_matured_at_ms = EXCLUDED.last_matured_at_ms, updated_at = now()",
                kind, scope_key, c.sample_count, c.win_count, c.sum_net_return, c.decayed_count,
                c.decayed_wins, c.decayed_return, c.last_matured_at_ms);
        }
        txn.exec_params(
            "UPDATE altsignalquality.prior_state SET matured_watermark_ms = $1,"
            " half_life_days = $2, decay_anchor_ms = $3, updated_at = now() WHERE singleton",
            watermark_ms, acc.config().half_life_days, acc.anchor_ms());
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

Result<std::vector<domain::MaturedOutcome>> Store::load_matured_outcomes(int64_t after_ms,
                                                                         int64_t until_ms,
                                                                         int limit) {
    std::vector<domain::MaturedOutcome> out;
    const std::string labelable = domain::labelable_outcomes_sql_list();
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        // Maturation time is GREATEST(resolved, time_stop): the outcome is only
        // treated as knowable once the FULL maturation window has elapsed, which
        // is exactly AltStrategy's own rule. Deriving it from the price path and
        // the plan (never from when the row happened to be written) keeps the
        // number stable across an outcome recompute.
        for (const auto& r : txn.exec_params(
                 "SELECT GREATEST(COALESCE(o.resolved_time_ms,0), p.time_stop_at_ms) AS matured,"
                 "       p.signal_proposal_id, p.strategy_id, p.canonical_pair_id,"
                 "       COALESCE(cs.global_regime,''), p.proposal_grade, p.side,"
                 "       o.outcome, o.realized_net_return"
                 " FROM " + altstrategy_ + ".market_outcomes o"
                 " JOIN " + altstrategy_ + ".trade_signal_proposals p"
                 "   ON p.signal_proposal_id = o.signal_proposal_id"
                 " LEFT JOIN " + altstrategy_ + ".strategy_candidates sc"
                 "   ON sc.candidate_id = p.candidate_id"
                 " LEFT JOIN " + altstrategy_ + ".decision_context_snapshots cs"
                 "   ON cs.context_id = sc.context_id"
                 " WHERE NOT o.is_counterfactual"
                 "   AND o.outcome IN " + labelable +
                 "   AND o.realized_net_return IS NOT NULL"
                 "   AND p.proposal_grade <> 'INVALID'"
                 "   AND p.plan_version = " + std::to_string(kLivePlanVersion) +
                 "   AND p.stop_distance_bps <= $4"
                 "   AND GREATEST(COALESCE(o.resolved_time_ms,0), p.time_stop_at_ms) > $1"
                 "   AND GREATEST(COALESCE(o.resolved_time_ms,0), p.time_stop_at_ms) <= $2"
                 " ORDER BY matured, p.signal_proposal_id LIMIT $3",
                 after_ms, until_ms, limit, max_stop_distance_bps_)) {
            domain::MaturedOutcome o;
            o.matured_at_ms = r[0].as<int64_t>();
            o.signal_proposal_id = r[1].as<std::string>();
            o.strategy_id = r[2].as<std::string>();
            o.canonical_pair_id = r[3].as<int64_t>();
            o.regime = r[4].as<std::string>();
            o.grade = r[5].as<std::string>();
            o.side = r[6].as<std::string>() == "short" ? 2 : 1;
            domain::OutcomeRecord rec;
            rec.categorical = r[7].as<std::string>();
            rec.net_return_after_costs = r[8].as<double>();
            rec.net_return_present = true;
            const auto label = domain::label_of(rec);
            if (!label.usable) continue;
            o.label = label.label;
            o.net_return = rec.net_return_after_costs;
            out.push_back(std::move(o));
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

// ---------------------------------------------------------------------------
// dataset
// ---------------------------------------------------------------------------

namespace {

// The proposal columns a backfilled candidate is rebuilt from.
constexpr const char* kArchivedCandidateColumns =
    " p.signal_proposal_id, p.candidate_id, COALESCE(p.ranking_id,''),"
    " p.canonical_pair_id, p.bitget_instrument_id, p.strategy_id, p.strategy_version,"
    " p.parameter_set_id, p.side, p.proposal_grade, p.plan_version, p.holding_class,"
    " p.quality, p.created_at_ms, p.valid_until_ms, p.entry_type,"
    " COALESCE(p.entry_price,0), COALESCE(p.trigger_price,0), p.max_slippage_bps,"
    " p.post_only_allowed, p.expected_fill_time_p50_min, p.expected_fill_time_p90_min,"
    " p.entry_fill_timeout_ms, p.stop_loss_price, p.stop_distance_bps, p.stop_distance_atr,"
    " p.take_profit_price, p.target_distance_bps, p.expected_holding_time_p50_ms,"
    " p.expected_holding_time_p90_ms, p.max_holding_time_ms, p.time_stop_at_ms,"
    // Economics are NOT coalesced to zero. AltStrategy writes NULL for them when
    // no model produced an estimate, and COALESCE(...,0) turned that absence
    // into a measured "expected net return of zero" on every archived row.
    " COALESCE(p.edge_estimated,false), p.p_target_before_stop,"
    " p.expected_net_return, p.expected_rr, p.ml_conviction, p.ml_abstain,"
    " p.strategy_confidence, p.ensemble_agreement, p.adaptive_score,"
    " array_length(p.risk_flags,1), array_length(p.reason_codes,1),"
    " array_length(p.alternative_plan_ids,1), p.recommended_for_execution,"
    " COALESCE(sc.context_id,''), p.context_revision, COALESCE(p.feature_manifest_hash,''),"
    " (SELECT count(*) FROM %SCHEMA%.proposal_warnings w"
    "  WHERE w.signal_proposal_id = p.signal_proposal_id) ";

std::string archived_candidate_columns(const std::string& schema) {
    std::string out = kArchivedCandidateColumns;
    const std::string token = "%SCHEMA%";
    for (size_t at = out.find(token); at != std::string::npos; at = out.find(token)) {
        out.replace(at, token.size(), schema);
    }
    return out;
}

// Parse the block above, starting at `base`. One parser, so the two call sites
// cannot disagree about what column 15 is.
domain::CandidateSignal parse_archived_candidate(const pqxx::row& r, int base) {
    const auto at = [&](int i) { return r[base + i]; };
    domain::CandidateSignal c;
    c.signal_proposal_id = at(0).as<std::string>();
    c.candidate_id = at(1).as<std::string>();
    c.ranking_id = at(2).as<std::string>();
    c.canonical_pair_id = at(3).as<int64_t>();
    c.bitget_instrument_id = at(4).as<int64_t>();
    c.strategy_id = at(5).as<std::string>();
    c.strategy_version = at(6).as<std::string>();
    c.parameter_set_id = at(7).as<std::string>();
    c.side = at(8).as<std::string>() == "short" ? 2 : 1;
    c.proposal_grade = at(9).as<std::string>();
    c.plan_version = at(10).as<int32_t>();
    {
        // The archive stores the holding class by NAME; the feature contract
        // encodes the enum.
        const std::string hc = at(11).as<std::string>();
        c.holding_class = hc == "SCALP" ? 1 : hc == "EXTENDED_INTRADAY" ? 3 : 2;
    }
    c.quality = at(12).as<std::string>();
    c.created_at_ms = at(13).as<int64_t>();
    c.valid_until_ms = at(14).as<int64_t>();
    {
        // AltStrategy persists these tokens in LOWER case
        // (engine/decision_types.cpp: to_string(EntryType)), unlike its
        // grade/quality/holding-class tokens. Matching the wrong case here
        // would silently label every backfilled row LIMIT.
        const std::string et = at(15).as<std::string>();
        c.entry_type = et == "market" ? 1 : et == "stop_market" ? 3
                       : et == "stop_limit" ? 4 : 2;
    }
    c.entry_price = at(16).as<double>();
    c.trigger_price = at(17).as<double>();
    c.max_slippage_bps = at(18).as<double>();
    c.post_only_allowed = at(19).as<bool>();
    c.expected_fill_time_p50_min = at(20).as<double>();
    c.expected_fill_time_p90_min = at(21).as<double>();
    c.entry_fill_timeout_ms = at(22).as<int64_t>();
    c.stop_loss_price = at(23).as<double>();
    c.stop_distance_bps = at(24).as<double>();
    c.stop_distance_atr = at(25).as<double>();
    c.take_profit_price = at(26).as<double>();
    c.target_distance_bps = at(27).as<double>();
    c.expected_holding_time_p50_ms = at(28).as<int64_t>();
    c.expected_holding_time_p90_ms = at(29).as<int64_t>();
    c.max_holding_time_ms = at(30).as<int64_t>();
    c.time_stop_at_ms = at(31).as<int64_t>();
    // A NULL economics figure means nobody estimated it. edge_estimated says so
    // explicitly, and the values stay at their defaults so nothing downstream
    // reads a zero as an estimate -- the feature extractor gates on the flag.
    c.edge_estimated = at(32).as<bool>();
    if (!at(33).is_null()) c.p_target_before_stop = at(33).as<double>();   // NULL stays absent
    if (!at(34).is_null()) c.expected_net_return = at(34).as<double>();
    c.expected_rr = at(35).as<double>();
    if (!at(36).is_null()) c.ml_conviction = at(36).as<double>();
    c.ml_abstain = at(37).as<bool>();
    c.strategy_confidence = at(38).as<double>();
    c.ensemble_agreement = at(39).as<double>();
    c.adaptive_score = at(40).as<double>();
    c.risk_flag_count = at(41).is_null() ? 0 : at(41).as<int32_t>();
    c.reason_code_count = at(42).is_null() ? 0 : at(42).as<int32_t>();
    c.alternative_plan_count = at(43).is_null() ? 0 : at(43).as<int32_t>();
    c.recommended_for_execution = at(44).as<bool>();
    c.context_id = at(45).as<std::string>();
    c.context_revision = at(46).as<int64_t>();
    c.feature_manifest_hash = at(47).as<std::string>();
    c.warning_count = static_cast<int32_t>(at(48).as<int64_t>());
    return c;
}

constexpr int kArchivedCandidateColumnCount = 49;

// The outcome columns every matured-signal query selects, in one order.
constexpr const char* kMaturedOutcomeColumns =
    " GREATEST(COALESCE(o.resolved_time_ms,0), p.time_stop_at_ms) AS matured,"
    " o.outcome, o.realized_net_return, o.mfe, o.mae, o.holding_time_ms,"
    " COALESCE(cs.available_at_ms, p.created_at_ms), COALESCE(cs.global_regime,''),"
    " COALESCE(ase.example_id,''), COALESCE(ev.symbol,'') ";

MaturedSignal parse_matured_outcome(const pqxx::row& r, int base,
                                    const domain::CandidateSignal& c) {
    const auto at = [&](int i) { return r[base + i]; };
    MaturedSignal s;
    s.signal_proposal_id = c.signal_proposal_id;
    s.candidate_id = c.candidate_id;
    s.canonical_pair_id = c.canonical_pair_id;
    s.strategy_id = c.strategy_id;
    s.strategy_version = c.strategy_version;
    s.parameter_set_id = c.parameter_set_id;
    s.side = c.side == 2 ? "short" : "long";
    s.proposal_grade = c.proposal_grade;
    s.upstream_recommended = c.recommended_for_execution;
    s.context_id = c.context_id;
    s.observation_time_ms = c.created_at_ms;
    s.label_matured_at_ms = at(0).as<int64_t>();
    s.categorical_outcome = at(1).as<std::string>();
    s.net_return = at(2).as<double>();
    s.net_return_present = true;
    s.mfe = opt_double(at(3));
    s.mae = opt_double(at(4));
    if (!at(5).is_null()) s.holding_duration_ms = at(5).as<int64_t>();
    s.available_at_ms = at(6).as<int64_t>();
    s.regime = at(7).as<std::string>();
    s.altstrategy_example_id = at(8).as<std::string>();
    s.symbol = at(9).as<std::string>();
    if (s.symbol.empty()) s.symbol = "pair-" + std::to_string(s.canonical_pair_id);
    return s;
}

constexpr int kMaturedOutcomeColumnCount = 10;

} // namespace

Result<std::vector<Store::BackfillRow>> Store::load_backfill_rows(
    const std::string& feature_schema_version, const std::string& label_spec_version,
    int64_t not_before_ms, int64_t max_evaluation_lag_ms, int limit) {
    std::vector<BackfillRow> out;
    const std::string labelable = domain::labelable_outcomes_sql_list();
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const std::string sql =
            "SELECT" + archived_candidate_columns(altstrategy_) + "," +
            kMaturedOutcomeColumns + "," + kContextColumns +
            " FROM " + altstrategy_ + ".trade_signal_proposals p"
            " JOIN " + altstrategy_ + ".market_outcomes o"
            "   ON o.signal_proposal_id = p.signal_proposal_id AND NOT o.is_counterfactual"
            " LEFT JOIN " + altstrategy_ + ".strategy_candidates sc"
            "   ON sc.candidate_id = p.candidate_id"
            " LEFT JOIN " + altstrategy_ + ".decision_context_snapshots cs"
            "   ON cs.context_id = sc.context_id"
            " LEFT JOIN " + altstrategy_ + ".signal_training_examples ase"
            "   ON ase.signal_proposal_id = p.signal_proposal_id"
            " LEFT JOIN altsignalquality.signal_evaluations ev"
            "   ON ev.signal_proposal_id = p.signal_proposal_id"
            " WHERE p.proposal_grade <> 'INVALID'"
            "   AND p.plan_version = " + std::to_string(kLivePlanVersion) +
            "   AND o.outcome IN " + labelable +
            "   AND o.realized_net_return IS NOT NULL"
            "   AND p.stop_distance_bps <= $4"
            "   AND p.created_at_ms >= $1"
            // Never evaluated, evaluated under an older contract, or evaluated
            // too late for its stored priors to be as-of (see LiveRow).
            "   AND (ev.signal_proposal_id IS NULL"
            "        OR ev.feature_schema_version <> $5"
            "        OR ev.evaluated_at_ms - ev.signal_created_at_ms > $6)"
            "   AND NOT EXISTS (SELECT 1 FROM altsignalquality.training_examples t"
            "                   WHERE t.signal_proposal_id = p.signal_proposal_id"
            "                     AND t.feature_schema_version = $5"
            "                     AND t.label_spec_version = $2)"
            " ORDER BY p.created_at_ms LIMIT $3";
        for (const auto& r : txn.exec_params(sql, not_before_ms, label_spec_version, limit,
                                             max_stop_distance_bps_, feature_schema_version,
                                             max_evaluation_lag_ms)) {
            BackfillRow row;
            row.candidate = parse_archived_candidate(r, 0);
            row.signal = parse_matured_outcome(r, kArchivedCandidateColumnCount, row.candidate);
            const int ctx_base = kArchivedCandidateColumnCount + kMaturedOutcomeColumnCount;
            if (!r[ctx_base].is_null()) row.context = parse_context_row(r, ctx_base);
            out.push_back(std::move(row));
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::vector<Store::LiveRow>> Store::load_live_rows(
    const std::string& feature_schema_version, const std::string& label_spec_version,
    int64_t max_evaluation_lag_ms, int limit) {
    std::vector<LiveRow> out;
    std::string parse_error;
    const std::string labelable = domain::labelable_outcomes_sql_list();
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const std::string sql =
            "SELECT" + archived_candidate_columns(altstrategy_) + "," +
            kMaturedOutcomeColumns +
            ", ev.features::text, ev.validity_bitmap::text, ev.missing_feature_count,"
            "  ev.feature_schema_hash"
            " FROM " + altstrategy_ + ".trade_signal_proposals p"
            " JOIN " + altstrategy_ + ".market_outcomes o"
            "   ON o.signal_proposal_id = p.signal_proposal_id AND NOT o.is_counterfactual"
            " JOIN altsignalquality.signal_evaluations ev"
            "   ON ev.signal_proposal_id = p.signal_proposal_id"
            "  AND ev.feature_schema_version = $4"
            // The prior features in a stored vector are only as-of when the
            // evaluation followed the signal promptly; see LiveRow.
            "  AND ev.evaluated_at_ms - ev.signal_created_at_ms <= $5"
            " LEFT JOIN " + altstrategy_ + ".strategy_candidates sc"
            "   ON sc.candidate_id = p.candidate_id"
            " LEFT JOIN " + altstrategy_ + ".decision_context_snapshots cs"
            "   ON cs.context_id = sc.context_id"
            " LEFT JOIN " + altstrategy_ + ".signal_training_examples ase"
            "   ON ase.signal_proposal_id = p.signal_proposal_id"
            " WHERE p.proposal_grade <> 'INVALID'"
            "   AND p.plan_version = " + std::to_string(kLivePlanVersion) +
            "   AND o.outcome IN " + labelable +
            "   AND o.realized_net_return IS NOT NULL"
            "   AND p.stop_distance_bps <= $3"
            "   AND NOT EXISTS (SELECT 1 FROM altsignalquality.training_examples t"
            "                   WHERE t.signal_proposal_id = p.signal_proposal_id"
            "                     AND t.feature_schema_version = $4"
            "                     AND t.label_spec_version = $1)"
            " ORDER BY p.created_at_ms LIMIT $2";
        for (const auto& r : txn.exec_params(sql, label_spec_version, limit,
                                             max_stop_distance_bps_, feature_schema_version,
                                             max_evaluation_lag_ms)) {
            LiveRow row;
            const auto candidate = parse_archived_candidate(r, 0);
            row.signal = parse_matured_outcome(r, kArchivedCandidateColumnCount, candidate);
            const int base = kArchivedCandidateColumnCount + kMaturedOutcomeColumnCount;
            try {
                row.features.values = pgarray::parse_float_array(r[base].as<std::string>());
                row.features.validity = pgarray::parse_hex_bytea(r[base + 1].as<std::string>());
                row.features.missing_count = r[base + 2].as<int>();
            } catch (const std::exception& e) {
                parse_error = row.signal.signal_proposal_id + ": " + e.what();
                return;
            }
            if (row.features.values.size() != domain::kFeatureCount) {
                parse_error = row.signal.signal_proposal_id + ": stored vector has " +
                              std::to_string(row.features.values.size()) +
                              " values, contract declares " +
                              std::to_string(domain::kFeatureCount);
                return;
            }
            if (r[base + 3].as<std::string>() != domain::feature_schema_hash()) {
                // Same version string, different hash: the contract was
                // redefined without a version bump somewhere. Refuse the row
                // rather than train on a vector whose meaning is unknown.
                parse_error = row.signal.signal_proposal_id +
                              ": stored vector was written under a different schema hash";
                return;
            }
            out.push_back(std::move(row));
        }
    });
    if (!res) return std::unexpected(res.error());
    if (!parse_error.empty()) return fail(ErrorCode::validation_error, parse_error);
    return out;
}

Result<int64_t> Store::insert_training_examples(const std::vector<TrainingExampleWrite>& rows,
                                                const std::string& feature_schema_version,
                                                const std::string& feature_schema_hash,
                                                const std::string& label_spec_version) {
    int64_t inserted = 0;
    if (rows.empty()) return inserted;

    // The domain invariants the table also enforces, checked HERE first.
    //
    // A row that violates one would abort the whole transaction, and the query
    // that produced it would return the same batch on the next pass — so a
    // single malformed upstream row would stop the dataset growing, forever,
    // with nothing but a repeated SQL error to show for it. Rejecting the row
    // and recording why keeps the batch moving; the CHECK constraints remain as
    // the second line of defence.
    std::vector<const TrainingExampleWrite*> usable;
    usable.reserve(rows.size());
    for (const auto& w : rows) {
        const auto& s = w.signal;
        std::string reject;
        if (s.label_matured_at_ms <= s.observation_time_ms) {
            reject = "label matured at or before the signal";
        } else if (s.observation_time_ms <= 0 || s.available_at_ms <= 0) {
            reject = "non-positive time anchor";
        } else if (s.side != "long" && s.side != "short") {
            reject = "side is neither long nor short";
        } else if (w.label != 0 && w.label != 1) {
            reject = "label is not binary";
        } else if (!std::isfinite(s.net_return)) {
            reject = "net return is not finite";
        } else if (w.features.values.size() != domain::kFeatureCount) {
            reject = "feature vector width does not match the contract";
        }
        if (!reject.empty()) {
            log::warn("training_example_rejected",
                      {{"signal_proposal_id", s.signal_proposal_id}, {"reason", reject}});
            continue;
        }
        usable.push_back(&w);
    }
    if (usable.empty()) return inserted;

    auto res = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto* row_ptr : usable) {
            const auto& w = *row_ptr;
            const auto& s = w.signal;
            const std::string example_id = make_example_id(s.signal_proposal_id,
                                                           feature_schema_version,
                                                           label_spec_version);
            const auto r = txn.exec_params(
                "INSERT INTO altsignalquality.training_examples ("
                " example_id, signal_proposal_id, candidate_id, canonical_pair_id, symbol,"
                " strategy_id, strategy_version, parameter_set_id, side, proposal_grade,"
                " upstream_recommended, regime, observation_time_ms, available_at_ms,"
                " label_matured_at_ms, feature_schema_version, feature_schema_hash, features,"
                " validity_bitmap, label_spec_version, label, categorical_outcome,"
                " net_return_after_costs, mfe, mae, holding_duration_ms,"
                " altstrategy_example_id, origin, dataset_eligible)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,"
                "         $18::real[],$19::bytea,$20,$21,$22,$23,$24,$25,$26,$27,$28,true)"
                " ON CONFLICT (signal_proposal_id, feature_schema_version, label_spec_version)"
                " DO NOTHING RETURNING 1",
                example_id, s.signal_proposal_id, s.candidate_id, s.canonical_pair_id, s.symbol,
                s.strategy_id, s.strategy_version, s.parameter_set_id, s.side, s.proposal_grade,
                s.upstream_recommended, s.regime, s.observation_time_ms, s.available_at_ms,
                s.label_matured_at_ms, feature_schema_version, feature_schema_hash,
                pgarray::float_array_literal(w.features.values),
                pgarray::bytea_hex_literal(w.features.validity), label_spec_version, w.label,
                s.categorical_outcome, s.net_return, s.mfe, s.mae, s.holding_duration_ms,
                s.altstrategy_example_id.empty() ? std::optional<std::string>()
                                                 : std::optional(s.altstrategy_example_id),
                w.origin);
            if (!r.empty()) ++inserted;
        }
    });
    if (!res) return std::unexpected(res.error());
    return inserted;
}

Result<std::vector<DatasetRow>> Store::load_dataset(const std::string& feature_schema_version,
                                                    const std::string& label_spec_version,
                                                    int64_t from_ms, int64_t max_rows) {
    std::vector<DatasetRow> out;
    std::string parse_error;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& r : txn.exec_params(
                 "SELECT example_id, signal_proposal_id, strategy_id, symbol, canonical_pair_id,"
                 "       regime, side, proposal_grade, upstream_recommended,"
                 "       observation_time_ms, label_matured_at_ms, features::text, label,"
                 "       net_return_after_costs, categorical_outcome"
                 " FROM altsignalquality.training_examples"
                 " WHERE feature_schema_version = $1 AND label_spec_version = $2"
                 "   AND dataset_eligible AND observation_time_ms >= $3"
                 " ORDER BY observation_time_ms, signal_proposal_id LIMIT $4",
                 feature_schema_version, label_spec_version, from_ms, max_rows)) {
            DatasetRow d;
            d.example_id = r[0].as<std::string>();
            d.signal_proposal_id = r[1].as<std::string>();
            d.strategy_id = r[2].as<std::string>();
            d.symbol = r[3].as<std::string>();
            d.canonical_pair_id = r[4].as<int64_t>();
            d.regime = r[5].as<std::string>();
            d.side = r[6].as<std::string>();
            d.grade = r[7].as<std::string>();
            d.upstream_recommended = r[8].as<bool>();
            d.observation_time_ms = r[9].as<int64_t>();
            d.label_matured_at_ms = r[10].as<int64_t>();
            try {
                d.features = pgarray::parse_float_array(r[11].as<std::string>());
            } catch (const std::exception& e) {
                parse_error = std::string("training example ") + d.example_id + ": " + e.what();
                return;
            }
            if (d.features.size() != domain::kFeatureCount) {
                parse_error = "training example " + d.example_id + " has " +
                              std::to_string(d.features.size()) + " features, contract declares " +
                              std::to_string(domain::kFeatureCount);
                return;
            }
            d.label = r[12].as<int>();
            d.net_return = r[13].as<double>();
            d.categorical_outcome = r[14].as<std::string>();
            out.push_back(std::move(d));
        }
    });
    if (!res) return std::unexpected(res.error());
    if (!parse_error.empty()) return fail(ErrorCode::validation_error, parse_error);
    return out;
}

Result<int64_t> Store::count_dataset(const std::string& feature_schema_version,
                                     const std::string& label_spec_version, int64_t from_ms) {
    int64_t n = 0;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT count(*) FROM altsignalquality.training_examples"
            " WHERE feature_schema_version = $1 AND label_spec_version = $2"
            "   AND dataset_eligible AND observation_time_ms >= $3",
            feature_schema_version, label_spec_version, from_ms);
        n = rows[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return n;
}

Result<void> Store::record_dataset_version(const std::string& dataset_version,
                                           const std::string& feature_schema_version,
                                           const std::string& label_spec_version, int64_t rows,
                                           int64_t positives, int64_t from_ms, int64_t to_ms,
                                           const std::string& content_hash) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altsignalquality.dataset_versions ("
            " dataset_version, feature_schema_version, label_spec_version, row_count,"
            " positive_count, from_time_ms, to_time_ms, content_hash)"
            " VALUES ($1,$2,$3,$4,$5,$6,$7,$8) ON CONFLICT (dataset_version) DO NOTHING",
            dataset_version, feature_schema_version, label_spec_version, rows, positives, from_ms,
            to_ms, content_hash);
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

// ---------------------------------------------------------------------------
// model registry
// ---------------------------------------------------------------------------

Result<std::optional<ActiveModel>> Store::load_active_model(const std::string& objective) {
    std::optional<ActiveModel> out;
    std::string parse_error;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT m.model_id, m.model_version, a.path, a.checksum, m.feature_schema_version,"
            "       m.feature_schema_hash, c.feature_count, m.decision_threshold,"
            "       m.calibration_version, COALESCE(cal.method,'identity'),"
            "       COALESCE(cal.knots::text,'{}'),"
            "       COALESCE((extract(epoch FROM m.activated_at) * 1000)::bigint, 0)"
            " FROM altsignalquality.models m"
            " JOIN altsignalquality.model_artifacts a"
            "   ON a.model_id = m.model_id AND a.kind = 'model'"
            " JOIN altsignalquality.model_feature_compatibility c"
            "   ON c.model_id = m.model_id AND c.feature_schema_version = m.feature_schema_version"
            " LEFT JOIN altsignalquality.model_calibration cal ON cal.model_id = m.model_id"
            " WHERE m.status = 'ACTIVE' AND m.algorithm = 'lightgbm' AND m.objective = $1"
            " LIMIT 1",
            objective);
        if (rows.empty()) return;
        ActiveModel m;
        m.model_id = rows[0][0].as<std::string>();
        m.model_version = rows[0][1].as<int32_t>();
        m.artifact_path = rows[0][2].as<std::string>();
        m.artifact_checksum = rows[0][3].as<std::string>();
        m.feature_schema_version = rows[0][4].as<std::string>();
        m.feature_schema_hash = rows[0][5].as<std::string>();
        m.feature_count = rows[0][6].as<int32_t>();
        m.decision_threshold = rows[0][7].as<double>();
        m.calibration_version = rows[0][8].as<std::string>();
        m.calibration_method = rows[0][9].as<std::string>();
        m.activated_at_ms = rows[0][11].as<int64_t>();
        try {
            const json knots = json::parse(rows[0][10].as<std::string>());
            if (knots.is_object() && knots.contains("x") && knots.contains("y")) {
                m.calibration_x = knots["x"].get<std::vector<double>>();
                m.calibration_y = knots["y"].get<std::vector<double>>();
            }
        } catch (const json::exception& e) {
            parse_error = std::string("calibration knots are not readable: ") + e.what();
            return;
        }
        if (m.calibration_method == "isotonic" &&
            (m.calibration_x.empty() || m.calibration_x.size() != m.calibration_y.size())) {
            parse_error = "model " + m.model_id +
                          " claims isotonic calibration but its knots are missing or ragged";
            return;
        }
        out = std::move(m);
    });
    if (!res) return std::unexpected(res.error());
    if (!parse_error.empty()) return fail(ErrorCode::model_error, parse_error);
    return out;
}

Result<void> Store::register_model(const ModelRegistration& reg,
                                   const std::vector<ArtifactRecord>& artifacts,
                                   const std::vector<FoldRecord>& folds,
                                   const domain::Calibration& calibration,
                                   const std::string& calibration_metrics_json,
                                   const std::vector<std::pair<std::string, double>>&
                                       scoped_metrics,
                                   const std::string& trained_detail_json) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altsignalquality.models ("
            " model_id, algorithm, objective, feature_schema_version, feature_schema_hash,"
            " label_spec_version, dataset_version, hyperparameters, decision_threshold,"
            " calibration_version, status, training_period, validation_period, test_period)"
            " VALUES ($1,$2,$3,$4,$5,$6,$7,$8::jsonb,$9,$10,'CANDIDATE',"
            "   tstzrange(to_timestamp($11::bigint/1000.0), to_timestamp($12::bigint/1000.0),'[)'),"
            "   tstzrange(to_timestamp($13::bigint/1000.0), to_timestamp($14::bigint/1000.0),'[)'),"
            "   tstzrange(to_timestamp($15::bigint/1000.0), to_timestamp($16::bigint/1000.0),'[)'))",
            reg.model_id, reg.algorithm, reg.objective, reg.feature_schema_version,
            reg.feature_schema_hash, reg.label_spec_version, reg.dataset_version,
            reg.hyperparameters_json, reg.decision_threshold, reg.calibration_version,
            reg.train_from_ms, reg.train_to_ms, reg.valid_from_ms, reg.valid_to_ms,
            reg.test_from_ms, reg.test_to_ms);
        for (const auto& a : artifacts) {
            txn.exec_params(
                "INSERT INTO altsignalquality.model_artifacts"
                " (model_id, kind, path, format, checksum, size_bytes)"
                " VALUES ($1,$2,$3,$4,$5,$6) ON CONFLICT (model_id, kind) DO NOTHING",
                reg.model_id, a.kind, a.path, a.format, a.checksum, a.size_bytes);
        }
        for (const auto& [scoped, value] : scoped_metrics) {
            if (!std::isfinite(value)) continue;
            const auto sep = scoped.find(':');
            const std::string scope = sep == std::string::npos ? "test" : scoped.substr(0, sep);
            const std::string metric = sep == std::string::npos ? scoped : scoped.substr(sep + 1);
            txn.exec_params(
                "INSERT INTO altsignalquality.model_metrics (model_id, scope, metric, value)"
                " VALUES ($1,$2,$3,$4) ON CONFLICT (model_id, scope, metric) DO NOTHING",
                reg.model_id, scope, metric, value);
        }
        for (const auto& f : folds) {
            txn.exec_params(
                "INSERT INTO altsignalquality.model_walk_forward_runs"
                " (model_id, fold, train_from, train_to, valid_from, valid_to, test_from,"
                "  test_to, metrics)"
                " VALUES ($1,$2, to_timestamp($3::bigint/1000.0), to_timestamp($4::bigint/1000.0),"
                "         to_timestamp($5::bigint/1000.0), to_timestamp($6::bigint/1000.0),"
                "         to_timestamp($7::bigint/1000.0), to_timestamp($8::bigint/1000.0),"
                "         $9::jsonb)"
                " ON CONFLICT (model_id, fold) DO NOTHING",
                reg.model_id, f.fold, f.train_from_ms, f.train_to_ms, f.valid_from_ms,
                f.valid_to_ms, f.test_from_ms, f.test_to_ms, f.metrics_json);
        }
        txn.exec_params(
            "INSERT INTO altsignalquality.model_feature_compatibility"
            " (model_id, feature_schema_version, schema_hash, feature_count)"
            " VALUES ($1,$2,$3,$4) ON CONFLICT (model_id, feature_schema_version) DO NOTHING",
            reg.model_id, reg.feature_schema_version, reg.feature_schema_hash, reg.feature_count);
        const json knots = {{"x", calibration.x}, {"y", calibration.y}};
        txn.exec_params(
            "INSERT INTO altsignalquality.model_calibration"
            " (model_id, calibration_version, method, knots, metrics)"
            " VALUES ($1,$2,$3,$4::jsonb,$5::jsonb) ON CONFLICT (model_id) DO NOTHING",
            reg.model_id, calibration.version, calibration.method, knots.dump(),
            calibration_metrics_json);
        txn.exec_params(
            "INSERT INTO altsignalquality.model_events (model_id, event, detail, actor)"
            " VALUES ($1,'trained',$2::jsonb,'altsignalquality-trainer')",
            reg.model_id, trained_detail_json);
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

Result<void> Store::reject_model(const std::string& model_id, const std::string& detail_json) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params("UPDATE altsignalquality.models SET status = 'REJECTED'"
                        " WHERE model_id = $1", model_id);
        txn.exec_params(
            "INSERT INTO altsignalquality.model_events (model_id, event, detail, actor)"
            " VALUES ($1,'rejected',$2::jsonb,'altsignalquality-trainer')",
            model_id, detail_json);
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

Result<void> Store::approve_model(const std::string& model_id) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params("UPDATE altsignalquality.models SET status = 'APPROVED',"
                        " approved_at = now() WHERE model_id = $1", model_id);
        txn.exec_params(
            "INSERT INTO altsignalquality.model_events (model_id, event, detail, actor)"
            " VALUES ($1,'approved','{\"note\":\"gates passed; awaiting operator activation\"}',"
            " 'altsignalquality-trainer')",
            model_id);
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

Result<std::string> Store::promote_model(const std::string& model_id,
                                         const std::string& objective,
                                         const std::string& checksum) {
    std::string previous;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const auto locked = txn.exec_params(
            "SELECT status FROM altsignalquality.models WHERE model_id = $1 FOR UPDATE", model_id);
        if (locked.empty() || locked[0][0].as<std::string>() != "CANDIDATE") {
            throw std::runtime_error("candidate status changed during promotion");
        }
        const auto prev = txn.exec_params(
            "SELECT model_id FROM altsignalquality.models"
            " WHERE status = 'ACTIVE' AND objective = $1 FOR UPDATE",
            objective);
        if (!prev.empty()) {
            previous = prev[0][0].as<std::string>();
            txn.exec_params("UPDATE altsignalquality.models SET status = 'RETIRED',"
                            " retired_at = now() WHERE model_id = $1", previous);
            txn.exec_params(
                "INSERT INTO altsignalquality.model_events (model_id, event, detail, actor)"
                " VALUES ($1,'retired',$2::jsonb,'altsignalquality-trainer')",
                previous, json({{"replaced_by", model_id}}).dump());
        }
        txn.exec_params(
            "UPDATE altsignalquality.models SET status = 'ACTIVE', approved_at = now(),"
            " activated_at = now(), retired_at = NULL WHERE model_id = $1",
            model_id);
        txn.exec_params(
            "INSERT INTO altsignalquality.model_events (model_id, event, detail, actor)"
            " VALUES ($1,'activated',$2::jsonb,'altsignalquality-trainer')",
            model_id, json({{"previous", previous}, {"checksum", checksum}}).dump());
    });
    if (!res) return std::unexpected(res.error());
    return previous;
}

Result<bool> Store::has_active_model(const std::string& objective) {
    bool active = false;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        active = txn.exec_params("SELECT EXISTS (SELECT 1 FROM altsignalquality.models"
                                 " WHERE status = 'ACTIVE' AND algorithm = 'lightgbm'"
                                 "   AND objective = $1)",
                                 objective)[0][0]
                     .as<bool>();
    });
    if (!res) return std::unexpected(res.error());
    return active;
}

Result<int64_t> Store::last_training_ms(const std::string& objective) {
    int64_t out = 0;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT (extract(epoch FROM max(created_at)) * 1000)::bigint"
            " FROM altsignalquality.models WHERE algorithm = 'lightgbm' AND objective = $1"
            "   AND status <> 'FAILED'",
            objective);
        if (!rows.empty() && !rows[0][0].is_null()) out = rows[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::string> Store::model_status_json(const std::string& objective, int limit) {
    json out;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        out["objective"] = objective;
        out["feature_schema_version"] = std::string(domain::kFeatureSchemaVersion);
        out["feature_schema_hash"] = domain::feature_schema_hash();
        out["label_spec_version"] = std::string(domain::kLabelSpecVersion);
        out["feature_count"] = static_cast<int64_t>(domain::kFeatureCount);

        json models = json::array();
        for (const auto& r : txn.exec_params(
                 "SELECT m.model_id, m.status, m.dataset_version, m.decision_threshold,"
                 "       m.calibration_version, m.feature_schema_version, m.feature_schema_hash,"
                 "       to_char(m.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SSZ'),"
                 "       COALESCE(to_char(m.activated_at, 'YYYY-MM-DD\"T\"HH24:MI:SSZ'),''),"
                 "       COALESCE(a.checksum,'')"
                 " FROM altsignalquality.models m"
                 " LEFT JOIN altsignalquality.model_artifacts a"
                 "   ON a.model_id = m.model_id AND a.kind = 'model'"
                 " WHERE m.objective = $1 ORDER BY m.created_at DESC LIMIT $2",
                 objective, limit)) {
            json m = {{"model_id", r[0].as<std::string>()},
                      {"status", r[1].as<std::string>()},
                      {"dataset_version", r[2].as<std::string>()},
                      {"decision_threshold", r[3].as<double>()},
                      {"calibration_version", r[4].as<std::string>()},
                      {"feature_schema_version", r[5].as<std::string>()},
                      {"feature_schema_hash", r[6].as<std::string>()},
                      {"created_at", r[7].as<std::string>()},
                      {"activated_at", r[8].as<std::string>()},
                      {"artifact_checksum", r[9].as<std::string>()}};
            json metrics = json::object();
            for (const auto& mr : txn.exec_params(
                     "SELECT scope, metric, value FROM altsignalquality.model_metrics"
                     " WHERE model_id = $1 ORDER BY scope, metric",
                     r[0].as<std::string>())) {
                metrics[mr[0].as<std::string>()][mr[1].as<std::string>()] = mr[2].as<double>();
            }
            m["metrics"] = std::move(metrics);
            json folds = json::array();
            for (const auto& fr : txn.exec_params(
                     "SELECT fold, to_char(train_from,'YYYY-MM-DD\"T\"HH24:MI:SSZ'),"
                     "       to_char(test_to,'YYYY-MM-DD\"T\"HH24:MI:SSZ'), metrics::text"
                     " FROM altsignalquality.model_walk_forward_runs"
                     " WHERE model_id = $1 ORDER BY fold",
                     r[0].as<std::string>())) {
                folds.push_back({{"fold", fr[0].as<int>()},
                                 {"train_from", fr[1].as<std::string>()},
                                 {"test_to", fr[2].as<std::string>()},
                                 {"metrics", parse_json_column(fr[3].as<std::string>())}});
            }
            m["walk_forward"] = std::move(folds);
            models.push_back(std::move(m));
        }
        out["models"] = std::move(models);

        const auto counts = txn.exec_params(
            "SELECT count(*) FILTER (WHERE dataset_eligible),"
            "       count(*) FILTER (WHERE dataset_eligible AND label = 1),"
            "       COALESCE(min(observation_time_ms),0), COALESCE(max(observation_time_ms),0)"
            " FROM altsignalquality.training_examples"
            " WHERE feature_schema_version = $1 AND label_spec_version = $2",
            std::string(domain::kFeatureSchemaVersion), std::string(domain::kLabelSpecVersion));
        out["dataset"] = {{"eligible_rows", counts[0][0].as<int64_t>()},
                          {"positive_rows", counts[0][1].as<int64_t>()},
                          {"from_time_ms", counts[0][2].as<int64_t>()},
                          {"to_time_ms", counts[0][3].as<int64_t>()}};

        const auto evals = txn.exec(
            "SELECT count(*), count(*) FILTER (WHERE decision = 'ACCEPTED'),"
            "       count(*) FILTER (WHERE decision = 'ACCEPTED' AND NOT published),"
            "       count(*) FILTER (WHERE has_quality_score)"
            " FROM altsignalquality.signal_evaluations");
        out["evaluations"] = {{"total", evals[0][0].as<int64_t>()},
                              {"accepted", evals[0][1].as<int64_t>()},
                              {"accepted_unpublished", evals[0][2].as<int64_t>()},
                              {"model_scored", evals[0][3].as<int64_t>()}};

        // Why decisions were taken, not just how many. A filter running mostly
        // on COLD_START or MODEL_UNUSABLE looks identical to a working one in
        // the counters above.
        json bases = json::object();
        for (const auto& r : txn.exec(
                 "SELECT decision_basis, decision, count(*)"
                 " FROM altsignalquality.signal_evaluations"
                 " GROUP BY decision_basis, decision ORDER BY decision_basis, decision")) {
            bases[r[0].as<std::string>()][r[1].as<std::string>()] = r[2].as<int64_t>();
        }
        out["decision_bases"] = std::move(bases);

        // The dataset versions a model can be reproduced from.
        json datasets = json::array();
        for (const auto& r : txn.exec_params(
                 "SELECT dataset_version, row_count, positive_count, content_hash,"
                 "       from_time_ms, to_time_ms"
                 " FROM altsignalquality.dataset_versions ORDER BY created_at DESC LIMIT $1",
                 limit)) {
            datasets.push_back({{"dataset_version", r[0].as<std::string>()},
                                {"rows", r[1].as<int64_t>()},
                                {"positives", r[2].as<int64_t>()},
                                {"content_hash", r[3].as<std::string>()},
                                {"from_time_ms", r[4].as<int64_t>()},
                                {"to_time_ms", r[5].as<int64_t>()}});
        }
        out["dataset_versions"] = std::move(datasets);

        // What the trainer has been doing, including the cycles that produced
        // nothing — a run of `rejected` is the single most useful signal that
        // the filter has no edge to find yet.
        json runs = json::array();
        for (const auto& r : txn.exec_params(
                 "SELECT run_id, trigger, status, COALESCE(model_id,''), rows_used,"
                 "       COALESCE(error,''), to_char(started_at,'YYYY-MM-DD\"T\"HH24:MI:SSZ'),"
                 "       COALESCE(detail::text,'{}')"
                 " FROM altsignalquality.training_runs ORDER BY started_at DESC LIMIT $1",
                 limit)) {
            runs.push_back({{"run_id", r[0].as<std::string>()},
                            {"trigger", r[1].as<std::string>()},
                            {"status", r[2].as<std::string>()},
                            {"model_id", r[3].as<std::string>()},
                            {"rows_used", r[4].as<int64_t>()},
                            {"error", r[5].as<std::string>()},
                            {"started_at", r[6].as<std::string>()},
                            {"detail", parse_json_column(r[7].as<std::string>())}});
        }
        out["training_runs"] = std::move(runs);

        // The lifecycle audit trail: which model went ACTIVE, when, and why a
        // candidate was refused.
        json events = json::array();
        for (const auto& r : txn.exec_params(
                 "SELECT model_id, event, actor,"
                 "       to_char(created_at,'YYYY-MM-DD\"T\"HH24:MI:SSZ'), detail::text"
                 " FROM altsignalquality.model_events ORDER BY created_at DESC LIMIT $1",
                 limit)) {
            events.push_back({{"model_id", r[0].as<std::string>()},
                              {"event", r[1].as<std::string>()},
                              {"actor", r[2].as<std::string>()},
                              {"created_at", r[3].as<std::string>()},
                              {"detail", parse_json_column(r[4].as<std::string>())}});
        }
        out["model_events"] = std::move(events);
    });
    if (!res) return std::unexpected(res.error());
    return out.dump(2);
}

Result<std::vector<std::pair<std::string, std::string>>> Store::list_model_statuses() {
    std::vector<std::pair<std::string, std::string>> out;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& r : txn.exec("SELECT model_id, status FROM altsignalquality.models")) {
            out.emplace_back(r[0].as<std::string>(), r[1].as<std::string>());
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::string> Store::begin_training_run(const std::string& trigger) {
    const std::string run_id =
        "run_" + std::to_string(timeutil::now_ms()) + "_" + trigger;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params("INSERT INTO altsignalquality.training_runs (run_id, trigger)"
                        " VALUES ($1,$2) ON CONFLICT (run_id) DO NOTHING",
                        run_id, trigger);
    });
    if (!res) return std::unexpected(res.error());
    return run_id;
}

Result<void> Store::finish_training_run(const std::string& run_id, const std::string& status,
                                        const std::string& model_id, int64_t rows,
                                        const std::string& detail_json,
                                        const std::string& error) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE altsignalquality.training_runs SET finished_at = now(), status = $2,"
            " model_id = $3, rows_used = $4, detail = $5::jsonb, error = $6 WHERE run_id = $1",
            run_id, status,
            model_id.empty() ? std::optional<std::string>() : std::optional(model_id), rows,
            detail_json.empty() ? std::string("{}") : detail_json,
            error.empty() ? std::optional<std::string>() : std::optional(error));
    });
    if (!res) return std::unexpected(res.error());
    return {};
}

// ---------------------------------------------------------------------------
// retention
// ---------------------------------------------------------------------------

Result<std::vector<Store::RetentionResult>> Store::run_retention(int64_t now_ms) {
    std::vector<RetentionResult> out;
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        struct Policy {
            std::string table;
            int retain_days;
            std::string time_column;
        };
        std::vector<Policy> policies;
        for (const auto& r : txn.exec(
                 "SELECT table_name, retain_days, time_column"
                 " FROM altsignalquality.retention_policies ORDER BY table_name")) {
            policies.push_back({r[0].as<std::string>(), r[1].as<int>(), r[2].as<std::string>()});
        }
        for (const auto& p : policies) {
            // Table and column names come from this service's OWN policy table,
            // which only the migration seeds; they are still validated as plain
            // identifiers before being interpolated, because a policy row is
            // data and data is never trusted as SQL.
            const auto valid = [](const std::string& s) {
                return !s.empty() &&
                       s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") ==
                           std::string::npos;
            };
            if (!valid(p.table) || !valid(p.time_column)) {
                log::error("retention_policy_rejected", {{"table", p.table}});
                continue;
            }
            const auto deleted = txn.exec_params(
                "WITH gone AS (DELETE FROM altsignalquality." + p.table +
                    " WHERE " + p.time_column + " < to_timestamp($1::bigint/1000.0)"
                    " RETURNING 1) SELECT count(*) FROM gone",
                now_ms - static_cast<int64_t>(p.retain_days) * 86'400'000LL);
            out.push_back({p.table, deleted[0][0].as<int64_t>()});
        }
        int64_t total = 0;
        json detail = json::object();
        for (const auto& r : out) {
            total += r.deleted_rows;
            detail[r.table_name] = r.deleted_rows;
        }
        txn.exec_params(
            "INSERT INTO altsignalquality.retention_runs (finished_at, rows_deleted, detail)"
            " VALUES (now(), $1, $2::jsonb)",
            total, detail.dump());
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

} // namespace altsignalquality::data
