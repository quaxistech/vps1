// The schema checksum decides whether `alttech-schema` reports the database as
// out of date, and the only remedy it offers is `--recreate`, which destroys
// every analytics table. So what the checksum reacts to has real consequences:
// it must ignore prose and must not ignore a single character of SQL.

#include "sql_digest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using alttech::tools::schema::normalize_sql;

// --- prose must be free ----------------------------------------------------

TEST_CASE("editing a comment does not change the digest") {
    const std::string before =
        "-- The active zone set at one closed bar. Its primary key doubles as\n"
        "-- the durable marker for the stateful zone lifecycle.\n"
        "CREATE TABLE alttech.structure_snapshots (id BIGINT);\n";
    const std::string after =
        "-- The active zone set at one closed bar. Zone state is derived from\n"
        "-- the bar window on every cycle.\n"
        "CREATE TABLE alttech.structure_snapshots (id BIGINT);\n";
    CHECK(normalize_sql(before) == normalize_sql(after));
}

TEST_CASE("reindenting and rewrapping does not change the digest") {
    const std::string dense = "CREATE TABLE t (a INT, b INT);";
    const std::string spread = "CREATE   TABLE t (\n    a INT,\n    b INT\n);\n\n";
    CHECK(normalize_sql(dense) == normalize_sql(spread));
}

TEST_CASE("a trailing comment is removed like a leading one") {
    CHECK(normalize_sql("CREATE TABLE t (a INT); -- why\n") ==
          normalize_sql("CREATE TABLE t (a INT);\n"));
}

TEST_CASE("a block comment is removed, including a nested one") {
    CHECK(normalize_sql("CREATE /* an /* inner */ aside */ TABLE t (a INT);") ==
          normalize_sql("CREATE TABLE t (a INT);"));
}

// --- SQL must not be free --------------------------------------------------

TEST_CASE("changing a column type changes the digest") {
    CHECK(normalize_sql("CREATE TABLE t (a INT);") !=
          normalize_sql("CREATE TABLE t (a BIGINT);"));
}

TEST_CASE("adding an index changes the digest") {
    const std::string base = "CREATE TABLE t (a INT);";
    CHECK(normalize_sql(base) != normalize_sql(base + " CREATE INDEX i ON t (a);"));
}

TEST_CASE("removing whitespace does not fuse two tokens into one") {
    // The failure this guards against is collapsing "a INT" to "aINT" and
    // thereby making two different schemas hash alike.
    CHECK(normalize_sql("CREATE TABLE t (a INT);") != normalize_sql("CREATE TABLE t (aINT);"));
    CHECK(normalize_sql("SELECT  a  ,  b") == "SELECT a,b");
}

TEST_CASE("changing a retention interval changes the digest") {
    CHECK(normalize_sql("VALUES ('t', interval '4 days');") !=
          normalize_sql("VALUES ('t', interval '2 days');"));
}

// --- quoting: where a naive strip would delete SQL -------------------------

TEST_CASE("a double dash inside a string literal is not a comment") {
    const std::string sql = "INSERT INTO p VALUES ('sub-minute -- not a comment', 10);";
    CHECK(normalize_sql(sql).find("-- not a comment") != std::string::npos);
    CHECK(normalize_sql(sql).find("10") != std::string::npos);
}

TEST_CASE("a block comment opener inside a string literal is not a comment") {
    const std::string sql = "INSERT INTO p VALUES ('ratio a/*b', 20);";
    CHECK(normalize_sql(sql).find("20") != std::string::npos);
}

TEST_CASE("whitespace inside a string literal is preserved") {
    // Collapsing it would make two different rationales hash alike, and would
    // corrupt the value if the digest were ever executed.
    CHECK(normalize_sql("VALUES ('two  spaces')") == "VALUES('two  spaces')");
    CHECK(normalize_sql("VALUES ('a b')") != normalize_sql("VALUES ('ab')"));
}

TEST_CASE("a doubled quote inside a literal does not end it") {
    const std::string sql = "VALUES ('it''s -- fine', 30);";
    CHECK(normalize_sql(sql).find("30") != std::string::npos);
    CHECK(normalize_sql(sql).find("-- fine") != std::string::npos);
}

TEST_CASE("a quoted identifier is preserved and does not start a comment") {
    const std::string sql = "CREATE TABLE \"odd -- name\" (a INT);";
    CHECK(normalize_sql(sql).find("odd -- name") != std::string::npos);
    CHECK(normalize_sql(sql).find("INT") != std::string::npos);
}

TEST_CASE("a dollar-quoted function body is kept verbatim") {
    // The schema's partition helper is written this way, and its body contains
    // both comments and formatting that PostgreSQL stores as given.
    const std::string sql =
        "CREATE FUNCTION f() RETURNS INT AS $$\n"
        "BEGIN\n"
        "    -- inside the body\n"
        "    RETURN 1;\n"
        "END;\n"
        "$$ LANGUAGE plpgsql;";
    const std::string norm = normalize_sql(sql);
    CHECK(norm.find("-- inside the body") != std::string::npos);
    CHECK(norm.find("BEGIN\n") != std::string::npos);
    // Changing the body still changes the digest.
    std::string changed = sql;
    changed.replace(changed.find("RETURN 1"), 8, "RETURN 2");
    CHECK(normalize_sql(sql) != normalize_sql(changed));
}

TEST_CASE("a tagged dollar quote is matched by its tag, not by any dollar pair") {
    const std::string sql = "CREATE FUNCTION f() RETURNS TEXT AS $body$ SELECT '$$'; $body$ "
                            "LANGUAGE sql;";
    const std::string norm = normalize_sql(sql);
    CHECK(norm.find("$body$") != std::string::npos);
    CHECK(norm.find("LANGUAGE sql") != std::string::npos);
}

TEST_CASE("a lone dollar is not treated as a quote") {
    // Parameter placeholders appear in the schema's function bodies.
    CHECK(normalize_sql("SELECT $1, $2;") == "SELECT $1,$2;");
}

TEST_CASE("dropped whitespace never fuses two operators into a comment") {
    // "a - -1" collapsed to "a --1" would read as a comment, making two
    // different expressions hash alike.
    CHECK(normalize_sql("SELECT a - -1;") != normalize_sql("SELECT a - 1;"));
    CHECK(normalize_sql("SELECT a - -1;").find("- -") != std::string::npos);
}

// --- the real file ---------------------------------------------------------

TEST_CASE("normalising is idempotent and loses no statement terminator") {
    const std::string sql =
        "-- header\n"
        "CREATE SCHEMA IF NOT EXISTS alttech;\n"
        "CREATE TABLE alttech.t (a INT);  -- trailing\n";
    const std::string once = normalize_sql(sql);
    CHECK(normalize_sql(once) == once);
    CHECK(once == "CREATE SCHEMA IF NOT EXISTS alttech;CREATE TABLE alttech.t(a INT);");
}
