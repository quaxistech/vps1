#pragma once

#include "core/error.hpp"
#include "core/types.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <vector>

namespace altspotter::persistence {

struct CandleRow {
    int16_t exchange_id = 0;
    int64_t instrument_id = 0;
    Candle candle;
    int64_t exchange_received_at_ms = 0; // local receive timestamp (0 = unknown/REST)
};

struct WriteStats {
    int64_t inserted = 0;
    int64_t updated = 0;   // legitimate revision (values changed)
    int64_t identical = 0; // duplicate delivery, no-op
};

// Batch UPSERT into market_candles_1m. Natural key (exchange_id, instrument_id, open_time).
// Identical rows are no-ops; changed rows bump `revision` and refresh audit columns.
// Optional unknowns retain known values. Source/quality/close-time corrections
// are semantic revisions; a changed local receipt timestamp alone is not.
// Caller controls transaction scope and chunking (<= ~2000 rows advisable per call).
Result<WriteStats> upsert_candles_1m(pqxx::transaction_base& txn, const std::vector<CandleRow>& rows);

// Same contract for derived bars in market_candles_agg (key + timeframe).
Result<WriteStats> upsert_candles_agg(pqxx::transaction_base& txn, const std::vector<CandleRow>& rows);

// In-batch deduplication by natural key; the LAST occurrence wins (later
// deliveries carry newer values). with_timeframe adds tf to the key (agg table).
void dedupe_candle_rows(std::vector<CandleRow>& rows, bool with_timeframe);

} // namespace altspotter::persistence
