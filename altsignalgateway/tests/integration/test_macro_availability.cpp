#include "altsignalgateway/data/store.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

using namespace altsignalgateway;

TEST_CASE("macro enrichment selects only computations available by the original cutoff") {
    const char* dsn=std::getenv("ASGW_TEST_PG_DSN");
    if(!dsn) SKIP("dedicated local test database required");
    pqxx::connection db(dsn);
    REQUIRE(std::string(db.dbname()).starts_with("asgw_audit_test_"));
    REQUIRE(std::string(db.hostname()).starts_with("/"));
    {
        pqxx::work tx(db);
        tx.exec("DROP SCHEMA IF EXISTS alttech_ref CASCADE; CREATE SCHEMA alttech_ref;"
                "CREATE TABLE alttech_ref.feature_set_versions(id bigint,name text,version int,schema_hash text,scope text);"
                "CREATE TABLE alttech_ref.feature_set_members(feature_set_version_id bigint,position int,output_name text);"
                "CREATE TABLE alttech_ref.macro_feature_vectors(feature_set_version_id bigint,timeframe text,"
                "bar_open_time timestamptz,computed_at timestamptz,method_version text,status text,valid_count int,"
                "values real[],validity_bitmap bytea);"
                "INSERT INTO alttech_ref.feature_set_versions VALUES(1,'macro-core',1,'unchanged-schema','GLOBAL');"
                "INSERT INTO alttech_ref.feature_set_members VALUES(1,0,'fixture-value');"
                "INSERT INTO alttech_ref.macro_feature_vectors VALUES"
                "(1,'1m',to_timestamp(100),to_timestamp(100.01),'old','VALID',1,'{1}','\\x01'),"
                "(1,'1m',to_timestamp(160),to_timestamp(160.1),'new','VALID',1,'{2}','\\x01')");
        tx.commit();
    }
    config::RuntimeConfig config; config.common.postgres_dsn=dsn;
    config.journal.postgres_pool_size=2;
    data::JournalStore store(config,"macro-cutoff-fixture");
    const auto earlier=store.load_macro_asof(160'050,120'000);
    REQUIRE(earlier); REQUIRE(earlier->present);
    CHECK(earlier->method_version=="old");
    CHECK(earlier->values.at(0).value==1);
    const auto boundary=store.load_macro_asof(160'100,120'000);
    REQUIRE(boundary); REQUIRE(boundary->present);
    CHECK(boundary->method_version=="new"); CHECK(boundary->values.at(0).value==2);
    {
        pqxx::work tx(db);
        tx.exec("UPDATE alttech_ref.macro_feature_vectors SET computed_at=to_timestamp(300),"
                "method_version='replacement',values='{9}' WHERE bar_open_time=to_timestamp(160)");
        tx.commit();
    }
    // Re-opening the reader after a method replacement must not backdate it.
    data::JournalStore restarted(config,"macro-cutoff-restarted");
    const auto replaced=restarted.load_macro_asof(200'000,120'000);
    REQUIRE(replaced); REQUIRE(replaced->present);
    CHECK(replaced->method_version=="old"); CHECK(replaced->values.at(0).value==1);
    {
        pqxx::work tx(db);
        tx.exec("DELETE FROM alttech_ref.macro_feature_vectors WHERE method_version='old'");
        tx.commit();
    }
    const auto unavailable=restarted.load_macro_asof(200'000,120'000);
    REQUIRE(unavailable); CHECK_FALSE(unavailable->present);
    CHECK(unavailable->values.empty());
    const auto current=restarted.load_macro_asof(300'000,180'000);
    REQUIRE(current); REQUIRE(current->present);
    CHECK(current->method_version=="replacement"); CHECK(current->computed_at_ms==300'000);
    CHECK(current->values.at(0).value==9);
    const auto stale=restarted.load_macro_asof(300'000,60'000);
    REQUIRE(stale); CHECK_FALSE(stale->present);
}
