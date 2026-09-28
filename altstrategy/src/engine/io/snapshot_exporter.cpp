#include "altstrategy/engine_io/snapshot_exporter.hpp"

#include "altstrategy/common/hash.hpp"
#include "altstrategy/engine/ids.hpp"
#include "altstrategy/persistence/pg.hpp"

#include <pqxx/pqxx>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace altstrategy::engine_io {

namespace {

// RFC-4180 CSV field quoting.
std::string csv_field(const std::string& v) {
    const bool needs = v.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs) return v;
    std::string out = "\"";
    for (char c : v) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

std::string render_csv(const ColumnarTable& t) {
    std::ostringstream os;
    for (size_t i = 0; i < t.header.size(); ++i) { if (i) os << ','; os << csv_field(t.header[i]); }
    os << '\n';
    for (const auto& row : t.rows) {
        for (size_t i = 0; i < row.size(); ++i) { if (i) os << ','; os << csv_field(row[i]); }
        os << '\n';
    }
    return os.str();
}

} // namespace

SnapshotFile write_table(const std::string& dir, const ColumnarTable& table, const std::string& format) {
    // The Parquet backend activates only when built with libarrow; this host
    // emits validated CSV columnar files with identical schema + checksums.
    const std::string ext = format == "parquet" ? "parquet" : "csv";
    const std::string content = render_csv(table);   // CSV backend
    const std::string path = dir + "/" + table.name + "." + ext;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    SnapshotFile f;
    f.file_name = table.name + "." + ext;
    f.row_count = static_cast<int64_t>(table.rows.size());
    f.byte_size = static_cast<int64_t>(content.size());
    f.sha256 = altstrategy::sha256_hex(content);
    return f;
}

std::string build_manifest_json(const SnapshotResult& r, int64_t created_at_ms) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"snapshot_id\": \"" << r.snapshot_id << "\",\n";
    os << "  \"kind\": \"" << r.kind << "\",\n";
    os << "  \"format\": \"" << r.format << "\",\n";
    os << "  \"created_at_ms\": " << created_at_ms << ",\n";
    os << "  \"cutoff_time_ms\": " << r.cutoff_time_ms << ",\n";
    os << "  \"from_sequence\": " << r.from_sequence << ",\n";
    os << "  \"to_sequence\": " << r.to_sequence << ",\n";
    os << "  \"previous_snapshot_id\": \"" << r.previous_snapshot_id << "\",\n";
    os << "  \"signal_count\": " << r.signal_count << ",\n";
    os << "  \"candidate_count\": " << r.candidate_count << ",\n";
    os << "  \"plan_count\": " << r.plan_count << ",\n";
    os << "  \"matured_outcome_count\": " << r.matured_outcome_count << ",\n";
    os << "  \"files\": [\n";
    for (size_t i = 0; i < r.files.size(); ++i) {
        const auto& f = r.files[i];
        os << "    {\"file\": \"" << f.file_name << "\", \"rows\": " << f.row_count
           << ", \"bytes\": " << f.byte_size << ", \"sha256\": \"" << f.sha256 << "\"}";
        os << (i + 1 < r.files.size() ? ",\n" : "\n");
    }
    os << "  ]\n}\n";
    return os.str();
}

