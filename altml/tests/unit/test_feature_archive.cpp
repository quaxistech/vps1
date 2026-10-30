#include "altml/components/feature_archive.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <sstream>
using namespace altml;

namespace {
std::string text(const std::string& path) {
    std::ifstream input(path); std::ostringstream out; out << input.rdbuf();
    if (!input) throw std::runtime_error("fixture source unavailable");
    return out.str();
}
}

TEST_CASE("original live features commit before successful consumption even after backfill") {
    const char* dsn = std::getenv("ALTML_TEST_PG_DSN");
    if (!dsn) SKIP("dedicated local test database required");
    pqxx::connection observer(dsn);
    REQUIRE(std::string(observer.dbname()).starts_with("altml_audit_test_"));
    REQUIRE(std::string(observer.hostname()).starts_with("/"));
    {
        pqxx::work tx(observer);
        tx.exec("DROP SCHEMA IF EXISTS altml CASCADE; CREATE SCHEMA altml");
        const auto schema = text(std::string(ALTML_SOURCE_DIR) + "/db/schema.sql");
        const auto start = schema.find("CREATE TABLE IF NOT EXISTS altml.feature_vectors (");
        const auto end = schema.find(") PARTITION BY RANGE (bar_open_time);", start);
        REQUIRE(start != std::string::npos); REQUIRE(end != std::string::npos);
        tx.exec(schema.substr(start, end + std::string(") PARTITION BY RANGE (bar_open_time);").size() - start));
        tx.exec("CREATE TABLE altml.fixture_partition PARTITION OF altml.feature_vectors DEFAULT");
        tx.commit();
    }
    persistence::PgPool pool(dsn, 2);
    alttech::v1::Envelope input;
    input.set_schema_version(1); input.set_exchange(alttech::v1::EXCHANGE_BITGET);
    input.set_instrument_id(42); input.set_canonical_pair_id(42); input.set_exchange_symbol("FIXTURE");
    auto* fv = input.mutable_feature_vector();
    fv->set_timeframe(alttech::v1::TF_1M); fv->set_bar_open_time_ms(1'700'000'000'000);
    fv->set_computed_at_ms(1'700'000'060'000); fv->set_status(alttech::v1::FEATURE_STATUS_VALID);
    fv->set_feature_set_name("fixture"); fv->set_feature_set_version(1);
    fv->set_schema_hash("fixture-schema"); fv->set_engine_version("fixture-engine");
    fv->add_values(1.25f); fv->set_validity_bitmap(std::string(1,'\1'));
    const auto consume = [&](const alttech::v1::Envelope& raw) {
        altml::v1::Envelope decoded;
        REQUIRE(inference::repack_feature_envelope(raw, &decoded));
        return inference::archive_live_feature(pool, decoded, decoded.feature_vector(), "1m",
                                               "fixture-schema", raw.SerializeAsString());
    };
    CHECK_FALSE(consume(input)); // old schema / failed persistence is an error
    {
        pqxx::work tx(observer);
        tx.exec(text(std::string(ALTML_SOURCE_DIR) + "/db/migrations/0001_original_live_payload.sql"));
        tx.exec(text(std::string(ALTML_SOURCE_DIR) + "/db/migrations/0002_live_feature_projection.sql"));
        tx.exec(text(std::string(ALTML_SOURCE_DIR) + "/db/migrations/0002_live_feature_projection.sql"));
        tx.exec_params("INSERT INTO altml.feature_vectors(exchange_id,instrument_id,timeframe,bar_open_time,"
            "bar_revision,feature_set_name,feature_set_version,schema_hash,engine_version,values,validity_bitmap,"
            "computed_at,ingest_path) VALUES(1,42,'1m',to_timestamp($1::bigint/1000.0),0,'fixture',1,"
            "'fixture-schema','fixture-engine','{99}','\\x01',now(),'backfill')", fv->bar_open_time_ms());
        tx.commit();
    }
    REQUIRE(consume(input)); // original broker bytes, not the later upstream reconstruction
    REQUIRE(consume(input)); // duplicate after retry
    auto transport_retry = input;
    transport_retry.set_produced_at_ms(1'700'000'061'000);
    REQUIRE(consume(transport_retry)); // same vector with transport metadata changed
    {
        pqxx::read_transaction tx(observer);
        const auto rows = tx.exec("SELECT values[1],original_live_payload,ingest_path FROM altml.feature_vectors");
        REQUIRE(rows.size() == 1);
        CHECK(rows[0][0].as<float>() == 99); // historical backfill values were not overwritten
        const pqxx::binarystring original(rows[0][1]);
        CHECK(std::string(reinterpret_cast<const char*>(original.data()),original.size()) == input.SerializeAsString());
        CHECK(rows[0][2].as<std::string>() == "backfill");
        const auto projected = tx.exec("SELECT e.values[1], e.ingest_path, f.original_live_received_at "
            "FROM altml.econ_feature_vectors e JOIN altml.feature_vectors f USING "
            "(exchange_id,instrument_id,timeframe,bar_open_time,bar_revision,schema_hash)");
        REQUIRE(projected.size() == 1);
        CHECK(projected[0][0].as<float>() == 1.25f);
        CHECK(projected[0][1].as<std::string>() == "live");
        CHECK_FALSE(projected[0][2].is_null());
    }
    auto conflicting = input; conflicting.mutable_feature_vector()->set_values(0, 2.5f);
    CHECK_FALSE(consume(conflicting)); // no false durability for a conflicting live identity
    conflicting.mutable_feature_vector()->set_bar_revision(1);
    REQUIRE(consume(conflicting)); // a real upstream revision is a separate archive key
    {
        persistence::PgPool restored_pool(dsn,2);
        altml::v1::Envelope decoded;
        REQUIRE(inference::repack_feature_envelope(input,&decoded));
        REQUIRE(inference::archive_live_feature(restored_pool,decoded,decoded.feature_vector(),"1m",
                                                "fixture-schema",input.SerializeAsString()));
    }
    pqxx::read_transaction tx(observer);
    CHECK(tx.exec("SELECT count(*) FROM altml.feature_vectors")[0][0].as<int>() == 2);
}
