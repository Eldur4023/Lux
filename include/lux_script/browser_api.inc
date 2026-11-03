// The browser's LuxScript API, as data -- written ONCE, used by two hosts:
//
//   * Lux compiles page scripts against it (the "browser profile",
//     natives.hpp: BrowserProfile) when it translates a page to .luxp;
//   * Luxium builds its real native tables from it, pairing each entry with
//     the implementation named in the last column.
//
// A page reaches nothing that is not listed here: no fs, no net, no db, no
// session.  Adding a capability = one line here + its implementation in
// Luxium; the .luxp verifier rejects any native it does not know by name.
//
// Before including, define the macros you need (undefined ones are skipped):
//   LUX_BROWSER_NATIVE(name, min_args, max_args, impl, is_async)
//   LUX_BROWSER_MEMBER(object, member, native)       object.member -> native
//   LUX_BROWSER_METHOD_X(type, name, min_args, max_args, return_type or nullptr,
//                        async_native or nullptr)
// or, for the common shapes, the shorthands this file defines on top of them:
//   LUX_BROWSER_CALL(object, member, min, max, async)   a native + its member;
//       the native is "__object_member" and its implementation is the host's
//       `host_object_member` (Luxium dispatches these by name through
//       Host::call_native -- see natives.hpp)
//   LUX_BROWSER_METHOD(type, name, min, max, ret)       a plain method
//   LUX_BROWSER_AMETHOD(type, name, min, max, ret, native)
//       an ASYNC method (`await sock.recv()`): the checker demands `await` and
//       lowers the call to the async native `native`, receiver first

