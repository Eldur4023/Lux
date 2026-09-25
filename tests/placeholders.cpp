// Test of the postgres placeholder translator, with no server involved.
//
// It includes the driver's .cpp to reach the function, which lives in an
// anonymous namespace: that way THE code is tested, not a copy of it that
// could drift without anyone noticing.
#include "../src/lux_script/db_postgres.cpp"

#include <cstdio>
#include <string>

static int failures = 0;

static void good(const char* name, const std::string& sql, size_t nargs,
                 const std::string& expected) {
    std::string out, err;
    if (!lux_script::traducir_marcadores(sql, nargs, out, err)) {
        ++failures;
        std::printf("  FAIL %s\n    unexpected error: %s\n", name, err.c_str());
        return;
    }
    if (out != expected) {
        ++failures;
        std::printf("  FAIL %s\n    expected: <<%s>>\n    got: <<%s>>\n",
                    name, expected.c_str(), out.c_str());
        return;
    }
    std::printf("  ok    %s\n", name);
}

static void bad(const char* name, const std::string& sql, size_t nargs,
                const std::string& trozo) {
    std::string out, err;
    if (lux_script::traducir_marcadores(sql, nargs, out, err)) {
        ++failures;
        std::printf("  FAIL %s\n    passed when it should not: <<%s>>\n", name, out.c_str());
        return;
    }
    if (err.find(trozo) == std::string::npos) {
        ++failures;
        std::printf("  FAIL %s\n    expected it to say '%s'\n    said: %s\n",
                    name, trozo.c_str(), err.c_str());
        return;
    }
    std::printf("  ok    %s\n", name);
}

int main() {
    std::printf("== basic translation ==\n");
    good("one placeholder", "select * from t where id = ?", 1,
         "select * from t where id = $1");
    good("three placeholders", "select ? , ? where a = ?", 3,
         "select $1 , $2 where a = $3");
    good("no placeholders", "select 1", 0, "select 1");

    std::printf("== compatibility with the postgres style ==\n");
    good("already came with $1", "select * from t where id = $1", 1,
         "select * from t where id = $1");
    good("$1 and $2", "insert into t values ($1, $2)", 2,
         "insert into t values ($1, $2)");

    std::printf("== what is NOT a placeholder ==\n");
    good("inside a string", "select * from t where s = 'what?' and id = ?", 1,
         "select * from t where s = 'what?' and id = $1");
    good("escaped quote inside", "select * from t where s = 'a''b?' and id = ?", 1,
         "select * from t where s = 'a''b?' and id = $1");
    good("string with E and a backslash", "select * from t where s = E'a\\'?' and id = ?", 1,
         "select * from t where s = E'a\\'?' and id = $1");
    good("quoted identifier", "select \"col?\" from t where id = ?", 1,
         "select \"col?\" from t where id = $1");
    good("line comment", "-- what?\nselect ?", 1, "-- what?\nselect $1");
    good("block comment", "/* ? */ select ?", 1, "/* ? */ select $1");
    good("nested comment", "/* a /* ? */ ? */ select ?", 1,
         "/* a /* ? */ ? */ select $1");
    good("block with $$", "select $$ ? $$ , ?", 1, "select $$ ? $$ , $1");
    good("tagged block", "select $x$ ? $x$ , ?", 1, "select $x$ ? $x$ , $1");

    std::printf("== the JSONB operator ==\n");
    // With no arguments nothing is translated: the `?` is the JSONB operator.
    good("with no arguments, it is left alone", "select * from t where data ? 'key'", 0,
         "select * from t where data ? 'key'");
    // With arguments, `??` is the escape hatch for that operator.
    good("?? is a literal ?", "select * from t where data ?? 'k' and id = ?", 1,
         "select * from t where data ? 'k' and id = $1");

    std::printf("== errors ==\n");
    bad("mixing the two styles", "select * from t where a = $1 and b = ?", 2, "mixes");
    bad("too many arguments", "select * from t where id = ?", 2, "1 '?' placeholder");
    bad("too few arguments", "select * from t where a = ? and b = ?", 1, "2 '?' placeholder");

    std::printf("\n%s\n", failures ? "THERE ARE FAILURES" : "all passing");
    return failures ? 1 : 0;
}
