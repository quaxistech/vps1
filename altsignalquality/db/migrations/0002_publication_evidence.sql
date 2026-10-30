-- Additive, prospective evidence. No historical decision is rewritten.
-- NULL identifies legacy rows; never guess missing publication evidence.
ALTER TABLE altsignalquality.signal_evaluations
    ADD COLUMN IF NOT EXISTS publication_payload BYTEA,
    ADD COLUMN IF NOT EXISTS candidate_sha256 TEXT;

COMMENT ON COLUMN altsignalquality.signal_evaluations.publication_payload IS
    'Original final-signal envelope bytes, frozen in the first claim transaction';
COMMENT ON COLUMN altsignalquality.signal_evaluations.candidate_sha256 IS
    'SHA256 of original input envelope; retained after pending payload is cleared';
