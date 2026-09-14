# Lumen Script — VS Code extension

Syntax highlighting, live diagnostics and completion for `.lum` files. See
`LUMEN_SCRIPT-GRAMMAR.md` for the language this follows.

## What it provides

- **Syntax highlighting** — comments, strings (single and triple-quoted, including
  multi-line), numbers, keywords, primitives, native types (`Json`/`List`/`Dict`/`File`),
  reserved objects, function/method calls, operators. TextMate grammar
  (`syntaxes/lumen.tmLanguage.json`), verified by tokenizing every `.lum` file in
  `tests/cases/` and `bench/` with `vscode-textmate`/`vscode-oniguruma` (the same engine VS
  Code uses) — zero exceptions.
- **Diagnostics** — real compiler errors, not reimplemented: the language server
  (`server/`) invokes the actual `lumen` binary with `--json` (see
  `src/lumen_script/main.cpp`) and turns its output straight into LSP diagnostics. There is
  no separate, second implementation of Lumen's checking rules to keep in sync — the
  server is a thin adapter around the same compiler that ships the app.
- **Completion** — a static, context-free list (keywords, primitives, native types,
  reserved objects, builtins, and a few snippets for route/class/group/`on error`
  skeletons), from `LUMEN_SCRIPT-GRAMMAR.md` §2 and `GUIDE.md` §20. Not yet aware of a
  given file's own classes, functions or locals — see "What is not here yet" below.
- **Editor configuration** — `#` comments, bracket/quote auto-closing, indentation rules
  for Lumen's Python-style indented blocks.

## How diagnostics actually work

A Lumen "project" is every `.lum` file in a directory compiled together as one program —
there is no per-file `import` for local files, so checking a single open file in isolation
produces false positives for anything defined in another file (confirmed while building
this: a route taking a class parameter defined in a sibling file reports `unknown type`
when checked alone, and reports nothing when the whole project directory is checked
instead). The server therefore always runs `lumen <workspace-root> --json`, not
`lumen <this-file> --json` — diagnostics for every open file in the workspace come from one
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
cd editors/vscode-lumen
npm install
cd server && npm install && cd ..
npm run compile
```

## Try it (Extension Development Host)

1. Open `editors/vscode-lumen` in VS Code (as its own folder, not as part of the whole
   Lumen repo — the extension's `package.json` needs to be the workspace root for `F5` to
   find it).
2. Press `F5`. A new VS Code window opens with the extension loaded (the
   `preLaunchTask` in `.vscode/launch.json` builds both projects first).
3. Open a `.lum` file. By default the server looks for a `lumen` binary on `PATH`; during
   development, set `lumen.compilerPath` in that window's settings to your build, e.g.
   `/path/to/LoHin/build/lumen`.
4. Open a folder containing `.lum` files (not a loose file) so the server has a workspace
   root to check — see "How diagnostics actually work" above.

## Install locally without publishing

```sh
npm install -g @vscode/vsce
cd editors/vscode-lumen
vsce package
code --install-extension lumen-script-0.2.0.vsix
```

## Verifying it works, without opening VS Code

Both pieces were checked directly rather than eyeballed in the editor:

- **Grammar**: tokenized every `.lum` in `tests/cases/` and `bench/lumen-native/app.lum`
  through `vscode-textmate` + `vscode-oniguruma` — no exceptions, multi-line strings close
  correctly.
- **Server**: spoke real LSP JSON-RPC (`Content-Length`-framed, over stdio) against the
  compiled `server/out/server.js` — `initialize`, `initialized` triggering a project-wide
  check with a genuine unbound-route-parameter error, fixing the file and confirming the
  diagnostic clears on `didSave`, and `textDocument/completion` returning the full list
  including `endpoint`, `state` and the route snippet. All of it against the real
  `build/lumen` binary, not a mock.

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
