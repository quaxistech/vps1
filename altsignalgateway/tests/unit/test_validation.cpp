// FINAL TradeSignal contract validation — the last place a malformed trade can
// be stopped before it leaves VPS1.

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/domain/validation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace altsignalgateway::domain;
namespace as = altstrategy::v1;
namespace sq = altsignalquality::v1;

namespace {

constexpr int64_t kT0 = 1'787'676'374'679;
constexpr int64_t kValidUntil = kT0 + 1'350'000;   // the deployed 22.5 minute window

// A signal shaped exactly like the ones AltStrategy actually produces on this
// deployment: a long market entry with the geometry its own CHECK constraints
// enforce.
sq::FinalTradeSignal good_signal() {
    sq::FinalTradeSignal sig;
    sig.set_final_signal_id("sqfin_0123456789abcdef0123456789abcdef");
    sig.set_published_at_ms(kT0 + 500);
    sig.set_producer_version("1.0.0");

    auto* p = sig.mutable_proposal();
    p->set_signal_proposal_id("sig_9a1e4d57883b376b582955c0");
    p->set_candidate_id("cand_1");
    p->set_canonical_pair_id(4);
    p->set_strategy_id("volume_weighted_momentum_burst");
    p->set_strategy_version("1.0.0");
    p->set_side(as::SIDE_LONG);
    p->set_proposal_grade(as::GRADE_NORMAL);
    p->set_entry_type(as::ENTRY_MARKET);
    p->set_entry_price(1.4733);
    p->set_stop_loss_price(1.4564);
    p->set_take_profit_price(1.4878);
    p->set_created_at_ms(kT0);
    p->set_valid_until_ms(kValidUntil);
    p->set_max_holding_time_ms(28'800'000);
    p->set_recommended_for_execution(true);

    auto* q = sig.mutable_quality();
    q->set_evaluation_id("sqev_abc");
    q->set_decision(sq::DECISION_ACCEPTED);
    q->set_basis(sq::BASIS_MODEL);
    q->set_has_quality_score(true);
    q->set_quality_score(0.61);
    q->set_signal_created_at_ms(kT0);
    return sig;
}

bool has_warning(const ValidationResult& r, const std::string& code) {
    return std::find(r.warnings.begin(), r.warnings.end(), code) != r.warnings.end();
}

} // namespace

TEST_CASE("a real-shaped signal validates") {
    const auto r = validate_final_signal(good_signal(), kT0 + 1000);
    CHECK(r.ok());
    CHECK(r.warnings.empty());
}

TEST_CASE("only an ACCEPTED verdict may reach the boundary") {
    auto sig = good_signal();
    sig.mutable_quality()->set_decision(sq::DECISION_REJECTED);
    // Forwarding this would put a trade AltSignalQuality declined on the wire.
    CHECK(validate_final_signal(sig, kT0).code == RejectionCode::decision_not_accepted);

    sig.mutable_quality()->set_decision(sq::QUALITY_DECISION_UNSPECIFIED);
    CHECK(validate_final_signal(sig, kT0).code == RejectionCode::decision_not_accepted);
}

TEST_CASE("plan geometry is enforced exactly as AltStrategy enforces it") {
    SECTION("a long with the stop above entry") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_stop_loss_price(1.50);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_geometry);
    }
    SECTION("a long with the target below entry") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_take_profit_price(1.40);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_geometry);
    }
    SECTION("a short with long geometry") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_side(as::SIDE_SHORT);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_geometry);
    }
    SECTION("a correct short") {
        auto sig = good_signal();
        auto* p = sig.mutable_proposal();
        p->set_side(as::SIDE_SHORT);
        p->set_entry_price(1.4733);
        p->set_stop_loss_price(1.4878);
        p->set_take_profit_price(1.4564);
        CHECK(validate_final_signal(sig, kT0).ok());
    }
}

TEST_CASE("a non-finite or non-positive price is refused") {
    SECTION("NaN entry") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_entry_price(std::nan(""));
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::non_finite_price);
    }
    SECTION("infinite stop") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_stop_loss_price(
            std::numeric_limits<double>::infinity());
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::non_finite_price);
    }
    SECTION("zero target") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_take_profit_price(0.0);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_target_price);
    }
    SECTION("negative entry") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_entry_price(-1.0);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_entry_price);
    }
}

