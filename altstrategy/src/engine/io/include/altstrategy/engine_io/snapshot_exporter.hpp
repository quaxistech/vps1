#pragma once

// training_snapshot_exporter (§26–§31). Materializes mature training examples into
// immutable, content-addressed snapshots under /var/lib/altstrategy/exports/
// signal-quality/<snapshot-id>/ with a manifest.json + checksums.sha256, in a
// consistent (REPEATABLE READ) DB snapshot. Supports FULL (VPS2 bootstrap) and
// INCREMENTAL (since a prior sequence watermark). The columnar writer is
// pluggable: a validated CSV backend now, a libarrow-gated Parquet backend on
// hosts where it is present (recorded in the manifest `format`). Runs inside the
// single service via the maintenance_scheduler — never a separate service.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace altstrategy::persistence { class PgPool; }

namespace altstrategy::engine_io {

struct ExporterConfig {
    std::string export_root = "/var/lib/altstrategy/exports/signal-quality";
    std::string format = "csv";     // "csv" | "parquet" (parquet requires libarrow)
};

struct SnapshotFile {
    std::string file_name;
    int64_t row_count = 0;
    int64_t byte_size = 0;
    std::string sha256;
};

struct SnapshotResult {
    std::string snapshot_id;
    std::string kind;               // FULL | INCREMENTAL
    int64_t cutoff_time_ms = 0;
    int64_t from_sequence = 0;
    int64_t to_sequence = 0;
    std::string previous_snapshot_id;
    std::string artifact_location;
    std::string manifest_hash;
    std::string format;
    int64_t signal_count = 0;
    int64_t candidate_count = 0;
    int64_t plan_count = 0;
    int64_t matured_outcome_count = 0;
    std::vector<SnapshotFile> files;
};

// --- testable columnar-writer core (no DB) ---
struct ColumnarTable {
    std::string name;               // -> <name>.<ext>
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
};

// Writes one table to `dir` in the given format, returning its file record
// (row count, byte size, SHA-256). CSV is RFC-4180 quoted.
[[nodiscard]] SnapshotFile write_table(const std::string& dir, const ColumnarTable& table,
                                       const std::string& format);

// Serializes the manifest.json for a snapshot and returns its content.
[[nodiscard]] std::string build_manifest_json(const SnapshotResult& r, int64_t created_at_ms);

class SnapshotExporter {
public:
    SnapshotExporter(altstrategy::persistence::PgPool& pool, ExporterConfig cfg)
        : pool_(pool), cfg_(std::move(cfg)) {}

    // Export a FULL or INCREMENTAL snapshot. `from_sequence` is ignored for FULL.
    [[nodiscard]] std::optional<SnapshotResult> export_snapshot(const std::string& kind,
                                                                int64_t from_sequence, int64_t now_ms,
                                                                const std::string& previous_snapshot_id);

private:
    altstrategy::persistence::PgPool& pool_;
    ExporterConfig cfg_;
};

} // namespace altstrategy::engine_io
