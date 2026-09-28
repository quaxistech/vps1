// The resume decision has been wrong twice, in two different ways, and each
// time the symptom was the same: a series that looked complete and was not.
// These cases are the record of both.

#include "altmacro/components/bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>

using altmacro::components::Bootstrap;
using altmacro::data::Cursor;

namespace {
constexpr int64_t kDec01 = 1764547200000LL;   // 2025-12-01T00:00:00Z
constexpr int64_t kSep07 = 1788739200000LL;   // 2026-09-07T00:00:00Z
constexpr int64_t kSep10 = 1788998400000LL;   // 2026-09-10T00:00:00Z
} // namespace

TEST_CASE("with no cursor the walk starts where it was asked to", "[resume]") {
    const auto d = Bootstrap::resolve_resume_start(kDec01, std::nullopt);
    CHECK(d.start_ms == kDec01);
    CHECK_FALSE(d.resumed);
    CHECK_FALSE(d.restarted);
}

TEST_CASE("an interrupted walk resumes from its watermark", "[resume]") {
    Cursor c;
    c.watermark_ms = kSep07;
    c.backfilled_from_ms = kDec01;   // it really did start back there
    const auto d = Bootstrap::resolve_resume_start(kDec01, c);
    CHECK(d.start_ms == kSep07);
    CHECK(d.resumed);
}

TEST_CASE("a window widened backwards is refilled, not skipped", "[resume]") {
    // The defect: a narrow earlier run left a watermark at the end of its own
    // window. The next run asked for nine more months of history and, trusting
    // that watermark, walked none of it -- and the series looked complete.
    Cursor c;
    c.watermark_ms = kSep10;
    c.backfilled_from_ms = kSep07;   // never went back further than this
    const auto d = Bootstrap::resolve_resume_start(kDec01, c);
    CHECK(d.start_ms == kDec01);
    CHECK(d.restarted);
    CHECK_FALSE(d.resumed);
}

TEST_CASE("a cursor that never recorded where it began is not trusted", "[resume]") {
    // The second form of the same defect: `backfilled_from` was being written
    // as the configured window start rather than the point the walk actually
    // began, so the cursor vouched for ground it had never covered. A cursor
    // with nothing recorded gets no benefit of the doubt.
    Cursor c;
    c.watermark_ms = kSep10;
    c.backfilled_from_ms = std::nullopt;
    const auto d = Bootstrap::resolve_resume_start(kDec01, c);
    CHECK(d.start_ms == kDec01);
    CHECK(d.restarted);
}

TEST_CASE("a watermark behind the requested start is ignored", "[resume]") {
    Cursor c;
    c.watermark_ms = kDec01;
    c.backfilled_from_ms = kDec01;
    const auto d = Bootstrap::resolve_resume_start(kSep07, c);
    CHECK(d.start_ms == kSep07);
    CHECK_FALSE(d.resumed);
}

TEST_CASE("a completed window resumes past its end and does no work", "[resume]") {
    Cursor c;
    c.watermark_ms = kSep10;
    c.backfilled_from_ms = kDec01;
    const auto d = Bootstrap::resolve_resume_start(kDec01, c);
    CHECK(d.start_ms == kSep10);
    CHECK(d.resumed);
}
