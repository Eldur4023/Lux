// Lumen Script language server.
//
// Diagnostics are NOT reimplemented here: every check is the real `lumen`
// compiler (src/lumen_script/main.cpp, `--json`), invoked as a subprocess and
// its stdout (a JSON array of {file,line,col,message}) turned into LSP
// Diagnostics. That mirrors the project's own rule for --native (see
// COMPILACION-NATIVA.md, §3, "el invariante innegociable"): never let a
// second implementation of the language's checking rules drift from the
// first. Completion (see completions.ts) is the one part that is NOT
// compiler-backed yet -- it is a static, context-free list, not aware of a
// given file's classes/functions/locals.
import {
  createConnection,
  ProposedFeatures,
  InitializeParams,
  TextDocumentSyncKind,
  InitializeResult,
  Diagnostic,
  DiagnosticSeverity,
  CompletionItem,
  TextDocuments,
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { URI } from 'vscode-uri';
import { execFile } from 'child_process';
import * as fs from 'fs';
import { ALL_COMPLETIONS } from './completions';

// Explicit stdio, not the IPC-channel auto-detection: keeps the server
// launchable (and testable) as a plain child process, not only forked by
// Node -- see server/README-ish note in extension.ts's ServerOptions.
const connection = createConnection(ProposedFeatures.all, process.stdin, process.stdout);
const documents = new TextDocuments(TextDocument);

let workspaceRoot: string | undefined;
let compilerPath = 'lumen';
// URIs we last sent diagnostics for -- needed to clear a file's squiggles
// when a later check comes back clean for it (an empty entry never arrives
// on its own; the file just stops appearing in the JSON array).
let lastDiagnosedUris = new Set<string>();

connection.onInitialize((params: InitializeParams): InitializeResult => {
  const folders = params.workspaceFolders;
  if (folders && folders.length > 0) {
    workspaceRoot = URI.parse(folders[0].uri).fsPath;
  } else if (params.rootUri) {
    workspaceRoot = URI.parse(params.rootUri).fsPath;
  }
  const opts = params.initializationOptions as { compilerPath?: string } | undefined;
  if (opts?.compilerPath) compilerPath = opts.compilerPath;

  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Incremental,
      completionProvider: { resolveProvider: false, triggerCharacters: ['.'] },
    },
  };
});

connection.onInitialized(() => {
  // One check on startup, over the whole project: a file opened mid-session
  // still gets diagnostics for errors that live in a DIFFERENT file (a
  // Lumen "project" is every .lum in the directory compiled together --
  // LUMEN_SCRIPT-GRAMMAR.md §12 -- so checking one file in isolation would
  // misreport cross-file references as undeclared).
  if (workspaceRoot) runCheck(workspaceRoot);
});

connection.onDidChangeConfiguration((change) => {
  const settings = change.settings as { lumen?: { compilerPath?: string } } | undefined;
  if (settings?.lumen?.compilerPath) compilerPath = settings.lumen.compilerPath;
});

// Diagnostics trigger on open and on save -- not on every keystroke. The
// compiler reads files from disk (project.cpp's compile()), so unsaved
// edits are invisible to it; wiring true as-you-type diagnostics would need
// an overlay parameter threaded through compile() (a small, additive change,
// left for a later phase -- see editors/vscode-lumen/README.md).
documents.onDidOpen((e) => runCheckFor(e.document));
documents.onDidSave((e) => runCheckFor(e.document));

function runCheckFor(doc: TextDocument) {
  const root = workspaceRoot ?? URI.parse(doc.uri).fsPath;
  runCheck(root);
}

function runCheck(root: string) {
  if (!fs.existsSync(root)) return;
  execFile(compilerPath, [root, '--json'], { maxBuffer: 16 * 1024 * 1024 }, (_err, stdout) => {
    // A non-zero exit is expected whenever there ARE diagnostics (see
    // main.cpp: --json returns 1 with errors, 0 clean) -- `_err` alone does
    // not mean the invocation failed, so it is deliberately ignored here and
    // stdout is parsed regardless. A JSON parse failure (compiler missing,
    // wrong path, a real crash) is the only case worth surfacing.
    let items: Array<{ file: string; line: number; col: number; message: string }>;
    try {
      items = JSON.parse(stdout || '[]');
    } catch {
      connection.window.showErrorMessage(
        `lumen (${compilerPath}) did not return valid JSON -- check the "lumen.compilerPath" setting.`,
      );
      return;
    }
    publish(items);
  });
}

function publish(items: Array<{ file: string; line: number; col: number; message: string }>) {
  const byUri = new Map<string, Diagnostic[]>();
  for (const it of items) {
    if (!it.file) continue; // a diagnostic with no location (e.g. resolve_inputs failure)
    const uri = URI.file(it.file).toString();
    const line = Math.max(0, it.line - 1);
    // Column is byte-indexed 1-based in the compiler (SourceLoc, token.hpp);
    // LSP wants a 0-based UTF-16 offset. Close enough for ASCII source, off
    // by a byte or two on a line with multi-byte identifiers -- a known,
    // documented limitation (README.md), not solved here.
    const col = Math.max(0, it.col - 1);
    const diag: Diagnostic = {
      severity: DiagnosticSeverity.Error,
      range: { start: { line, character: col }, end: { line, character: col + 1 } },
      message: it.message,
      source: 'lumen',
    };
    const list = byUri.get(uri) ?? [];
    list.push(diag);
    byUri.set(uri, list);
  }

  const nextUris = new Set(byUri.keys());
  for (const uri of lastDiagnosedUris) {
    if (!nextUris.has(uri)) connection.sendDiagnostics({ uri, diagnostics: [] });
  }
  for (const [uri, diagnostics] of byUri) {
    connection.sendDiagnostics({ uri, diagnostics });
  }
  lastDiagnosedUris = nextUris;
}

connection.onCompletion((): CompletionItem[] => ALL_COMPLETIONS);

documents.listen(connection);
connection.listen();
