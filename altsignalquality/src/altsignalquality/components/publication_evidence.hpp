#pragma once

#include "altsignalquality/core/codec.hpp"
#include "altsignalquality/core/hash.hpp"
#include "altsignalquality/data/store.hpp"

namespace altsignalquality {

inline bool redelivery_matches(const data::ExistingEvaluation& row, std::string_view bytes) {
    if (!row.candidate_sha256.empty()) return sha256_hex(bytes) == row.candidate_sha256;
    // Legacy pending rows still carry their original input. A legacy published
    // duplicate without either evidence cannot be verified and is refused.
    return !row.candidate_payload.empty() && row.candidate_payload == bytes;
}

struct PublicationEvidence {
    altstrategy::v1::TradeSignalProposal proposal;
    altsignalquality::v1::SignalQualityEvaluation evaluation;
    std::string symbol;
    std::string trace_id;
};

inline Result<PublicationEvidence> restore_publication(const data::ExistingEvaluation& row) {
    PublicationEvidence out;
    auto proposal = messaging::parse_candidate(row.candidate_payload, &out.symbol, &out.trace_id);
    if (!proposal) return std::unexpected(proposal.error());
    out.proposal = std::move(*proposal);
    auto publication = messaging::parse_envelope(row.publication_payload);
    if (!publication || !publication->has_final_trade_signal())
        return fail(ErrorCode::validation_error, "original publication evidence unavailable");
    const auto& final = publication->final_trade_signal();
    out.evaluation = final.quality();
    if (final.final_signal_id() != row.final_signal_id ||
        final.proposal().SerializeAsString() != out.proposal.SerializeAsString())
        return fail(ErrorCode::validation_error, "stored final proposal inconsistent");
    const auto& evaluation = out.evaluation;
    if (row.decision != "ACCEPTED" || row.final_signal_id.empty() ||
        evaluation.evaluation_id() != row.evaluation_id ||
        evaluation.signal_proposal_id() != out.proposal.signal_proposal_id() ||
        evaluation.decision() != altsignalquality::v1::DECISION_ACCEPTED ||
        evaluation.evaluated_at_ms() != row.evaluated_at_ms ||
        evaluation.basis() == altsignalquality::v1::DECISION_BASIS_UNSPECIFIED ||
        !altsignalquality::v1::DecisionBasis_IsValid(evaluation.basis()) ||
        !redelivery_matches(row, row.candidate_payload))
        return fail(ErrorCode::validation_error, "stored publication evidence inconsistent");
    return out;
}

} // namespace altsignalquality