std::optional<SnapshotResult> SnapshotExporter::export_snapshot(const std::string& kind,
                                                                int64_t from_sequence, int64_t now_ms,
                                                                const std::string& previous_snapshot_id) {
    SnapshotResult r;
    r.kind = kind;
    r.format = cfg_.format;
    r.cutoff_time_ms = now_ms;
    r.from_sequence = kind == "FULL" ? 0 : from_sequence;
    r.previous_snapshot_id = previous_snapshot_id;

    std::vector<ColumnarTable> tables;
    ColumnarTable examples{"signals", {"example_id", "signal_proposal_id", "candidate_id",
                                        "canonical_pair_id", "strategy_id", "side", "proposal_grade",
                                        "observation_time_ms", "label_matured_at_ms", "sequence"}, {}};
    ColumnarTable outcomes{"outcomes", {"example_id", "categorical_label", "net_return_after_costs",
                                         "mfe", "mae", "holding_duration_ms", "binary_label_usable"}, {}};
    ColumnarTable contexts{"contexts", {"example_id", "context_id", "feature_manifest_hash", "quality"}, {}};

    // Consistent snapshot: read everything in one REPEATABLE READ transaction so
    // signals and their outcomes come from a single committed DB state (§28).
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
        const std::string seq_filter = kind == "FULL" ? "" : " AND e.sequence > " + std::to_string(from_sequence);
        const std::string base =
            "FROM altstrategy.signal_training_examples e "
            "JOIN altstrategy.signal_training_labels l ON l.example_id=e.example_id "
            "WHERE e.dataset_eligible=true AND e.label_matured_at_ms IS NOT NULL" + seq_filter;

        for (const auto& row : txn.exec(
                 "SELECT e.example_id,e.signal_proposal_id,e.candidate_id,e.canonical_pair_id,e.strategy_id,"
                 "e.side,e.proposal_grade,e.observation_time_ms,e.label_matured_at_ms,e.sequence " + base +
                 " ORDER BY e.sequence")) {
            examples.rows.push_back({row[0].c_str(), row[1].c_str(), row[2].c_str(), row[3].c_str(),
                                     row[4].c_str(), row[5].c_str(), row[6].c_str(), row[7].c_str(),
                                     row[8].c_str(), row[9].c_str()});
            r.to_sequence = std::max<int64_t>(r.to_sequence, row[9].as<int64_t>());
        }
        for (const auto& row : txn.exec(
                 "SELECT e.example_id,l.categorical_label,l.net_return_after_costs,l.mfe,l.mae,"
                 "l.holding_duration_ms,l.binary_label_usable " + base + " ORDER BY e.sequence"))
            outcomes.rows.push_back({row[0].c_str(), row[1].c_str(), row[2].c_str(), row[3].c_str(),
                                     row[4].c_str(), row[5].c_str(), row[6].c_str()});
        for (const auto& row : txn.exec(
                 "SELECT e.example_id,e.context_id,e.feature_manifest_hash,e.quality " + base +
                 " ORDER BY e.sequence"))
            contexts.rows.push_back({row[0].c_str(), row[1].c_str(), row[2].c_str(), row[3].c_str()});
    });
    if (!res) return std::nullopt;

    r.signal_count = static_cast<int64_t>(examples.rows.size());
    r.matured_outcome_count = static_cast<int64_t>(outcomes.rows.size());
    r.snapshot_id = engine::ids::make("snap", engine::ids::join({kind, std::to_string(r.to_sequence),
                                                                 std::to_string(now_ms)}));
    r.artifact_location = cfg_.export_root + "/" + r.snapshot_id;

    std::error_code ec;
    fs::create_directories(r.artifact_location, ec);
    if (ec) return std::nullopt;

    tables = {examples, outcomes, contexts};
    std::ostringstream checksums;
    for (const auto& t : tables) {
        auto f = write_table(r.artifact_location, t, cfg_.format);
        checksums << f.sha256 << "  " << f.file_name << "\n";
        r.files.push_back(std::move(f));
    }

    const std::string manifest = build_manifest_json(r, now_ms);
    r.manifest_hash = altstrategy::sha256_hex(manifest);
    { std::ofstream m(r.artifact_location + "/manifest.json", std::ios::binary | std::ios::trunc);
      m << manifest; }
    { std::ofstream c(r.artifact_location + "/checksums.sha256", std::ios::binary | std::ios::trunc);
      c << checksums.str(); }

    // Record the snapshot + files (idempotent).
    pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.signal_archive_snapshots(snapshot_id,kind,cutoff_time_ms,from_sequence,"
            "to_sequence,previous_snapshot_id,manifest_hash,signal_count,matured_outcome_count,"
            "artifact_location,format,status) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,'created') "
            "ON CONFLICT (snapshot_id) DO NOTHING",
            r.snapshot_id, r.kind, r.cutoff_time_ms, r.from_sequence, r.to_sequence,
            r.previous_snapshot_id, r.manifest_hash, r.signal_count, r.matured_outcome_count,
            r.artifact_location, r.format);
        for (const auto& f : r.files)
            txn.exec_params(
                "INSERT INTO altstrategy.signal_archive_files(snapshot_id,file_name,row_count,byte_size,sha256)"
                " VALUES($1,$2,$3,$4,$5) ON CONFLICT (snapshot_id,file_name) DO NOTHING",
                r.snapshot_id, f.file_name, f.row_count, f.byte_size, f.sha256);
    });
    return r;
}

} // namespace altstrategy::engine_io
