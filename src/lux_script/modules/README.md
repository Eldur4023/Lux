# Native modules

Everything under here compiles into `lux_script` and registers itself automatically.
There is no central list to edit anymore — see NATIVE-MODULES.md §5 for the full story
of why, and `include/lux_script/builtin_module.hpp`'s comment on `LUX_REGISTER_MODULE`
for the mechanism.

- **`base_modules/`** — the officially shipped modules (`hash`, `encoding`, `text`, `math`,
  `time`, `regex`, `json`, `csv`, `os`, `proc`, `rooms`, `net`, `zip`, `pdf`, `http`). Treat it
  as upstream code: changes here are project changes, not app-specific ones.
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
    if (args[0].as_str().empty()) { error = "qrcode.generate(): nothing to encode"; return Value::null(); }
    return Value::str(/* ... */);
}

} // namespace

LUX_MODULE(qrcode, {
    {"generate", "s|i", fn_qrcode_generate},
})

} // namespace lux_script
```

`LUX_MODULE` is the whole registration: the name is what `import` resolves against, and
nothing else in the project changes.

The string after the function name is its **signature**, checked before the function runs, so
the body can use `as_str()`/`as_int()` without checking: `s` string, `i` int, `n` int or
float, `b` bool, `l` List, `d` Dict, `f` function, `x` anything; an uppercase letter also
accepts `null`; what follows `|` is optional; a trailing `*` takes any number of further
arguments; `>` then one letter declares what it returns (`s` `i` `b` `l` `d`, `r` float;
nothing, or anything else, is `Json`) — the compiler checks a call site's use of it, so leave it
out for a function that can return `null`. The argument count the compiler checks comes from
it too: `{"hmac_sha256", "ss>s", fn}`. Set `error` to fail the
call (a catchable Lux Script error, or `{"error": ...}` for an async function), and pass
`/*is_async=*/true` as a fourth field for anything that waits on disk, the network, a child
process or real CPU work, so it runs on the I/O pool and has to be `await`ed.

A module that needs an `app:` block writes its own `BuiltinModule` subclass, overriding
`configure()`, and registers it with `LUX_REGISTER_MODULE(ClassName)`.

Name the file after the module (`qrcode.cpp`).

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
