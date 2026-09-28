#pragma once
// What "the database matches db/schema.sql" is checksummed over.
//
// AltTech keeps no migration lineage: the schema file is the whole truth, and a
// database that disagrees with it is rebuilt rather than evolved. That makes the
// comparison itself important. Hashing the file verbatim would mean that fixing
// a stale comment reports the database as out of date and demands `--recreate`,
// which destroys every analytics table — so the cheapest possible edit carries
// the highest possible cost, and the incentive is to leave comments wrong.
//
// So the digest is taken over what the file *asks PostgreSQL to do*: comments
// removed and runs of whitespace collapsed. Editing prose is then free, while
// any change to a statement still shows up.
//
// The normalisation is a real SQL lexer for the three quoting forms the schema
// uses, not a regex: inside a string literal, a quoted identifier or a
// dollar-quoted function body, `--` and `/*` are ordinary characters, and
// treating them as comment starts would silently delete SQL.

#include <cctype>
#include <string>
#include <string_view>

namespace alttech::tools::schema {

namespace detail {

// Reads the dollar-quote tag starting at `pos` ("$$" or "$name$"), returning it
// including both dollars, or empty when this `$` does not open one.
[[nodiscard]] inline std::string_view dollar_tag(std::string_view sql, std::size_t pos) {
    std::size_t i = pos + 1;
    while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) != 0 ||
                              sql[i] == '_')) {
        ++i;
    }
    if (i < sql.size() && sql[i] == '$') return sql.substr(pos, i - pos + 1);
    return {};
}

[[nodiscard]] inline bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

// Characters PostgreSQL can combine into a multi-character operator. Two of them
// meeting across removed whitespace would form a token that was not written.
[[nodiscard]] inline bool operator_char(char c) {
    return std::string_view("+-*/<>=~!@#%^&|?").find(c) != std::string_view::npos;
}

// Whether dropped whitespace between these two characters has to be replaced by
// a single space, rather than removed outright. Only fusion matters: "a INT"
// must not become "aINT", while "t (a" and "t(a" are the same statement.
[[nodiscard]] inline bool needs_separator(char prev, char next) {
    return (word_char(prev) && word_char(next)) || (operator_char(prev) && operator_char(next));
}

// Copies a run delimited by `quote`, honouring the doubled-quote escape ('' and
// ""), which is how SQL embeds the delimiter in its own literal.
inline void copy_quoted(std::string_view sql, std::size_t& i, char quote, std::string& out) {
    out += sql[i++];
    while (i < sql.size()) {
        if (sql[i] == quote) {
            if (i + 1 < sql.size() && sql[i + 1] == quote) {
                out += sql[i];
                out += sql[i + 1];
                i += 2;
                continue;
            }
            out += sql[i++];
            return;
        }
        out += sql[i++];
    }
}

} // namespace detail

// Strips comments and collapses whitespace, preserving every quoted region
// byte for byte. The result is meant for hashing, not for execution.
[[nodiscard]] inline std::string normalize_sql(std::string_view sql) {
    std::string out;
    out.reserve(sql.size());
    bool pending_space = false;

    // Replays the whitespace that was dropped before `next`, but only where
    // leaving it out would fuse two tokens into one.
    const auto separate = [&](char next) {
        if (pending_space && !out.empty() && detail::needs_separator(out.back(), next)) {
            out += ' ';
        }
        pending_space = false;
    };

    for (std::size_t i = 0; i < sql.size();) {
        const char c = sql[i];

        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            pending_space = true;
            ++i;
            continue;
        }

        // -- line comment
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') ++i;
            pending_space = true;
            continue;
        }

        // /* block comment */, which PostgreSQL allows to nest
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            int depth = 1;
            i += 2;
            while (i < sql.size() && depth > 0) {
                if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
                    ++depth;
                    i += 2;
                } else if (sql[i] == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
                    --depth;
                    i += 2;
                } else {
                    ++i;
                }
            }
            pending_space = true;
            continue;
        }

        // $$ ... $$ or $tag$ ... $tag$ — a function body, kept verbatim
        if (c == '$') {
            const std::string_view tag = detail::dollar_tag(sql, i);
            if (!tag.empty()) {
                const std::size_t end = sql.find(tag, i + tag.size());
                const std::size_t stop = end == std::string_view::npos
                                             ? sql.size()
                                             : end + tag.size();
                separate(sql[i]);
                out.append(sql.substr(i, stop - i));
                i = stop;
                continue;
            }
        }

        if (c == '\'' || c == '"') {
            separate(c);
            detail::copy_quoted(sql, i, c, out);
            continue;
        }

        separate(c);
        out += c;
        ++i;
    }
    return out;
}

} // namespace alttech::tools::schema
