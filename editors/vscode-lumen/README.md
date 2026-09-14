# Lumen Script — VS Code extension

Syntax highlighting and editor configuration for `.lum` files. This is phase 1 of editor
tooling for Lumen Script (see `LUMEN_SCRIPT-GRAMMAR.md` for the language this grammar
follows) — a TextMate grammar plus indentation/bracket rules, no language server yet.

## What it provides

- Syntax highlighting: comments, strings (single and triple-quoted, including multi-line),
  numbers, keywords (control flow, declarations, route methods), the four primitives and the
  native types (`Json`/`List`/`Dict`/`File`), reserved objects (`request`, `session`, `state`,
  `jwt`, `log`, `this`, and `sse`/`ws`/`error` when used as a value rather than as a keyword),
  function/method calls, and operators.
- Language configuration: `#` line comments, bracket matching and auto-closing pairs
  (including `"""`), and indentation rules tuned to Lumen's Python-style indented blocks
  (`if`/`class`/`fn`/`group`/route declarations/etc. ending in `:` increase the indent level).

## Try it (Extension Development Host)

1. Open this directory (`editors/vscode-lumen`) in VS Code.
2. Press `F5` (or Run → Start Debugging). A new VS Code window opens with the extension
   loaded.
3. Open any `.lum` file (e.g. `example/app.lum` or `bench/lumen-native/app.lum`) in that
   window.

## Install locally without publishing

```sh
npm install -g @vscode/vsce
cd editors/vscode-lumen
vsce package
code --install-extension lumen-script-0.1.0.vsix
```

## Verifying the grammar without VS Code

The grammar was checked against every `.lum` file in `tests/cases/` and `bench/` using
`vscode-textmate` + `vscode-oniguruma` directly (the same tokenizer engine VS Code uses) —
tokenizes cleanly, no exceptions, multi-line triple-quoted strings close correctly. That
script was a throwaway (not part of this extension); to redo it, load
`syntaxes/lumen.tmLanguage.json` through `vscode-textmate`'s `Registry` and call
`tokenizeLine` line by line, carrying the returned `ruleStack` forward for multi-line
constructs.

## What is not here yet

No language server: no autocomplete, no live diagnostics, no go-to-definition, no hover.
Lumen's own compiler frontend (`emitter.cpp`, `diagnostic.cpp`) already produces precise
file:line:column errors — wiring those into an LSP server is the natural next phase, and could
reuse that frontend directly instead of re-implementing the language's checking rules in
TypeScript.
