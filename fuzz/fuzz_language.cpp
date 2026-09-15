// Fuzzing harness for the Lux Script frontend: lexer, parser, checker.
//
// Seeds: every .lux in the given directory (tests/cases by default).
// Each mutated case is written to a temporary file and compiled with
// lux_script::compile(), the SAME function `lux --check` uses -- the real
// path is tested, not a summarized version of it.
//
//   fuzz_language [seeds-directory] [iterations]

#include "chaos.hpp"
#include <lux_script/project.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static std::vector<std::string> cargar_semillas(const std::string& dir) {
    std::vector<std::string> out;
    if (!fs::exists(dir)) return out;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".lux") continue;
        std::ifstream f(e.path(), std::ios::binary);
        if (!f) continue;
        out.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return out;
}

int main(int argc, char** argv) {
    std::string seeds_dir = argc > 1 ? argv[1] : "tests/cases";
    int iterations = argc > 2 ? std::atoi(argv[2]) : 20000;

    auto seeds = cargar_semillas(seeds_dir);
    if (seeds.empty()) {
        std::fprintf(stderr, "fuzz_language: no .lux files in %s\n", seeds_dir.c_str());
        return 1;
    }
    std::fprintf(stderr, "fuzz_language: %zu seeds from %s, %d iterations\n", seeds.size(),
                  seeds_dir.c_str(), iterations);

    int failures = chaos::run(
        "language", iterations, 2000, seeds, [](const std::string& case_) {
            std::string path = "/tmp/fuzz_language_case_" + std::to_string(getpid()) + ".lux";
            {
                std::ofstream f(path, std::ios::binary);
                f << case_;
            }
            lux_script::DiagnosticBag diags;
            auto mod = lux_script::compile({fs::path(path)}, diags);
            (void)mod;
            std::remove(path.c_str());
        });

    return failures ? 1 : 0;
}
