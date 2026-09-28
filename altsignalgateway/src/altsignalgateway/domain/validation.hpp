#pragma once

#include "vendor/decision.pb.h"
#include "vendor/signalquality.pb.h"

#include <string>
#include <string_view>
#include <vector>

namespace altsignalgateway::domain {

// Contract validation of one inbound FINAL TradeSignal.
//
// The Gateway is the point at which VPS1 becomes externally visible, so this is
// the last place a malformed trade can be stopped. It validates SHAPE and
// INTERNAL CONSISTENCY only — never quality, never edge, never whether the
// trade is a good idea. A signal that passes here is transmitted exactly as it
// arrived; a signal that fails is recorded as rejected and never journalled,
// because the journal is a promise of delivery and an unexecutable message is
// not something to promise.
//
// The geometry rules deliberately mirror AltStrategy's own CHECK constraints
// (chk_long_geometry / chk_short_geometry / chk_holding_limit). Anything that
// could not have been written to AltStrategy's own table cannot legitimately
// have been published by it either.

enum class RejectionCode {
    ok = 0,
    not_a_final_signal,        // envelope carries a different payload
    // The producer's envelope declares a schema this build does not decode.
    // Reported separately from "not a final signal": one is a version skew an
    // operator fixes by upgrading, the other is a routing mistake, and they
    // send an investigator to opposite ends of the stack.
    unsupported_schema_version,
    malformed_envelope,        // not decodable as protobuf at all
    wrong_source_service,
    missing_final_signal_id,
    missing_proposal,
    missing_quality,
    decision_not_accepted,     // a FINAL signal that was not ACCEPTED upstream
    missing_signal_proposal_id,
    missing_candidate_id,
    missing_strategy_id,
    invalid_canonical_pair,
    invalid_side,
    invalid_entry_type,
    invalid_grade,
    missing_created_at,
    invalid_validity_window,
    non_finite_price,
    invalid_stop_price,
    invalid_target_price,
    invalid_entry_price,
    invalid_geometry,
    invalid_entry_range,
    invalid_holding_time,
    invalid_quality_score,
    // The canonical payload would not fit the message bound this endpoint
    // advertises to its own clients.
    payload_too_large,
};

[[nodiscard]] std::string_view rejection_code_name(RejectionCode c);

struct ValidationResult {
    RejectionCode code = RejectionCode::ok;
    std::string detail;
    // Non-fatal observations recorded alongside an accepted signal. They never
    // block delivery; they exist so a contract drift is visible in the journal
    // before it becomes a support case.
    std::vector<std::string> warnings;

    [[nodiscard]] bool ok() const { return code == RejectionCode::ok; }
};

// Validate the envelope-level framing.
[[nodiscard]] ValidationResult validate_envelope(const altsignalquality::v1::Envelope& env);

// Validate the FINAL signal itself. `now_ms` is passed in so the freshness
// warnings are testable without a clock.
[[nodiscard]] ValidationResult validate_final_signal(
    const altsignalquality::v1::FinalTradeSignal& sig, int64_t now_ms);

} // namespace altsignalgateway::domain
