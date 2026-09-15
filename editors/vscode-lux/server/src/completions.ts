// Static completion list for Lux Script -- context-free by design (v1):
// VS Code's own fuzzy filter narrows this as the user types. It is not
// symbol-aware (no knowledge of a given file's classes/functions/locals) --
// that needs the compiler to expose its symbol table, a later phase. Every
// item here comes straight from LUX_SCRIPT-GRAMMAR.md (keywords, §2) and
// GUIDE.md (§20, builtin reference) -- nothing invented.
import { CompletionItem, CompletionItemKind, InsertTextFormat } from 'vscode-languageserver/node';

function kw(label: string): CompletionItem {
  return { label, kind: CompletionItemKind.Keyword };
}
function type(label: string): CompletionItem {
  return { label, kind: CompletionItemKind.Class };
}
function variable(label: string, detail: string): CompletionItem {
  return { label, kind: CompletionItemKind.Variable, detail };
}
function fn(label: string, insertText: string, detail: string): CompletionItem {
  return {
    label,
    kind: CompletionItemKind.Function,
    insertText,
    insertTextFormat: InsertTextFormat.Snippet,
    detail,
  };
}
function method(label: string, insertText: string, detail: string): CompletionItem {
  return {
    label,
    kind: CompletionItemKind.Method,
    insertText,
    insertTextFormat: InsertTextFormat.Snippet,
    detail,
  };
}
function snippet(label: string, insertText: string, detail: string): CompletionItem {
  return {
    label,
    kind: CompletionItemKind.Snippet,
    insertText,
    insertTextFormat: InsertTextFormat.Snippet,
    detail,
  };
}

const KEYWORDS = [
  'import', 'class', 'fn', 'app', 'group', 'endpoint', 'on', 'error', 'origins',
  'get', 'post', 'put', 'patch', 'delete', 'any', 'sse', 'ws',
  'if', 'else', 'elif', 'while', 'for', 'in', 'return', 'require', 'try', 'catch',
  'break', 'continue', 'validate', 'and', 'or', 'not', 'true', 'false', 'null',
  'this', 'void', 'spa',
].map(kw);

const TYPES = [
  'int', 'long', 'float', 'double', 'bool', 'string', 'void',
  'Json', 'List', 'Dict', 'File',
].map(type);

const RESERVED_OBJECTS = [
  variable('request', 'request.path / .method / .ip'),
  variable('session', 'session.<field>, session.clear()'),
  variable('jwt', 'jwt.valid / .claims'),
  variable('state', 'state.incr/decr/get/set/remove -- shared in-memory state'),
  variable('log', 'log.info/warn/error'),
  variable('sse', 'sse.send/ping/open -- inside sse routes'),
  variable('ws', 'ws.send/recv/open/close -- inside ws routes'),
  variable('error', 'error.code / .message / .messages -- inside on error blocks'),
  variable('sqlite', 'sqlite.query/exec/begin/commit/rollback/last_id (await)'),
  variable('postgres', 'postgres.query/exec/begin/commit/rollback (await)'),
  variable('mysql', 'mysql.query/exec/begin/commit/rollback/last_id (await)'),
];

const FUNCTIONS = [
  fn('text', 'text(${1:value})', 'text(v) -- writes a plain-text response'),
  fn('html', 'html(${1:value})', 'html(v) -- writes an HTML response'),
  fn('json', 'json(${1:value})', 'json(v) -- writes a JSON response'),
  fn('render', 'render(${1:"template.html"}, ${2:key}=${3:value})', 'render(template, k=v, ...) -- renders a Lux Script template'),
  fn('status', 'status(${1:code})', 'status(code)'),
  fn('redirect', 'redirect(${1:target})', 'redirect(target[, code])'),
  fn('send_file', 'send_file(${1:path})', 'send_file(path)'),
  fn('len', 'len(${1:value})', 'len(v) -- size of a string, List or Dict'),
  fn('str', 'str(${1:value})', 'str(v) -- explicit conversion'),
  fn('int', 'int(${1:value})', 'int(v) -- explicit conversion'),
  fn('header', 'header(${1:"name"})', 'header(name[, default]) -- request header'),
  fn('query', 'query(${1:"name"})', 'query(name[, default]) -- query parameter'),
  fn('cookie', 'cookie(${1:"name"})', 'cookie(name[, default]) -- request cookie'),
  fn('form', 'form(${1:"name"})', 'form(name[, default]) -- urlencoded form field'),
  fn('sleep', 'sleep(${1:ms})', 'await sleep(ms) -- suspends the handler'),
];

const METHODS = [
  method('status', 'status(${1:code})', 'any -- chainable, sets the response status'),
  method('header', 'header(${1:"name"}, ${2:"value"})', 'any -- chainable, sets a response header'),
  method('cookie', 'cookie(${1:"name"}, ${2:"value"})', 'any -- chainable, sets a response cookie'),
  method('starts_with', 'starts_with(${1:"prefix"})', 'string'),
  method('ends_with', 'ends_with(${1:"suffix"})', 'string'),
  method('contains', 'contains(${1:"needle"})', 'string'),
  method('upper', 'upper()', 'string'),
  method('lower', 'lower()', 'string'),
  method('trim', 'trim()', 'string'),
  method('add', 'add(${1:value})', 'List'),
  method('has', 'has(${1:"key"})', 'Dict'),
  method('keys', 'keys()', 'Dict'),
  method('save', 'save(${1:"./uploads"})', 'File'),
  method('incr', 'incr(${1:"key"})', 'state -- atomic increment'),
  method('decr', 'decr(${1:"key"})', 'state -- atomic decrement'),
  method('get', 'get(${1:"key"}, ${2:default})', 'state'),
  method('set', 'set(${1:"key"}, ${2:value})', 'state'),
  method('remove', 'remove(${1:"key"})', 'state'),
];

const SNIPPETS = [
  snippet(
    'get endpoint',
    'get endpoint("${1:/path}"):\n    return ${0:{ }}',
    'GET route',
  ),
  snippet(
    'post endpoint',
    'post endpoint("${1:/path}", ${2:Type} ${3:body}):\n    ${0}',
    'POST route with a JSON body parameter',
  ),
  snippet(
    'class with validate',
    'class ${1:Name}:\n    ${2:string} ${3:field}\n\n    validate:\n        ${0:${3:field} != ""}   "${3:field}: required"',
    'class declaration with a validate: block',
  ),
  snippet(
    'group',
    'group("${1:/prefix}"):\n    require ${2:condition} else status(${3:401})\n\n    ${0}',
    'group with a guard',
  ),
  snippet(
    'on error',
    'on error ${1:404}:\n    return ${0:status(${1:404})}',
    'error handler for one status code',
  ),
  snippet(
    'require else',
    'require ${1:condition} else ${0:status(400)}',
    'require ... else -- the only guard/middleware construct',
  ),
  snippet(
    'try/catch',
    'try:\n    ${1}\ncatch ${2:e}:\n    ${0}',
    'try/catch',
  ),
  snippet(
    'for in',
    'for ${1:item} in ${2:items}:\n    ${0}',
    'for .. in loop',
  ),
];

export const ALL_COMPLETIONS: CompletionItem[] = [
  ...KEYWORDS,
  ...TYPES,
  ...RESERVED_OBJECTS,
  ...FUNCTIONS,
  ...METHODS,
  ...SNIPPETS,
];
