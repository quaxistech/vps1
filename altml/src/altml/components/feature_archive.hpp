#pragma once
#include "altml/core/pg.hpp"
#include "altml/components/trainer_logic.hpp"
#include "altml/components/inference_logic.hpp"
#include "system.pb.h"
#include <stdexcept>

namespace altml::inference {
// Existing archive key/feature values remain first-wins. Retain the original
// live wire evidence even when an earlier backfill already occupied that key.
inline Result<void> archive_live_feature(persistence::PgPool& pool, const altml::v1::Envelope& env,
    const altml::v1::FeatureVectorV1& fv, const std::string& tf_label,
    const std::string& schema_hash, std::string_view original_payload,
    int64_t* durable_received_at_ms = nullptr) {
        int64_t received_at_ms = 0;
        auto res = pool.with_txn([&](pqxx::work& txn) {
            const auto stored = txn.exec_params(
                "INSERT INTO altml.feature_vectors (exchange_id, instrument_id, timeframe, "
                "bar_open_time, bar_revision, feature_set_name, feature_set_version, "
                "schema_hash, engine_version, values, validity_bitmap, quality_status, "
                "computed_at, ingest_path, original_live_payload, original_live_values, "
                "original_live_validity_bitmap, original_live_computed_at, original_live_received_at, original_live_engine_version) "
                "VALUES ($1, $2, $3, to_timestamp($4::bigint / 1000.0), $5, $6, $7, $8, $9, "
                "$10::real[], $11::bytea, 'VALID', to_timestamp($12::bigint / 1000.0), 'live', $13::bytea, "
                "$10::real[], $11::bytea, to_timestamp($12::bigint / 1000.0), clock_timestamp(), $9) "
                "ON CONFLICT (exchange_id, instrument_id, timeframe, bar_open_time, "
                "bar_revision, schema_hash) DO UPDATE SET original_live_payload = "
                "COALESCE(altml.feature_vectors.original_live_payload, EXCLUDED.original_live_payload), "
                "original_live_values = COALESCE(altml.feature_vectors.original_live_values, EXCLUDED.original_live_values), "
                "original_live_validity_bitmap = COALESCE(altml.feature_vectors.original_live_validity_bitmap, EXCLUDED.original_live_validity_bitmap), "
                "original_live_computed_at = COALESCE(altml.feature_vectors.original_live_computed_at, EXCLUDED.original_live_computed_at), "
                "original_live_engine_version = COALESCE(altml.feature_vectors.original_live_engine_version, EXCLUDED.original_live_engine_version), "
                "original_live_received_at = COALESCE(altml.feature_vectors.original_live_received_at, EXCLUDED.original_live_received_at) "
                "RETURNING original_live_payload, (extract(epoch FROM original_live_received_at) * 1000)::bigint",
                static_cast<int>(env.exchange()), env.instrument_id(), tf_label,
                fv.bar_open_time_ms(), fv.bar_revision(),
                fv.feature_set_name(), fv.feature_set_version(),
                schema_hash,
                fv.engine_version(), training::pg_float_array_literal(fv.values()),
                training::pg_bytea_hex_literal(fv.validity_bitmap()), fv.computed_at_ms(),
                training::pg_bytea_hex_literal(original_payload));
            if (stored.empty()) throw std::runtime_error("conflicting original live feature payload");
            received_at_ms = stored[0][1].as<int64_t>();
            const pqxx::binarystring original(stored[0][0]);
            const std::string_view original_bytes(reinterpret_cast<const char*>(original.data()), original.size());
            if (original_bytes != original_payload) {
                // Transport metadata may differ on a republish. Only the same
                // original model-input vector can share an existing live key.
                alttech::v1::Envelope upstream;
                altml::v1::Envelope first;
                if (!upstream.ParseFromArray(original.data(), static_cast<int>(original.size())) ||
                    !repack_feature_envelope(upstream, &first) ||
                    first.exchange() != env.exchange() || first.instrument_id() != env.instrument_id() ||
                    first.feature_vector().SerializeAsString() != fv.SerializeAsString())
                    throw std::runtime_error("conflicting original live feature vector");
            }
        });
    if (res && durable_received_at_ms != nullptr) *durable_received_at_ms = received_at_ms;
    return res;
}
} // namespace altml::inference
