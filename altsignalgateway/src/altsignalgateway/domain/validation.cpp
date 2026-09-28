#include "altsignalgateway/domain/validation.hpp"

#include <cmath>

namespace altsignalgateway::domain {

namespace as = altstrategy::v1;
namespace sq = altsignalquality::v1;

namespace {

// AltStrategy's own hard bound (chk_holding_limit): 48 hours.
constexpr int64_t kMaxHoldingTimeMs = 172'800'000;

ValidationResult reject(RejectionCode code, std::string detail) {
    return ValidationResult{code, std::move(detail), {}};
}

bool finite_positive(double v) { return std::isfinite(v) && v > 0.0; }

} // namespace

std::string_view rejection_code_name(RejectionCode c) {
    switch (c) {
        case RejectionCode::ok: return "OK";
        case RejectionCode::not_a_final_signal: return "NOT_A_FINAL_SIGNAL";
        case RejectionCode::unsupported_schema_version: return "UNSUPPORTED_SCHEMA_VERSION";
        case RejectionCode::malformed_envelope: return "MALFORMED_ENVELOPE";
        case RejectionCode::wrong_source_service: return "WRONG_SOURCE_SERVICE";
        case RejectionCode::missing_final_signal_id: return "MISSING_FINAL_SIGNAL_ID";
        case RejectionCode::missing_proposal: return "MISSING_PROPOSAL";
        case RejectionCode::missing_quality: return "MISSING_QUALITY";
        case RejectionCode::decision_not_accepted: return "DECISION_NOT_ACCEPTED";
        case RejectionCode::missing_signal_proposal_id: return "MISSING_SIGNAL_PROPOSAL_ID";
        case RejectionCode::missing_candidate_id: return "MISSING_CANDIDATE_ID";
        case RejectionCode::missing_strategy_id: return "MISSING_STRATEGY_ID";
        case RejectionCode::invalid_canonical_pair: return "INVALID_CANONICAL_PAIR";
        case RejectionCode::invalid_side: return "INVALID_SIDE";
        case RejectionCode::invalid_entry_type: return "INVALID_ENTRY_TYPE";
        case RejectionCode::invalid_grade: return "INVALID_GRADE";
        case RejectionCode::missing_created_at: return "MISSING_CREATED_AT";
        case RejectionCode::invalid_validity_window: return "INVALID_VALIDITY_WINDOW";
        case RejectionCode::non_finite_price: return "NON_FINITE_PRICE";
        case RejectionCode::invalid_stop_price: return "INVALID_STOP_PRICE";
        case RejectionCode::invalid_target_price: return "INVALID_TARGET_PRICE";
        case RejectionCode::invalid_entry_price: return "INVALID_ENTRY_PRICE";
        case RejectionCode::invalid_geometry: return "INVALID_GEOMETRY";
        case RejectionCode::invalid_entry_range: return "INVALID_ENTRY_RANGE";
        case RejectionCode::invalid_holding_time: return "INVALID_HOLDING_TIME";
        case RejectionCode::invalid_quality_score: return "INVALID_QUALITY_SCORE";
        case RejectionCode::payload_too_large: return "PAYLOAD_TOO_LARGE";
    }
    return "UNKNOWN";
}

ValidationResult validate_envelope(const sq::Envelope& env) {
    if (!env.has_final_trade_signal()) {
        return reject(RejectionCode::not_a_final_signal,
                      "envelope carries no final_trade_signal");
    }
    // The subject filter already narrows to signalquality.final.>, but a
    // subject is a routing convention and source_service is the producer's own
    // claim about itself. Both must agree.
    if (env.source_service() != "altsignalquality") {
        return reject(RejectionCode::wrong_source_service,
                      "source_service='" + env.source_service() + "'");
    }
    return {};
}

ValidationResult validate_final_signal(const sq::FinalTradeSignal& sig, int64_t now_ms) {
    ValidationResult out;

    if (sig.final_signal_id().empty()) {
        return reject(RejectionCode::missing_final_signal_id, "final_signal_id is empty");
    }
    if (!sig.has_proposal()) {
        return reject(RejectionCode::missing_proposal, "no proposal on the final signal");
    }
    if (!sig.has_quality()) {
        return reject(RejectionCode::missing_quality, "no quality evaluation on the final signal");
    }

    const as::TradeSignalProposal& p = sig.proposal();
    const sq::SignalQualityEvaluation& q = sig.quality();

    // --- the verdict ---------------------------------------------------------
    // A FINAL signal is by definition the accepted branch. A REJECTED one
    // arriving here means the upstream published on the wrong subject, and
    // forwarding it would put a trade AltSignalQuality declined onto the wire.
    if (q.decision() != sq::DECISION_ACCEPTED) {
        return reject(RejectionCode::decision_not_accepted,
                      "quality.decision=" + sq::QualityDecision_Name(q.decision()));
    }
    if (q.has_quality_score()) {
        const double s = q.quality_score();
        if (!std::isfinite(s) || s < 0.0 || s > 1.0) {
            return reject(RejectionCode::invalid_quality_score,
                          "quality_score=" + std::to_string(s));
        }
    }

    // --- identity ------------------------------------------------------------
    if (p.signal_proposal_id().empty()) {
        return reject(RejectionCode::missing_signal_proposal_id, "signal_proposal_id is empty");
    }
    if (p.candidate_id().empty()) {
        return reject(RejectionCode::missing_candidate_id, "candidate_id is empty");
    }
    if (p.strategy_id().empty()) {
        return reject(RejectionCode::missing_strategy_id, "strategy_id is empty");
    }
    if (p.canonical_pair_id() <= 0) {
        return reject(RejectionCode::invalid_canonical_pair,
                      "canonical_pair_id=" + std::to_string(p.canonical_pair_id()));
    }
    if (p.side() != as::SIDE_LONG && p.side() != as::SIDE_SHORT) {
        return reject(RejectionCode::invalid_side, "side=" + as::Side_Name(p.side()));
    }
    if (p.entry_type() == as::ENTRY_TYPE_UNSPECIFIED) {
        return reject(RejectionCode::invalid_entry_type, "entry_type is UNSPECIFIED");
    }
    // GRADE_INVALID is the one grade AltStrategy's own contract forbids from
    // being published as executable.
    if (p.proposal_grade() == as::GRADE_INVALID ||
        p.proposal_grade() == as::PROPOSAL_GRADE_UNSPECIFIED) {
        return reject(RejectionCode::invalid_grade,
                      "proposal_grade=" + as::ProposalGrade_Name(p.proposal_grade()));
    }

    // --- time ----------------------------------------------------------------
    if (p.created_at_ms() <= 0) {
        return reject(RejectionCode::missing_created_at, "created_at_ms is not set");
    }
    if (p.valid_until_ms() <= p.created_at_ms()) {
        return reject(RejectionCode::invalid_validity_window,
                      "valid_until_ms=" + std::to_string(p.valid_until_ms()) +
                          " <= created_at_ms=" + std::to_string(p.created_at_ms()));
    }
    if (p.max_holding_time_ms() <= 0 || p.max_holding_time_ms() > kMaxHoldingTimeMs) {
        return reject(RejectionCode::invalid_holding_time,
                      "max_holding_time_ms=" + std::to_string(p.max_holding_time_ms()));
    }

    // --- geometry ------------------------------------------------------------
    for (const auto& [name, value] :
         {std::pair<const char*, double>{"entry_price", p.entry_price()},
          {"stop_loss_price", p.stop_loss_price()},
          {"take_profit_price", p.take_profit_price()},
          {"entry_lower", p.entry_lower()},
          {"entry_upper", p.entry_upper()},
          {"trigger_price", p.trigger_price()},
          {"optional_secondary_target", p.optional_secondary_target()}}) {
        if (!std::isfinite(value)) {
            return reject(RejectionCode::non_finite_price,
                          std::string(name) + " is not finite");
        }
    }
    if (!finite_positive(p.entry_price())) {
        return reject(RejectionCode::invalid_entry_price,
                      "entry_price=" + std::to_string(p.entry_price()));
    }
    if (!finite_positive(p.stop_loss_price())) {
        return reject(RejectionCode::invalid_stop_price,
                      "stop_loss_price=" + std::to_string(p.stop_loss_price()));
    }
    if (!finite_positive(p.take_profit_price())) {
        return reject(RejectionCode::invalid_target_price,
                      "take_profit_price=" + std::to_string(p.take_profit_price()));
    }
    // The same rule AltStrategy enforces in its own table.
    if (p.side() == as::SIDE_LONG) {
        if (!(p.take_profit_price() > p.entry_price() &&
              p.stop_loss_price() < p.entry_price())) {
            return reject(RejectionCode::invalid_geometry,
                          "long requires stop < entry < target");
        }
    } else {
        if (!(p.take_profit_price() < p.entry_price() &&
              p.stop_loss_price() > p.entry_price())) {
            return reject(RejectionCode::invalid_geometry,
                          "short requires target < entry < stop");
        }
    }
    // The entry range is optional (0/0 means absent), but a half-populated or
    // inverted one is a defect: a consumer would place an order outside the
    // band the plan intended.
    const bool has_lower = p.entry_lower() > 0.0;
    const bool has_upper = p.entry_upper() > 0.0;
    if (has_lower != has_upper) {
        return reject(RejectionCode::invalid_entry_range,
                      "entry_lower/entry_upper are half-populated");
    }
    if (has_lower && has_upper) {
        if (p.entry_lower() > p.entry_upper()) {
            return reject(RejectionCode::invalid_entry_range, "entry_lower > entry_upper");
        }
        if (p.entry_price() < p.entry_lower() || p.entry_price() > p.entry_upper()) {
            // A warning rather than a rejection: some plan profiles quote a
            // reference price just outside a band they still consider valid,
            // and refusing the trade over it would be the Gateway overruling
            // AltStrategy's plan.
            out.warnings.emplace_back("ENTRY_PRICE_OUTSIDE_RANGE");
        }
    }

    // --- freshness (never fatal here) ---------------------------------------
    // Expiry is a DELIVERY decision made against the journal, not a reason to
    // refuse to record that VPS1 produced the signal.
    if (p.valid_until_ms() <= now_ms) {
        out.warnings.emplace_back("ALREADY_EXPIRED_ON_ARRIVAL");
    }
    if (q.signal_created_at_ms() != 0 && q.signal_created_at_ms() != p.created_at_ms()) {
        out.warnings.emplace_back("T0_MISMATCH_QUALITY_VS_PROPOSAL");
    }
    // No warning for recommended_for_execution == false: VPS1 takes no
    // execution decision and the flag is false on every signal by design.
    return out;
}

} // namespace altsignalgateway::domain
