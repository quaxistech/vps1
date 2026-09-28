#include "altstrategy/engine/decision_types.hpp"

#include <cmath>

namespace altstrategy::engine {

std::string to_string(Side s) {
    switch (s) {
        case Side::long_: return "long";
        case Side::short_: return "short";
        default: return "unspecified";
    }
}
Side side_from_string(std::string_view s) {
    if (s == "long") return Side::long_;
    if (s == "short") return Side::short_;
    return Side::unspecified;
}
Side opposite(Side s) {
    if (s == Side::long_) return Side::short_;
    if (s == Side::short_) return Side::long_;
    return Side::unspecified;
}

std::string to_string(Family f) {
    switch (f) {
        case Family::trend: return "TREND";
        case Family::breakout: return "BREAKOUT";
        case Family::mean_reversion: return "MEAN_REVERSION";
        case Family::cross_venue: return "CROSS_VENUE";
        case Family::vwap: return "VWAP";
        case Family::crowding: return "CROWDING";
        case Family::microstructure: return "MICROSTRUCTURE";
        case Family::order_flow: return "ORDER_FLOW";
        case Family::basis: return "BASIS";
        case Family::liquidity: return "LIQUIDITY";
        default: return "UNSPECIFIED";
    }
}
Family family_from_string(std::string_view s) {
    if (s == "TREND") return Family::trend;
    if (s == "BREAKOUT") return Family::breakout;
    if (s == "MEAN_REVERSION") return Family::mean_reversion;
    if (s == "CROSS_VENUE") return Family::cross_venue;
    if (s == "VWAP") return Family::vwap;
    if (s == "CROWDING") return Family::crowding;
    if (s == "MICROSTRUCTURE") return Family::microstructure;
    if (s == "ORDER_FLOW") return Family::order_flow;
    if (s == "BASIS") return Family::basis;
    if (s == "LIQUIDITY") return Family::liquidity;
    return Family::unspecified;
}

std::string to_string(Lifecycle l) {
    switch (l) {
        case Lifecycle::research: return "RESEARCH";
        case Lifecycle::backtested: return "BACKTESTED";
        case Lifecycle::shadow: return "SHADOW";
        case Lifecycle::candidate: return "CANDIDATE";
        case Lifecycle::limited: return "LIMITED";
        case Lifecycle::active: return "ACTIVE";
        case Lifecycle::degraded: return "DEGRADED";
        case Lifecycle::disabled: return "DISABLED";
        case Lifecycle::retired: return "RETIRED";
        default: return "RESEARCH";
    }
}
Lifecycle lifecycle_from_string(std::string_view s) {
    if (s == "RESEARCH") return Lifecycle::research;
    if (s == "BACKTESTED") return Lifecycle::backtested;
    if (s == "SHADOW") return Lifecycle::shadow;
    if (s == "CANDIDATE") return Lifecycle::candidate;
    if (s == "LIMITED") return Lifecycle::limited;
    if (s == "ACTIVE") return Lifecycle::active;
    if (s == "DEGRADED") return Lifecycle::degraded;
    if (s == "DISABLED") return Lifecycle::disabled;
    if (s == "RETIRED") return Lifecycle::retired;
    return Lifecycle::research;
}

std::string to_string(HoldingClass h) {
    switch (h) {
        case HoldingClass::scalp: return "SCALP";
        case HoldingClass::intraday: return "INTRADAY";
        case HoldingClass::extended_intraday: return "EXTENDED_INTRADAY";
        default: return "INTRADAY";
    }
}
HoldingClass holding_from_string(std::string_view s) {
    if (s == "SCALP") return HoldingClass::scalp;
    if (s == "INTRADAY") return HoldingClass::intraday;
    if (s == "EXTENDED_INTRADAY") return HoldingClass::extended_intraday;
    return HoldingClass::intraday;
}

std::string to_string(Grade g) {
    switch (g) {
        case Grade::strong: return "STRONG";
        case Grade::normal: return "NORMAL";
        case Grade::weak: return "WEAK";
        case Grade::experimental: return "EXPERIMENTAL";
        case Grade::shadow: return "SHADOW";
        case Grade::invalid: return "INVALID";
        default: return "NORMAL";
    }
}
Grade grade_from_string(std::string_view s) {
    if (s == "STRONG") return Grade::strong;
    if (s == "NORMAL") return Grade::normal;
    if (s == "WEAK") return Grade::weak;
    if (s == "EXPERIMENTAL") return Grade::experimental;
    if (s == "SHADOW") return Grade::shadow;
    if (s == "INVALID") return Grade::invalid;
    return Grade::normal;
}

std::string to_string(EntryType e) {
    switch (e) {
        case EntryType::market: return "market";
        case EntryType::limit: return "limit";
        case EntryType::stop_market: return "stop_market";
        case EntryType::stop_limit: return "stop_limit";
        default: return "limit";
    }
}
EntryType entry_from_string(std::string_view s) {
    if (s == "market") return EntryType::market;
    if (s == "limit") return EntryType::limit;
    if (s == "stop_market") return EntryType::stop_market;
    if (s == "stop_limit") return EntryType::stop_limit;
    return EntryType::limit;
}

std::string to_string(SelectionMode m) {
    switch (m) {
        case SelectionMode::exploit: return "EXPLOIT";
        case SelectionMode::explore_shadow: return "EXPLORE_SHADOW";
        default: return "EXPLOIT";
    }
}

std::string to_string(PlanProfile p) {
    switch (p) {
        case PlanProfile::aggressive: return "AGGRESSIVE";
        case PlanProfile::balanced: return "BALANCED";
        case PlanProfile::conservative: return "CONSERVATIVE";
        default: return "BALANCED";
    }
}

std::string to_string(ProposalState s) {
    switch (s) {
        case ProposalState::draft: return "DRAFT";
        case ProposalState::context_ready: return "CONTEXT_READY";
        case ProposalState::candidates_generated: return "CANDIDATES_GENERATED";
        case ProposalState::ranked: return "RANKED";
        case ProposalState::plans_generated: return "PLANS_GENERATED";
        case ProposalState::ml_evaluated: return "ML_EVALUATED";
        case ProposalState::proposed: return "PROPOSED";
        case ProposalState::superseded: return "SUPERSEDED";
        case ProposalState::expired: return "EXPIRED";
        case ProposalState::cancelled: return "CANCELLED";
        case ProposalState::entry_expired: return "ENTRY_EXPIRED";
        case ProposalState::outcome_pending: return "OUTCOME_PENDING";
        case ProposalState::outcome_final: return "OUTCOME_FINAL";
        default: return "DRAFT";
    }
}

std::string to_string(RankingResult r) {
    switch (r) {
        case RankingResult::recommended: return "RECOMMENDED";
        case RankingResult::multiple_close: return "MULTIPLE_CLOSE";
        case RankingResult::conflicted: return "CONFLICTED";
        case RankingResult::no_clear_winner: return "NO_CLEAR_WINNER";
        case RankingResult::no_candidates: return "NO_CANDIDATES";
        default: return "NO_CANDIDATES";
    }
}

std::string to_string(MarketOutcomeType o) {
    switch (o) {
        case MarketOutcomeType::entry_not_reached: return "ENTRY_NOT_REACHED";
        case MarketOutcomeType::entry_expired: return "ENTRY_EXPIRED";
        case MarketOutcomeType::target_first: return "TARGET_FIRST";
        case MarketOutcomeType::stop_first: return "STOP_FIRST";
        case MarketOutcomeType::time_stop: return "TIME_STOP";
        case MarketOutcomeType::cancelled: return "CANCELLED";
        case MarketOutcomeType::ambiguous: return "AMBIGUOUS";
        case MarketOutcomeType::data_incomplete: return "DATA_INCOMPLETE";
        default: return "DATA_INCOMPLETE";
    }
}
MarketOutcomeType market_outcome_from_string(std::string_view s) {
    if (s == "ENTRY_NOT_REACHED") return MarketOutcomeType::entry_not_reached;
    if (s == "ENTRY_EXPIRED") return MarketOutcomeType::entry_expired;
    if (s == "TARGET_FIRST") return MarketOutcomeType::target_first;
    if (s == "STOP_FIRST") return MarketOutcomeType::stop_first;
    if (s == "TIME_STOP") return MarketOutcomeType::time_stop;
    if (s == "CANCELLED") return MarketOutcomeType::cancelled;
    if (s == "AMBIGUOUS") return MarketOutcomeType::ambiguous;
    return MarketOutcomeType::data_incomplete;
}

std::string to_string(OutcomePath p) {
    switch (p) {
        case OutcomePath::raw_trades: return "PATH_RAW_TRADES";
        case OutcomePath::s1_trade_agg: return "PATH_1S_TRADE_AGG";
        case OutcomePath::s1_price_book_agg: return "PATH_1S_PRICE_BOOK_AGG";
        case OutcomePath::m1_candles: return "PATH_1M_CANDLES";
        default: return "PATH_1M_CANDLES";
    }
}

std::string to_string(PredictionState s) {
    switch (s) {
        case PredictionState::missing: return "MISSING";
        case PredictionState::present: return "PRESENT";
        case PredictionState::incomplete: return "INCOMPLETE";
        case PredictionState::stale: return "STALE";
        case PredictionState::abstain: return "ABSTAIN";
        case PredictionState::incompatible: return "INCOMPATIBLE";
    }
    return "MISSING";
}

std::string to_string(EconState s) {
    switch (s) {
        case EconState::no_data: return "NO_DATA";
        case EconState::model_unavailable: return "MODEL_UNAVAILABLE";
        case EconState::model_stale: return "MODEL_STALE";
        case EconState::model_abstain: return "MODEL_ABSTAIN";
        case EconState::model_shadow: return "MODEL_SHADOW";
        case EconState::horizon_conflict: return "HORIZON_CONFLICT";
        case EconState::no_edge: return "NO_EDGE";
        case EconState::small_edge: return "SMALL_EDGE";
        case EconState::positive_edge: return "POSITIVE_EDGE";
        case EconState::strong_edge: return "STRONG_EDGE";
    }
    return "MODEL_UNAVAILABLE";
}

std::string to_string(DriftStatus d) {
    switch (d) {
        case DriftStatus::stable: return "STABLE";
        case DriftStatus::watch: return "WATCH";
        case DriftStatus::degraded: return "DEGRADED";
        case DriftStatus::structural_break: return "STRUCTURAL_BREAK";
        default: return "STABLE";
    }
}

std::string to_string(ContextQuality q) {
    switch (q) {
        case ContextQuality::strict_4_of_4: return "STRICT_4_OF_4";
        case ContextQuality::degraded_3_of_4: return "DEGRADED_3_OF_4";
        case ContextQuality::degraded_2_of_4: return "DEGRADED_2_OF_4";
        case ContextQuality::invalid: return "INVALID";
        default: return "STRICT_4_OF_4";
    }
}
ContextQuality quality_from_string(std::string_view s) {
    if (s == "STRICT_4_OF_4") return ContextQuality::strict_4_of_4;
    if (s == "DEGRADED_3_OF_4") return ContextQuality::degraded_3_of_4;
    if (s == "DEGRADED_2_OF_4") return ContextQuality::degraded_2_of_4;
    if (s == "INVALID") return ContextQuality::invalid;
    return ContextQuality::strict_4_of_4;
}

const ProjectedZone* DecisionContext::nearest_zone(const std::string& kind, double price) const {
    const ProjectedZone* best = nullptr;
    if (!(price > 0)) return nullptr;
    for (const auto& z : bitget_projected_zones) {
        if (!(z.center > 0)) continue;
        // The side is decided here, by position relative to the current price —
        // a projected zone has none of its own. Requiring the wire label to
        // equal "support"/"resistance" matched nothing, because every zone
        // arrives tagged "dynamic"; the result was that no trade plan ever used
        // a structure target and every one silently fell back to the
        // reward/risk multiple.
        if (kind == "support" && z.center > price) continue;
        if (kind == "resistance" && z.center < price) continue;
        if (best == nullptr || std::abs(z.center - price) < std::abs(best->center - price)) {
            best = &z;
        }
    }
    return best;
}

double StrategyCandidate::raw_rr() const {
    if (entry_hint <= 0 || invalidation_hint <= 0 || target_hint <= 0) return 0;
    const bool is_long = side == Side::long_;
    if (is_long && !(invalidation_hint < entry_hint && entry_hint < target_hint)) return 0;
    if (!is_long && !(invalidation_hint > entry_hint && entry_hint > target_hint)) return 0;
    const double risk = std::abs(entry_hint - invalidation_hint);
    if (risk <= 0) return 0;
    return std::abs(target_hint - entry_hint) / risk;
}

} // namespace altstrategy::engine
