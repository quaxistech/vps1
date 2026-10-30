-- Install AltMacro0006 first. Reuse the existing owner read-only mapping;
-- no upstream writes or credential changes.
DO $$
DECLARE server_name TEXT;
BEGIN
    SELECT s.srvname INTO server_name FROM pg_foreign_table f
        JOIN pg_class c ON c.oid=f.ftrelid JOIN pg_namespace n ON n.oid=c.relnamespace
        JOIN pg_foreign_server s ON s.oid=f.ftserver
        WHERE n.nspname='altmacro_ref' AND c.relname='macro_reference_values';
    IF server_name IS NULL THEN RAISE EXCEPTION 'AltMacro owner read-only foreign mapping is required'; END IF;
    IF to_regclass('altmacro_ref.macro_reference_revisions') IS NULL THEN
        EXECUTE format('IMPORT FOREIGN SCHEMA public LIMIT TO (macro_reference_revisions) FROM SERVER %I INTO altmacro_ref',server_name);
    END IF;
    IF to_regclass('altmacro_ref.macro_events') IS NULL THEN
        EXECUTE format('IMPORT FOREIGN SCHEMA public LIMIT TO (macro_events) FROM SERVER %I INTO altmacro_ref',server_name);
    END IF;
END $$;
