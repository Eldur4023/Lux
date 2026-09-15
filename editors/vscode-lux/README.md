# Lux Script — VS Code extension

Syntax highlighting, live diagnostics and completion for `.lux` files. See
`LUX_SCRIPT-GRAMMAR.md` for the language this follows.

## What it provides

- **Syntax highlighting** — comments, strings (single and triple-quoted, including
  multi-line), numbers, keywords, primitives, native types (`Json`/`List`/`Dict`/`File`),
  reserved objects, function/method calls, operators. TextMate grammar
  (`syntaxes/lux.tmLanguage.json`), verified by tokenizing every `.lux` file in
  `tests/cases/` and `bench/` with `vscode-textmate`/`vscode-oniguruma` (the same engine VS
  Code uses) — zero exceptions.
- **Diagnostics** — real compiler errors, not reimplemented: the language server
  (`server/`) invokes the actual `lux` binary with `--json` (see
  `src/lux_script/main.cpp`) and turns its output straight into LSP diagnostics. There is
  no separate, second implementation of Lux's checking rules to keep in sync — the
  server is a thin adapter around the same compiler that ships the app.
- **Completion** — a static, context-free list (keywords, primitives, native types,
  reserved objects, builtins, and a few snippets for route/class/group/`on error`
  skeletons), from `LUX_SCRIPT-GRAMMAR.md` §2 and `GUIDE.md` §20. Not yet aware of a
  given file's own classes, functions or locals — see "What is not here yet" below.
- **Editor configuration** — `#` comments, bracket/quote auto-closing, indentation rules
  for Lux's Python-style indented blocks.
- **No injected global settings.** Colors come entirely from the grammar's scope names —
  `string.quoted.double.lux`, `entity.name.function.lux`, `support.type.property-name.lux`,
  and the rest all follow the standard `<role>.<subrole>.lux` convention, so any theme
  already has rules for the base scope (`string.quoted.double`, `entity.name.function`, …)
  and colors Lux source through ordinary TextMate prefix matching — no per-extension
  override needed. `package.json` sets exactly one default, `[lux]`'s
  `editor.semanticHighlighting.enabled: false` (there is no semantic token provider here,
  so there is no semantic data for it to disable) — scoped to `.lux` files only, unlike the
  global `editor.tokenColorCustomizations` key an earlier version of this extension wrote
  defaults into. **Trade-off, not glossed over:** a theme that colors mostly through
  *semantic* tokens (GitHub Dark, One Dark Pro, …) may render Lux source a bit flatter than
  a language with its own semantic provider, since such a theme's TextMate rules for
  unrecognized languages are usually a generic fallback. That is accepted rather than fixed
  by writing into a setting shared with every other language the user has open.
- **"Lux Dark" — an optional color theme**, not a default. It is a full copy of VS Code's
  own built-in "Default Dark Modern" (same UI colors, same base + override token colors —
  `dark_vs.json` and `dark_plus.json`, both copied in, not just the smaller override layer)
  with three additions: `await` (its own scope, `keyword.control.flow.await.lux`) in green;
  module namespaces (`sqlite.`, `hash.`, …, `support.class.lux`) in a darker green; and
  primitive types (`int`/`string`/`bool`/…, `storage.type.primitive.lux`) recolored to match
  `Json`/`List`/`File`/class names instead of Dark+'s plain blue `storage.type`, so every
  spelling of "this is a type" reads as one family. Nothing installs or activates it
  automatically — pick it from `Preferences: Color Theme` if you want those; every other
  theme keeps coloring Lux through its own rules as described above.

## How diagnostics actually work

A Lux "project" is every `.lux` file in a directory compiled together as one program —
there is no per-file `import` for local files, so checking a single open file in isolation
produces false positives for anything defined in another file (confirmed while building
this: a route taking a class parameter defined in a sibling file reports `unknown type`
when checked alone, and reports nothing when the whole project directory is checked
instead). The server therefore always runs `lux <workspace-root> --json`, not
`lux <this-file> --json` — diagnostics for every open file in the workspace come from one
subprocess call.

Diagnostics refresh **on open and on save**, not on every keystroke: `compile()`
(`project.cpp`) reads files from disk, so an unsaved edit is invisible to it. True
as-you-type diagnostics would need an overlay parameter threaded through `compile()` (map a
path to in-memory content instead of reading it from disk) — a small, additive change, left
for a later phase; see "What is not here yet".

## Setup

The extension has two `npm` projects: the client (this directory) and the server
(`server/`). Both need `npm install` once, and the root `compile` script builds both:

```sh
cd editors/vscode-lux
npm install
cd server && npm install && cd ..
npm run compile
```

## Try it (Extension Development Host)

1. Open `editors/vscode-lux` in VS Code (as its own folder, not as part of the whole
   Lux repo — the extension's `package.json` needs to be the workspace root for `F5` to
   find it).
2. Press `F5`. A new VS Code window opens with the extension loaded (the
   `preLaunchTask` in `.vscode/launch.json` builds both projects first).
3. Open a `.lux` file. No setup needed: the server auto-detects the compiler, trying
   `<workspace>/build/lux` first (this repo's own CMake output — the common case when
   developing inside it) and then `lux` on `PATH`. Set `lux.compilerPath` only if neither
   applies.
4. Open a folder containing `.lux` files (not a loose file) so the server has a workspace
   root to check — see "How diagnostics actually work" above.

## Install locally without publishing

```sh
npm install -g @vscode/vsce
cd editors/vscode-lux
vsce package
code --install-extension lux-script-0.3.0.vsix
```

## Verifying it works, without opening VS Code

Both pieces were checked directly rather than eyeballed in the editor:

- **Grammar**: tokenized every `.lux` in `tests/cases/` and `bench/lux-native/app.lux`
  through `vscode-textmate` + `vscode-oniguruma` — no exceptions, multi-line strings close
  correctly.
- **Server**: spoke real LSP JSON-RPC (`Content-Length`-framed, over stdio) against the
  compiled `server/out/server.js` — `initialize`, `initialized` triggering a project-wide
  check with a genuine unbound-route-parameter error, fixing the file and confirming the
  diagnostic clears on `didSave`, and `textDocument/completion` returning the full list
  including `endpoint`, `state` and the route snippet. All of it against the real
  `build/lux` binary, not a mock.

Neither script is part of the extension; both were throwaway verification.

## What is not here yet

- **Live, as-you-type diagnostics.** Currently on open/save only (see above) — the
  `compile()` overlay this needs does not exist yet.
- **Symbol-aware completion.** No knowledge of a file's own classes, fields, functions or
  local variables; no member completion after `.` beyond the static reserved-object/builtin
  list.
- **Go-to-definition, hover, rename.** None implemented. The compiler's own checker
  (`emitter.cpp`) already resolves every name to a definite declaration site at compile
  time — exposing that resolution (instead of just the pass/fail diagnostics it produces
  today) is what each of these would build on.
