# Native modules

Everything under here compiles into `lux_script` and registers itself automatically.
There is no central list to edit anymore — see NATIVE-MODULES.md §5 for the full story
of why, and `include/lux_script/builtin_module.hpp`'s comment on `LUX_REGISTER_MODULE`
for the mechanism.

- **`base_modules/`** — the officially shipped modules (`hash`, `csv`, `os`, `math`,
  `time`, `regex`, `pdf`, `http`). Treat it as upstream code: changes here are project
  changes, not app-specific ones.
- **This folder itself** — drop your own `.cpp` here (a sibling of `base_modules/`, not
  inside it) to add a module of your own. CMake picks up any `.cpp` file placed directly
  in either location and compiles it into the build automatically, the next time you
  reconfigure (`cmake -S . -B build` picks up new files on its own thanks to
  `CONFIGURE_DEPENDS` — a plain `cmake --build build` after adding a file may not, run
  the former once if the new module does not seem to appear).

## Writing one

A module is one `.cpp` file:

```cpp
#include <lux_script/builtin_module.hpp>

namespace lux_script {
namespace {

Value fn_qrcode_generate(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "qrcode.generate() expects a string"; return Value::null(); }
    return Value::str(/* ... */);
}

class QrcodeModule : public BuiltinModule {
public:
    const char* name() const override { return "qrcode"; }
    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"generate", 1, 1, fn_qrcode_generate},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(QrcodeModule)

} // namespace lux_script
```

That last line is the entire "registration" step — no edit to `builtin_module.cpp`, no
`slots_[...]` entry, nothing else in the project needs to change. `name()` is what
`import` resolves against; there is no second name to keep in sync with it, because
`LUX_REGISTER_MODULE` reads it back off the class itself at startup.

Name the file after the module (`qrcode.cpp`, not `module_qrcode.cpp`) — the folder
already says "this is a module", so the prefix used to exist elsewhere in the project is
redundant here.

## If the module needs a third-party library

CMake cannot know a `pkg-config`/`find_library` check is needed just from a `.cpp`
file existing, so that part still needs a few lines by hand. Two things to add, both in
`CMakeLists.txt`, following `LUX_PDF`'s block (cairo) as the worked example:

1. An `option()` and a dependency check (`pkg_check_modules`/`find_library`) that only
   proceeds if the library is actually present — the same skip-if-missing pattern every
   other optional dependency in this project follows.
2. Add your file's path to `LUX_MODULE_EXCLUDE` *before* the glob runs, so the
   unconditional auto-discovery does not try to compile it when the dependency check
   above decided to skip it — then add it back with `target_sources(lux_script PRIVATE ...)`
   inside the `if()` branch where the dependency was found, exactly like `LUX_PDF`/
   `LUX_HTTP` already do.

A dependency-free module never needs either step: dropping the file in is genuinely the
whole thing.

## Opting a file out of the automatic build

Pass `-DLUX_MODULE_EXCLUDE="path/to/file.cpp;other.cpp"` to `cmake` (paths relative to
the repo root), or set `LUX_MODULE_EXCLUDE` before the glob in a local, uncommitted
`CMakeLists.txt` override. Anything listed there is skipped even though it lives under
`modules/`.