TEST_CASE("the validity window must be a window") {
    SECTION("valid_until before creation") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_valid_until_ms(kT0 - 1);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_validity_window);
    }
    SECTION("valid_until equal to creation") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_valid_until_ms(kT0);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_validity_window);
    }
    SECTION("no creation time") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_created_at_ms(0);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::missing_created_at);
    }
}

TEST_CASE("the holding limit is AltStrategy's own 48 hours") {
    auto sig = good_signal();
    sig.mutable_proposal()->set_max_holding_time_ms(172'800'001);
    CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_holding_time);
    sig.mutable_proposal()->set_max_holding_time_ms(172'800'000);
    CHECK(validate_final_signal(sig, kT0).ok());
    sig.mutable_proposal()->set_max_holding_time_ms(0);
    CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_holding_time);
}

TEST_CASE("identity fields are mandatory") {
    SECTION("no final signal id") {
        auto sig = good_signal();
        sig.clear_final_signal_id();
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::missing_final_signal_id);
    }
    SECTION("no proposal") {
        auto sig = good_signal();
        sig.clear_proposal();
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::missing_proposal);
    }
    SECTION("no quality evaluation") {
        auto sig = good_signal();
        sig.clear_quality();
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::missing_quality);
    }
    SECTION("no signal proposal id") {
        auto sig = good_signal();
        sig.mutable_proposal()->clear_signal_proposal_id();
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::missing_signal_proposal_id);
    }
    SECTION("no canonical pair") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_canonical_pair_id(0);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_canonical_pair);
    }
    SECTION("unspecified side") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_side(as::SIDE_UNSPECIFIED);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_side);
    }
    SECTION("unspecified entry type") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_entry_type(as::ENTRY_TYPE_UNSPECIFIED);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_entry_type);
    }
    SECTION("the one grade AltStrategy forbids publishing") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_proposal_grade(as::GRADE_INVALID);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_grade);
    }
}

TEST_CASE("a quality score outside [0,1] is refused") {
    auto sig = good_signal();
    sig.mutable_quality()->set_quality_score(1.5);
    CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_quality_score);
    sig.mutable_quality()->set_quality_score(std::nan(""));
    CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_quality_score);
    // With has_quality_score false the field is meaningless and is not checked:
    // a cold-start decision legitimately carries garbage there.
    sig.mutable_quality()->set_has_quality_score(false);
    CHECK(validate_final_signal(sig, kT0).ok());
}

TEST_CASE("a half-populated entry range is a defect, an absent one is not") {
    SECTION("both absent is fine") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_entry_lower(0);
        sig.mutable_proposal()->set_entry_upper(0);
        CHECK(validate_final_signal(sig, kT0).ok());
    }
    SECTION("only the lower bound") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_entry_lower(1.47);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_entry_range);
    }
    SECTION("inverted bounds") {
        auto sig = good_signal();
        sig.mutable_proposal()->set_entry_lower(1.48);
        sig.mutable_proposal()->set_entry_upper(1.47);
        CHECK(validate_final_signal(sig, kT0).code == RejectionCode::invalid_entry_range);
    }
    SECTION("a reference price outside the band warns but does not refuse") {
        // The Gateway does not overrule AltStrategy's plan; it reports.
        auto sig = good_signal();
        sig.mutable_proposal()->set_entry_lower(1.40);
        sig.mutable_proposal()->set_entry_upper(1.45);
        const auto r = validate_final_signal(sig, kT0);
        CHECK(r.ok());
        CHECK(has_warning(r, "ENTRY_PRICE_OUTSIDE_RANGE"));
    }
}

TEST_CASE("expiry on arrival is recorded, never a reason to drop the signal") {
    // The audit trail must show that VPS1 produced it, whatever its fate.
    auto sig = good_signal();
    const auto r = validate_final_signal(sig, kValidUntil + 1);
    CHECK(r.ok());
    CHECK(has_warning(r, "ALREADY_EXPIRED_ON_ARRIVAL"));
}

TEST_CASE("contract drift between the two upstream blocks is flagged") {
    auto sig = good_signal();
    sig.mutable_quality()->set_signal_created_at_ms(kT0 + 999);
    const auto r = validate_final_signal(sig, kT0);
    CHECK(r.ok());
    CHECK(has_warning(r, "T0_MISMATCH_QUALITY_VS_PROPOSAL"));
}

