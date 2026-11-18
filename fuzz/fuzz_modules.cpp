// Fuzzing harness for the parsers that take bytes from outside: the built-in
// modules that read mail, images, JSON, CSV, gzip, regexes and formulas, and
// the template compiler.  Same method as the other harnesses (fuzz/chaos.hpp):
// a valid seed is mutated and each case runs in its own child, so a sanitizer
// abort or a hang takes down only that case.
//
//   fuzz_modules [iterations] [target...]
//
// Targets: mailparse image json csv gzip regex formula template (default: all).
// Seeds live in fuzz/seeds/ and are embedded by path at run time: run from the
// repository root, or set LUX_FUZZ_SEEDS.

#include "chaos.hpp"

#include <lux/request.hpp>
#include <lux/response.hpp>
#include <lux_script/builtin_module.hpp>
#include <lux_script/natives.hpp>
#include <lux_script/template.hpp>
#include <lux_script/value.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace lux_script;

static std::string seed_dir() {
    const char* e = std::getenv("LUX_FUZZ_SEEDS");
    return e ? e : "fuzz/seeds";
}

static std::vector<std::string> seeds(std::initializer_list<const char*> names) {
    std::vector<std::string> out;
    for (const char* n : names) {
        std::ifstream f(seed_dir() + "/" + n, std::ios::binary);
        if (f) out.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return out;
}

// Calls <module>.<fn>(args...) the way the VM does.  A returned error string is a normal outcome
// (a refused input); only crashes, hangs and sanitizer reports are failures.
static void call(const char* module, const char* fn, std::vector<Value> args) {
    const BuiltinModuleFn* f = BuiltinModuleRegistry::instance().find(module, fn);
    if (!f) { std::fprintf(stderr, "fuzz_modules: %s.%s is not registered\n", module, fn); _exit(2); }
    lux::Request  req;
    lux::Response res;
    NativeCtx     ctx{req, res};
    std::string   error;
    (void)f->call(ctx, args, error);
}

// A case as a file, for the functions that read from a path.
static std::string as_file(const std::string& bytes, const char* ext) {
    const std::string path = "/tmp/fuzz_modules_" + std::to_string(getpid()) + ext;
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
}

struct Target {
    const char*                                 name;
    std::vector<std::string>                    seeds;
    void                                        (*run)(const std::string&);
};

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 20000;
    std::set<std::string> only;
    for (int i = 2; i < argc; ++i) only.insert(argv[i]);

    const std::vector<Target> targets = {
        {"mailparse", seeds({"mail.eml"}), [](const std::string& c) {
             call("mailparse", "parse", {Value::str(c)});
             call("mailparse", "attachment", {Value::str(c), Value::integer(0)});
             call("mailparse", "attachment", {Value::str(c), Value::integer(1)});
         }},
        {"image", seeds({"tiny.png", "tiny.jpg", "exif.jpg", "tiny.webp"}), [](const std::string& c) {
             const std::string in = as_file(c, ".img"), out = in + ".out.png";
             call("image", "info", {Value::str(in)});
             Value::Dict o;
             o["width"] = Value::integer(16);
             o["height"] = Value::integer(16);
             o["fit"] = Value::str("cover");
             call("image", "resize", {Value::str(in), Value::str(out), Value::dict(o)});
             std::remove(in.c_str());
             std::remove(out.c_str());
         }},
        {"json", seeds({"data.json"}), [](const std::string& c) {
             Value v;
             if (Value::parse_json(c, v)) {
                 (void)v.to_json_text();
                 (void)v.equals(v);
                 call("json", "stringify", {v, Value::integer(2)});
             }
             call("json", "parse", {Value::str(c)});
         }},
        {"csv", seeds({"data.csv"}), [](const std::string& c) {
             call("csv", "parse", {Value::str(c)});
             call("csv", "parse", {Value::str(c), Value::boolean(true)});
         }},
        {"gzip", seeds({"data.gz"}), [](const std::string& c) {
             call("gzip", "decompress", {Value::str(c), Value::integer(1 << 20)});
         }},
        {"regex", seeds({"regex.txt"}), [](const std::string& c) {
             call("regex", "test", {Value::str(c), Value::str("abcabc123 aab 12")});
             call("regex", "find_all", {Value::str(c), Value::str("aaaaaaaaaaaaaaaa!")});
             call("regex", "replace", {Value::str(c), Value::str("x"), Value::str("y")});
         }},
        {"formula", seeds({"formula.txt"}), [](const std::string& c) {
             Value::Dict vars;
             vars["nivel"] = Value::integer(3);
             vars["habilidad"] = Value::integer(50);
             call("formula", "check", {Value::str(c)});
             call("formula", "stats", {Value::str(c), Value::dict(vars)});
             call("formula", "roll", {Value::str(c), Value::dict(vars)});
         }},
        {"template", seeds({"page.html"}), [](const std::string& c) {
             DiagnosticBag diags;
             Template      out;
             (void)compile_template(c, "fuzz.html", "/nonexistent-template-dir", {}, diags, out);
         }},
    };

    int failures = 0;
    for (const auto& t : targets) {
        if (!only.empty() && !only.count(t.name)) continue;
        if (t.seeds.empty()) { std::fprintf(stderr, "fuzz_modules: no seeds for %s in %s\n", t.name, seed_dir().c_str()); return 2; }
        std::fprintf(stderr, "fuzz_modules: %s (%zu seeds, %d iterations)\n", t.name, t.seeds.size(), iterations);
        failures += chaos::run(t.name, iterations, 5000, t.seeds, t.run);
    }
    return failures ? 1 : 0;
}
