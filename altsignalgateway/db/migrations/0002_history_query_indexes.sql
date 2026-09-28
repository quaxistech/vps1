-- 0002 — the protocol-2 history query.
--
-- The query path pages the journal newest-first on `sequence` (gapless,
-- unique, monotone: the one column that makes a keyset cursor exact) with
-- ANDed equality filters and a T0 range. Each filter that a consumer is
-- expected to lead with gets a composite index ending in `sequence DESC`, so
-- a filtered page is one ordered index range and never a scan followed by a
-- sort. The journal is small today (hundreds of rows) and the indexes cost
-- nothing; they are here so the query stays bounded when it is not.
--
-- No column changes, no data changes: everything the history and context
-- queries return already lives in the row (the canonical payload carries the
-- trade, the strategy, the quality verdict and, from schema 2, the macro
-- block). A second history table would have been a second copy of the truth.

CREATE INDEX IF NOT EXISTS idx_asgw_journal_seq_desc
    ON altsignalgateway.signal_journal (sequence DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_pair_seq
    ON altsignalgateway.signal_journal (canonical_pair_id, sequence DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_symbol_seq
    ON altsignalgateway.signal_journal (symbol, sequence DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_strategy_seq
    ON altsignalgateway.signal_journal (strategy_id, sequence DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_state_seq
    ON altsignalgateway.signal_journal (delivery_state, sequence DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_exchange_seq
    ON altsignalgateway.signal_journal (exchange, sequence DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_t0_seq
    ON altsignalgateway.signal_journal (created_at_ms DESC, sequence DESC);