#ifndef LUX_BROWSER_NATIVE
#define LUX_BROWSER_NATIVE(name, min, max, impl, is_async)
#endif
#ifndef LUX_BROWSER_MEMBER
#define LUX_BROWSER_MEMBER(object, member, native)
#endif
#ifndef LUX_BROWSER_METHOD_X
#define LUX_BROWSER_METHOD_X(type, name, min, max, ret, async_native)
#endif
#ifndef LUX_BROWSER_CALL
#define LUX_BROWSER_CALL(obj, member, min, max, is_async) \
    LUX_BROWSER_NATIVE("__" #obj "_" #member, min, max, host_##obj##_##member, is_async) \
    LUX_BROWSER_MEMBER(#obj, #member, "__" #obj "_" #member)
#define LUX_BROWSER_CALL_DEFAULTED
#endif
#define LUX_BROWSER_METHOD(type, name, min, max, ret) \
    LUX_BROWSER_METHOD_X(type, name, min, max, ret, nullptr)
#define LUX_BROWSER_AMETHOD(type, name, min, max, ret, native) \
    LUX_BROWSER_METHOD_X(type, name, min, max, ret, native)

// ── natives (stdlib, same semantics as the server's) ────────────────────────
LUX_BROWSER_NATIVE("len",   1, 1, fn_len,   0)
LUX_BROWSER_NATIVE("str",   1, 1, fn_str,   0)
LUX_BROWSER_NATIVE("int",   1, 1, fn_int,   0)
LUX_BROWSER_NATIVE("float", 1, 1, fn_float, 0)
LUX_BROWSER_NATIVE("range", 1, 3, fn_range, 0)
LUX_BROWSER_NATIVE("sleep", 1, 1, host_sleep, 1)          // await sleep(ms)
LUX_BROWSER_NATIVE("form_data", 0, 1, host_form_data, 0)  // Form; form_data(formElement) fills it
// Async natives behind async METHODS (the checker lowers `await x.m()` to them).
LUX_BROWSER_NATIVE("__socket_recv",      1, 1, host_socket_recv,      1)
LUX_BROWSER_NATIVE("__socket_wait_open", 1, 1, host_socket_wait_open, 1)

// ── reserved objects ────────────────────────────────────────────────────────
// log.* -- the page's console pane
LUX_BROWSER_CALL(log, info,  1, 1, 0)
LUX_BROWSER_CALL(log, warn,  1, 1, 0)
LUX_BROWSER_CALL(log, error, 1, 1, 0)
// document.*
LUX_BROWSER_CALL(document, get,              1, 1, 0)
LUX_BROWSER_CALL(document, query,            1, 1, 0)
LUX_BROWSER_CALL(document, query_all,        1, 1, 0)
LUX_BROWSER_CALL(document, title,            0, 0, 0)
LUX_BROWSER_CALL(document, root,             0, 0, 0)
LUX_BROWSER_CALL(document, body,             0, 0, 0)
LUX_BROWSER_CALL(document, create,           1, 1, 0)
LUX_BROWSER_CALL(document, listen,           2, 3, 0)
LUX_BROWSER_CALL(document, unlisten,         2, 2, 0)
LUX_BROWSER_CALL(document, fullscreen,       1, 1, 0)
LUX_BROWSER_CALL(document, exit_fullscreen,  0, 0, 0)
LUX_BROWSER_CALL(document, fullscreen_element, 0, 0, 0)
LUX_BROWSER_CALL(document, active_element,   0, 0, 0)
LUX_BROWSER_CALL(document, hidden,           0, 0, 0)
LUX_BROWSER_CALL(document, element_from_point, 2, 2, 0)   // (x, y) in page coordinates; null if none
// page.* -- store, timers, location, window-level events
LUX_BROWSER_CALL(page, get,         1, 1, 0)
LUX_BROWSER_CALL(page, set,         2, 2, 0)
LUX_BROWSER_CALL(page, after,       2, 2, 0)
LUX_BROWSER_CALL(page, every,       2, 2, 0)
LUX_BROWSER_CALL(page, cancel,      1, 1, 0)
LUX_BROWSER_CALL(page, clicked,     0, 0, 0)
LUX_BROWSER_CALL(page, frame,       0, 0, 1)          // await page.frame()
LUX_BROWSER_CALL(page, now,         0, 0, 0)          // monotonic ms (performance.now)
LUX_BROWSER_CALL(page, location,    0, 0, 0)
LUX_BROWSER_CALL(page, on_navigate, 1, 1, 0)
LUX_BROWSER_CALL(page, listen,      2, 2, 0)          // resize, visibility, fullscreenchange
LUX_BROWSER_CALL(page, title,       0, 1, 0)          // title() reads, title(s) sets
LUX_BROWSER_CALL(page, scroll_y,    0, 0, 0)
LUX_BROWSER_CALL(page, scroll_to,   1, 1, 0)
LUX_BROWSER_CALL(page, reload,      0, 0, 0)
// event.* -- the event being dispatched (null/false outside a handler)
LUX_BROWSER_CALL(event, kind,       0, 0, 0)
LUX_BROWSER_CALL(event, target,     0, 0, 0)
LUX_BROWSER_CALL(event, current,    0, 0, 0)
LUX_BROWSER_CALL(event, key,        0, 0, 0)
LUX_BROWSER_CALL(event, code,       0, 0, 0)
LUX_BROWSER_CALL(event, value,      0, 0, 0)
LUX_BROWSER_CALL(event, button,     0, 0, 0)
LUX_BROWSER_CALL(event, x,          0, 0, 0)
LUX_BROWSER_CALL(event, y,          0, 0, 0)
LUX_BROWSER_CALL(event, delta,      0, 0, 0)
LUX_BROWSER_CALL(event, ctrl,       0, 0, 0)
LUX_BROWSER_CALL(event, shift,      0, 0, 0)
LUX_BROWSER_CALL(event, alt,        0, 0, 0)
LUX_BROWSER_CALL(event, meta,       0, 0, 0)
LUX_BROWSER_CALL(event, repeat,     0, 0, 0)
LUX_BROWSER_CALL(event, prevent,    0, 0, 0)
LUX_BROWSER_CALL(event, stop,       0, 0, 0)
LUX_BROWSER_CALL(event, prevented,  0, 0, 0)
LUX_BROWSER_CALL(event, message,    0, 0, 0)          // a websocket text message
LUX_BROWSER_CALL(event, close_code, 0, 0, 0)
// http.* -- same origin only; url relative to the page.  get/delete(url,
// headers?), post/put/patch(url, body, headers?)
LUX_BROWSER_CALL(http, get,    1, 2, 1)
LUX_BROWSER_CALL(http, delete, 1, 2, 1)
LUX_BROWSER_CALL(http, post,   2, 3, 1)
LUX_BROWSER_CALL(http, put,    2, 3, 1)
LUX_BROWSER_CALL(http, patch,  2, 3, 1)
// json / ws / storage / history / clipboard / template / url / math / time
LUX_BROWSER_CALL(json, parse,     1, 1, 0)
LUX_BROWSER_CALL(json, stringify, 1, 1, 0)
LUX_BROWSER_CALL(ws, connect, 1, 1, 0)
LUX_BROWSER_CALL(storage, get,    1, 1, 0)
LUX_BROWSER_CALL(storage, set,    2, 2, 0)
LUX_BROWSER_CALL(storage, remove, 1, 1, 0)
LUX_BROWSER_CALL(storage, keys,   0, 0, 0)
LUX_BROWSER_CALL(storage, clear,  0, 0, 0)
LUX_BROWSER_CALL(history, push,    1, 1, 0)
LUX_BROWSER_CALL(history, replace, 1, 1, 0)
LUX_BROWSER_CALL(history, back,    0, 0, 0)
LUX_BROWSER_CALL(history, forward, 0, 0, 0)
LUX_BROWSER_CALL(clipboard, write, 1, 1, 0)
LUX_BROWSER_CALL(clipboard, read,  0, 0, 0)
LUX_BROWSER_CALL(template, render, 1, 2, 0)           // Lux's template engine over a string
LUX_BROWSER_CALL(template, escape, 1, 1, 0)           // HTML-escape
LUX_BROWSER_CALL(url, encode,      1, 1, 0)
LUX_BROWSER_CALL(url, decode,      1, 1, 0)
LUX_BROWSER_CALL(url, parse_query, 1, 1, 0)
LUX_BROWSER_CALL(math, floor, 1, 1, 0)
LUX_BROWSER_CALL(math, ceil,  1, 1, 0)
LUX_BROWSER_CALL(math, round, 1, 1, 0)
LUX_BROWSER_CALL(math, abs,   1, 1, 0)
LUX_BROWSER_CALL(math, min,   2, 2, 0)
LUX_BROWSER_CALL(math, max,   2, 2, 0)
LUX_BROWSER_CALL(math, fixed, 2, 2, 0)                // fixed(x, digits) -> string, like toFixed
LUX_BROWSER_CALL(time, now,   0, 0, 0)                // wall clock, epoch ms

// ── methods by receiver type ────────────────────────────────────────────────
// Element: DOM
LUX_BROWSER_METHOD("Element", "text",        0, 0, "string")
LUX_BROWSER_METHOD("Element", "set_text",    1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "append",      1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "prepend",     1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "remove",      0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "attr",        1, 1, "string")
LUX_BROWSER_METHOD("Element", "set_attr",    2, 2, nullptr)
LUX_BROWSER_METHOD("Element", "remove_attr", 1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "has_attr",    1, 1, "bool")
LUX_BROWSER_METHOD("Element", "tag",         0, 0, "string")
LUX_BROWSER_METHOD("Element", "id",          0, 0, "string")
LUX_BROWSER_METHOD("Element", "child_count", 0, 0, "int")
LUX_BROWSER_METHOD("Element", "query",       1, 1, "Element")
LUX_BROWSER_METHOD("Element", "query_all",   1, 1, "List")
LUX_BROWSER_METHOD("Element", "closest",     1, 1, "Element")
LUX_BROWSER_METHOD("Element", "matches",     1, 1, "bool")
LUX_BROWSER_METHOD("Element", "parent",      0, 0, "Element")
LUX_BROWSER_METHOD("Element", "children",    0, 0, "List")
LUX_BROWSER_METHOD("Element", "next",        0, 0, "Element")
LUX_BROWSER_METHOD("Element", "prev",        0, 0, "Element")
LUX_BROWSER_METHOD("Element", "contains",    1, 1, "bool")
LUX_BROWSER_METHOD("Element", "connected",   0, 0, "bool")
LUX_BROWSER_METHOD("Element", "html",        0, 0, "string")
LUX_BROWSER_METHOD("Element", "set_html",    1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "insert_before", 2, 2, nullptr)
LUX_BROWSER_METHOD("Element", "replace_with", 1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "clone",       0, 1, "Element")
// Element: classes, style, data, state
LUX_BROWSER_METHOD("Element", "add_class",    1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "remove_class", 1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "toggle_class", 1, 2, "bool")
LUX_BROWSER_METHOD("Element", "has_class",    1, 1, "bool")
LUX_BROWSER_METHOD("Element", "set_style",    2, 2, nullptr)
LUX_BROWSER_METHOD("Element", "style",        1, 1, "string")
LUX_BROWSER_METHOD("Element", "data",         1, 1, "string")
LUX_BROWSER_METHOD("Element", "set_data",     2, 2, nullptr)
LUX_BROWSER_METHOD("Element", "hidden",       0, 0, "bool")
LUX_BROWSER_METHOD("Element", "set_hidden",   1, 1, nullptr)
// Element: forms
LUX_BROWSER_METHOD("Element", "value",         0, 0, "string")
LUX_BROWSER_METHOD("Element", "set_value",     1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "selected_text", 0, 0, "string")
LUX_BROWSER_METHOD("Element", "checked",       0, 0, "bool")
LUX_BROWSER_METHOD("Element", "set_checked",   1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "disabled",      0, 0, "bool")
LUX_BROWSER_METHOD("Element", "set_disabled",  1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "files",         0, 0, "List")
LUX_BROWSER_METHOD("Element", "show_modal",    0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "close",         0, 1, nullptr)
LUX_BROWSER_METHOD("Element", "return_value",  0, 0, "string")
// Element: events, focus, geometry
LUX_BROWSER_METHOD("Element", "click",       1, 1, nullptr)   // listen("click", fn)
LUX_BROWSER_METHOD("Element", "listen",      2, 3, nullptr)
LUX_BROWSER_METHOD("Element", "unlisten",    2, 2, nullptr)
LUX_BROWSER_METHOD("Element", "dispatch",    1, 1, nullptr)   // synthetic event, e.g. "change"
LUX_BROWSER_METHOD("Element", "focus",       0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "blur",        0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "rect",        0, 0, "Dict")
LUX_BROWSER_METHOD("Element", "scroll_into_view", 0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "scroll_top",     0, 0, "float")
LUX_BROWSER_METHOD("Element", "set_scroll_top", 1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "scroll_left",    0, 0, "float")
LUX_BROWSER_METHOD("Element", "set_scroll_left", 1, 1, nullptr)
// Element: <video>/<audio>
LUX_BROWSER_METHOD("Element", "play",          0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "pause",         0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "seek",          1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "current_time",  0, 0, "float")
LUX_BROWSER_METHOD("Element", "duration",      0, 0, "float")
LUX_BROWSER_METHOD("Element", "paused",        0, 0, "bool")
LUX_BROWSER_METHOD("Element", "ended",         0, 0, "bool")
LUX_BROWSER_METHOD("Element", "seeking",       0, 0, "bool")
LUX_BROWSER_METHOD("Element", "set_volume",    1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "volume",        0, 0, "float")
LUX_BROWSER_METHOD("Element", "set_muted",     1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "muted",         0, 0, "bool")
LUX_BROWSER_METHOD("Element", "audio_tracks",  0, 0, "List")
LUX_BROWSER_METHOD("Element", "select_audio",  1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "text_tracks",   0, 0, "List")
LUX_BROWSER_METHOD("Element", "select_text",   1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "cue",           0, 0, "string")
LUX_BROWSER_METHOD("Element", "set_src",       1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "src",           0, 0, "string")
// Response (http.*)
LUX_BROWSER_METHOD("Response", "status", 0, 0, "int")
LUX_BROWSER_METHOD("Response", "ok",     0, 0, "bool")
LUX_BROWSER_METHOD("Response", "text",   0, 0, "string")
LUX_BROWSER_METHOD("Response", "json",   0, 0, "Json")
LUX_BROWSER_METHOD("Response", "header", 1, 1, "string")
LUX_BROWSER_METHOD("Response", "url",    0, 0, "string")
// Socket (ws.connect)
LUX_BROWSER_METHOD("Socket", "send",       1, 1, nullptr)
LUX_BROWSER_METHOD("Socket", "close",      0, 1, nullptr)
LUX_BROWSER_METHOD("Socket", "is_open",    0, 0, "bool")
LUX_BROWSER_METHOD("Socket", "on_open",    1, 1, nullptr)
LUX_BROWSER_METHOD("Socket", "on_message", 1, 1, nullptr)
LUX_BROWSER_METHOD("Socket", "on_close",   1, 1, nullptr)
LUX_BROWSER_AMETHOD("Socket", "recv",      0, 0, "Json", "__socket_recv")
LUX_BROWSER_AMETHOD("Socket", "wait_open", 0, 0, "bool", "__socket_wait_open")
// Form (form_data) / File (input type=file)
LUX_BROWSER_METHOD("Form", "add",      2, 2, nullptr)
LUX_BROWSER_METHOD("Form", "add_file", 2, 2, nullptr)
LUX_BROWSER_METHOD("Form", "progress", 0, 0, "Dict")      // {sent, total} bytes of the upload in flight
LUX_BROWSER_METHOD("File", "name",         0, 0, "string")
LUX_BROWSER_METHOD("File", "size",         0, 0, "int")
LUX_BROWSER_METHOD("File", "path",         0, 0, "string")
LUX_BROWSER_METHOD("File", "content_type", 0, 0, "string")

LUX_BROWSER_METHOD("string", "starts_with", 1, 1, "bool")
LUX_BROWSER_METHOD("string", "ends_with",   1, 1, "bool")
LUX_BROWSER_METHOD("string", "contains",    1, 1, "bool")
LUX_BROWSER_METHOD("string", "upper",       0, 0, "string")
LUX_BROWSER_METHOD("string", "lower",       0, 0, "string")
LUX_BROWSER_METHOD("string", "trim",        0, 0, "string")
LUX_BROWSER_METHOD("string", "index_of",    1, 1, "int")
LUX_BROWSER_METHOD("string", "replace",     2, 2, "string")
LUX_BROWSER_METHOD("string", "split",       1, 1, "List")
LUX_BROWSER_METHOD("string", "slice",       1, 2, "string")
LUX_BROWSER_METHOD("string", "repeat",      1, 1, "string")

LUX_BROWSER_METHOD("List", "add",        1, 1, nullptr)
LUX_BROWSER_METHOD("List", "contains",   1, 1, "bool")
LUX_BROWSER_METHOD("List", "index_of",   1, 1, "int")
LUX_BROWSER_METHOD("List", "remove_at",  1, 1, "bool")
LUX_BROWSER_METHOD("List", "sort",       0, 0, nullptr)
LUX_BROWSER_METHOD("List", "reverse",    0, 0, nullptr)
LUX_BROWSER_METHOD("List", "slice",      1, 2, "List")
LUX_BROWSER_METHOD("List", "concat",     1, 1, "List")
LUX_BROWSER_METHOD("List", "join",       1, 1, "string")
LUX_BROWSER_METHOD("List", "map",        1, 1, "List")
LUX_BROWSER_METHOD("List", "filter",     1, 1, "List")
LUX_BROWSER_METHOD("List", "reduce",     2, 2, "Json")
LUX_BROWSER_METHOD("List", "for_each",   1, 1, nullptr)
LUX_BROWSER_METHOD("List", "sort_by",    1, 1, nullptr)
LUX_BROWSER_METHOD("List", "find",       1, 1, "Json")
LUX_BROWSER_METHOD("List", "find_index", 1, 1, "int")
LUX_BROWSER_METHOD("List", "first",      0, 0, "Json")
LUX_BROWSER_METHOD("List", "last",       0, 0, "Json")
LUX_BROWSER_METHOD("List", "pop",        0, 0, "Json")
LUX_BROWSER_METHOD("List", "insert",     2, 2, nullptr)
LUX_BROWSER_METHOD("List", "sum",        0, 0, "Json")
LUX_BROWSER_METHOD("List", "min",        0, 0, "Json")
LUX_BROWSER_METHOD("List", "max",        0, 0, "Json")
LUX_BROWSER_METHOD("List", "unique",     0, 0, "List")
LUX_BROWSER_METHOD("List", "any",        0, 1, "bool")
LUX_BROWSER_METHOD("List", "all",        0, 1, "bool")
LUX_BROWSER_METHOD("List", "count",      0, 1, "int")
LUX_BROWSER_METHOD("List", "group_by",   1, 1, "Dict")
LUX_BROWSER_METHOD("List", "chunk",      1, 1, "List")

LUX_BROWSER_METHOD("Dict", "has",    1, 1, "bool")
LUX_BROWSER_METHOD("Dict", "keys",   0, 0, "List")
LUX_BROWSER_METHOD("Dict", "values", 0, 0, "List")
LUX_BROWSER_METHOD("Dict", "get",    1, 2, "Json")
LUX_BROWSER_METHOD("Dict", "remove", 1, 1, "bool")
LUX_BROWSER_METHOD("Dict", "merge",  1, 1, nullptr)
LUX_BROWSER_METHOD("Dict", "items",  0, 0, "List")

#undef LUX_BROWSER_AMETHOD
#undef LUX_BROWSER_METHOD
#undef LUX_BROWSER_METHOD_X
#ifdef LUX_BROWSER_CALL_DEFAULTED
#undef LUX_BROWSER_CALL
#undef LUX_BROWSER_CALL_DEFAULTED
#endif
#undef LUX_BROWSER_NATIVE
#undef LUX_BROWSER_MEMBER
