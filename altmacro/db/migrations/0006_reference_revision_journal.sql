BEGIN;

-- Prospective immutable revisions. Existing mutable latest rows are captured
-- at installation time, never relabelled as a historical point-in-time tape.
CREATE TABLE macro_reference_revisions (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    instrument_id BIGINT NOT NULL REFERENCES macro_instruments(id),
    provider_id SMALLINT NOT NULL REFERENCES macro_providers(id),
    observation_time TIMESTAMPTZ NOT NULL,
    publication_time TIMESTAMPTZ,
    source_ingest_time TIMESTAMPTZ NOT NULL,
    recorded_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    available_at TIMESTAMPTZ NOT NULL,
    value NUMERIC,
    value_as_of TIMESTAMPTZ,
    quality_status TEXT NOT NULL,
    quality_reason TEXT,
    source_revision BIGINT NOT NULL,
    timestamp_granularity TEXT NOT NULL CHECK(timestamp_granularity IN ('SOURCE_INSTANT','RECEIPT_BOUND'))
);
CREATE INDEX macro_reference_revisions_asof
    ON macro_reference_revisions(instrument_id,available_at,observation_time,id);

CREATE FUNCTION macro_capture_reference_revision() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE captured_at TIMESTAMPTZ := clock_timestamp();
BEGIN
    IF TG_OP='UPDATE' AND ROW(NEW.value,NEW.publication_time,NEW.value_as_of,NEW.quality_status,NEW.quality_reason)
       IS NOT DISTINCT FROM ROW(OLD.value,OLD.publication_time,OLD.value_as_of,OLD.quality_status,OLD.quality_reason) THEN
        RETURN NEW;
    END IF;
    INSERT INTO macro_reference_revisions(instrument_id,provider_id,observation_time,publication_time,
        source_ingest_time,recorded_at,available_at,value,value_as_of,quality_status,quality_reason,
        source_revision,timestamp_granularity)
    VALUES(NEW.instrument_id,NEW.provider_id,NEW.event_time,NEW.publication_time,NEW.ingest_time,
        captured_at,GREATEST(captured_at,NEW.publication_time,NEW.ingest_time),NEW.value,NEW.value_as_of,
        NEW.quality_status,NEW.quality_reason,NEW.revision,
        CASE WHEN NEW.publication_time IS NULL THEN 'RECEIPT_BOUND' ELSE 'SOURCE_INSTANT' END);
    RETURN NEW;
END $$;
CREATE TRIGGER macro_reference_revision_capture AFTER INSERT OR UPDATE ON macro_reference_values
    FOR EACH ROW EXECUTE FUNCTION macro_capture_reference_revision();

INSERT INTO macro_reference_revisions(instrument_id,provider_id,observation_time,publication_time,
    source_ingest_time,available_at,value,value_as_of,quality_status,quality_reason,source_revision,timestamp_granularity)
SELECT instrument_id,provider_id,event_time,publication_time,ingest_time,
    GREATEST(clock_timestamp(),publication_time,ingest_time),value,value_as_of,quality_status,quality_reason,
    revision,CASE WHEN publication_time IS NULL THEN 'RECEIPT_BOUND' ELSE 'SOURCE_INSTANT' END
FROM macro_reference_values;

CREATE FUNCTION macro_forbid_reference_revision_mutation() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'macro reference revisions are immutable'; END $$;
CREATE TRIGGER macro_reference_revisions_immutable BEFORE UPDATE OR DELETE ON macro_reference_revisions
    FOR EACH ROW EXECUTE FUNCTION macro_forbid_reference_revision_mutation();

DO $$ BEGIN
    IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='altmacro_reader') THEN
        GRANT SELECT ON macro_reference_revisions TO altmacro_reader;
    END IF;
END $$;
COMMIT;