TEST_CASE("a signal with no execution recommendation is an ordinary VPS1 signal",
          "[validation][vps1][regression]") {
    // VPS1 takes no execution decision: recommended_for_execution is false on
    // every signal by design, so it is neither refused nor flagged (a warning
    // on every signal says nothing).
    auto sig = good_signal();
    sig.mutable_proposal()->set_recommended_for_execution(false);
    const auto r = validate_final_signal(sig, kT0);
    CHECK(r.ok());
    CHECK_FALSE(has_warning(r, "UPSTREAM_NOT_RECOMMENDED"));
}

TEST_CASE("envelope framing is checked before the payload") {
    sq::Envelope env;
    env.set_schema_version(1);
    env.set_source_service("altsignalquality");
    SECTION("a non-final payload is not an error") {
        *env.mutable_quality_evaluation() = sq::SignalQualityEvaluation();
        CHECK(validate_envelope(env).code == RejectionCode::not_a_final_signal);
    }
    SECTION("a foreign producer is refused") {
        *env.mutable_final_trade_signal() = good_signal();
        env.set_source_service("someone-else");
        CHECK(validate_envelope(env).code == RejectionCode::wrong_source_service);
    }
    SECTION("the real thing passes") {
        *env.mutable_final_trade_signal() = good_signal();
        CHECK(validate_envelope(env).ok());
    }
}

TEST_CASE("every rejection code has a name") {
    // A code that renders as UNKNOWN in the journal is a code nobody can act on.
    for (int i = 0; i <= static_cast<int>(RejectionCode::payload_too_large); ++i) {
        CHECK(rejection_code_name(static_cast<RejectionCode>(i)) != "UNKNOWN");
    }
}

TEST_CASE("a version skew and a routing mistake are different codes") {
    // Reported live as NOT_A_FINAL_SIGNAL before this was split out, which sent
    // an investigator looking for a subject-routing bug when the answer was a
    // producer running a schema this build does not decode.
    CHECK(rejection_code_name(RejectionCode::unsupported_schema_version) ==
          "UNSUPPORTED_SCHEMA_VERSION");
    CHECK(rejection_code_name(RejectionCode::malformed_envelope) == "MALFORMED_ENVELOPE");
    CHECK(rejection_code_name(RejectionCode::not_a_final_signal) == "NOT_A_FINAL_SIGNAL");
    CHECK(rejection_code_name(RejectionCode::payload_too_large) == "PAYLOAD_TOO_LARGE");
}

TEST_CASE("the upstream codec distinguishes undecodable from unsupported") {
    // Undecodable bytes.
    auto garbage = altsignalgateway::messaging::parse_upstream_envelope(std::string(64, '\xAB'));
    REQUIRE_FALSE(garbage.has_value());
    CHECK(garbage.error().code == altsignalgateway::ErrorCode::parse_error);

    // Decodable, but declaring a schema this build does not speak.
    sq::Envelope future;
    future.set_schema_version(99);
    future.set_source_service("altsignalquality");
    std::string bytes;
    future.SerializeToString(&bytes);
    auto skew = altsignalgateway::messaging::parse_upstream_envelope(bytes);
    REQUIRE_FALSE(skew.has_value());
    CHECK(skew.error().code == altsignalgateway::ErrorCode::validation_error);
    CHECK(skew.error().message.find("99") != std::string::npos);

    // An empty message decodes to schema_version 0, which is also a skew rather
    // than a parse failure — and must not be mistaken for a valid signal.
    auto empty = altsignalgateway::messaging::parse_upstream_envelope(std::string());
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == altsignalgateway::ErrorCode::validation_error);
}

TEST_CASE("an unbounded upstream free-text field is what makes the size bound necessary") {
    // The upstream contract puts no length limit on any string field, so a
    // structurally perfect signal can be arbitrarily large. Validation accepts
    // it — it IS valid — and the size bound is applied afterwards, against the
    // message limit the endpoint advertises to its clients.
    auto sig = good_signal();
    sig.mutable_proposal()->set_stop_basis(std::string(900 * 1024, 'x'));
    CHECK(validate_final_signal(sig, kT0).ok());
    std::string bytes;
    sig.SerializeToString(&bytes);
    CHECK(bytes.size() > 900 * 1024);
}
