-- Ensure physical partition bounds are UTC calendar months irrespective of the
-- PostgreSQL session timezone. 0001 remains immutable/checksummed.
--
-- Do NOT reinterpret or rebuild existing partitions: a range mismatch is a
-- data-integrity issue requiring a separate, reviewed migration/backfill plan.
-- An existing nonconforming partition is rejected, not silently accepted.
CREATE OR REPLACE FUNCTION altspotter_ensure_candle_partitions(
    from_ts TIMESTAMPTZ, to_ts TIMESTAMPTZ)
RETURNS INTEGER
LANGUAGE plpgsql
AS $$
DECLARE
    month_start DATE := date_trunc('month', from_ts AT TIME ZONE 'UTC')::date;
    month_last  DATE := date_trunc('month', to_ts AT TIME ZONE 'UTC')::date;
    month_next  DATE;
    tbl         TEXT;
    part        TEXT;
    created     INTEGER := 0;
    lower_utc   TIMESTAMPTZ;
    upper_utc   TIMESTAMPTZ;
    existing    OID;
    matching    BOOLEAN;
BEGIN
    IF from_ts > to_ts OR month_start > month_last THEN
        RAISE EXCEPTION 'invalid partition range: % > %', from_ts, to_ts;
    END IF;

    WHILE month_start <= month_last LOOP
        month_next := (month_start + INTERVAL '1 month')::date;
        lower_utc := month_start::timestamp AT TIME ZONE 'UTC';
        upper_utc := month_next::timestamp AT TIME ZONE 'UTC';
        FOREACH tbl IN ARRAY ARRAY['market_candles_1m', 'market_candles_agg'] LOOP
            part := tbl || '_' || to_char(month_start, 'YYYYMM');
            SELECT c.oid INTO existing
            FROM pg_class c
            JOIN pg_namespace n ON n.oid = c.relnamespace
            WHERE n.nspname = current_schema() AND c.relname = part;

            IF existing IS NOT NULL THEN
                -- Includes a check that the named table is *attached* to the
                -- expected parent, not just present somewhere in the schema.
                SELECT EXISTS (
                    SELECT 1 FROM altspotter_partition_bounds(tbl::regclass) b
                    WHERE b.partition_name = part
                      AND b.lower_bound = lower_utc
                      AND b.upper_bound = upper_utc
                ) INTO matching;
                IF NOT matching THEN
                    RAISE EXCEPTION
                        'partition % exists but is not attached with exact UTC bounds [% , %)',
                        part, lower_utc, upper_utc;
                END IF;
            ELSE
                EXECUTE format(
                    'CREATE TABLE %I PARTITION OF %I FOR VALUES FROM (%L) TO (%L)',
                    part, tbl, lower_utc, upper_utc);
                created := created + 1;
            END IF;
        END LOOP;
        month_start := month_next;
    END LOOP;
    RETURN created;
END;
$$;
