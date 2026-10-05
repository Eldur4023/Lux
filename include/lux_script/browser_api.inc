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
//   LUX_BROWSER_NATIVE(name, min_args, max_args, impl)
//   LUX_BROWSER_MEMBER(object, member, native)       object.member -> native
//   LUX_BROWSER_METHOD(type, name, min_args, max_args, return_type or nullptr)

#ifndef LUX_BROWSER_NATIVE
#define LUX_BROWSER_NATIVE(name, min, max, impl)
#endif
#ifndef LUX_BROWSER_MEMBER
#define LUX_BROWSER_MEMBER(object, member, native)
#endif
#ifndef LUX_BROWSER_METHOD
#define LUX_BROWSER_METHOD(type, name, min, max, ret)
#endif

// ── natives (stdlib, same semantics as the server's) ────────────────────────
LUX_BROWSER_NATIVE("len",   1, 1, fn_len)
LUX_BROWSER_NATIVE("str",   1, 1, fn_str)
LUX_BROWSER_NATIVE("int",   1, 1, fn_int)
LUX_BROWSER_NATIVE("float", 1, 1, fn_float)
LUX_BROWSER_NATIVE("range", 1, 3, fn_range)
// log.*
LUX_BROWSER_NATIVE("__log_info",  1, 1, fn_log_info)
LUX_BROWSER_NATIVE("__log_warn",  1, 1, fn_log_warn)
LUX_BROWSER_NATIVE("__log_error", 1, 1, fn_log_error)
// document.*
LUX_BROWSER_NATIVE("__document_get",       1, 1, fn_document_get)
LUX_BROWSER_NATIVE("__document_query_all", 1, 1, fn_document_query_all)
LUX_BROWSER_NATIVE("__document_title",     0, 0, fn_document_title)
LUX_BROWSER_NATIVE("__document_root",      0, 0, fn_document_root)
LUX_BROWSER_NATIVE("__document_create",    1, 1, fn_document_create)
// page.* -- timers, the clicked element, the page-scoped store
LUX_BROWSER_NATIVE("__page_after",   2, 2, fn_page_after)
LUX_BROWSER_NATIVE("__page_clicked", 0, 0, fn_page_clicked)
LUX_BROWSER_NATIVE("__page_get",     1, 1, fn_page_get)
LUX_BROWSER_NATIVE("__page_set",     2, 2, fn_page_set)

// ── reserved objects ────────────────────────────────────────────────────────
LUX_BROWSER_MEMBER("log",      "info",      "__log_info")
LUX_BROWSER_MEMBER("log",      "warn",      "__log_warn")
LUX_BROWSER_MEMBER("log",      "error",     "__log_error")
LUX_BROWSER_MEMBER("document", "get",       "__document_get")
LUX_BROWSER_MEMBER("document", "query_all", "__document_query_all")
LUX_BROWSER_MEMBER("document", "title",     "__document_title")
LUX_BROWSER_MEMBER("document", "root",      "__document_root")
LUX_BROWSER_MEMBER("document", "create",    "__document_create")
LUX_BROWSER_MEMBER("page",     "get",       "__page_get")
LUX_BROWSER_MEMBER("page",     "set",       "__page_set")
LUX_BROWSER_MEMBER("page",     "after",     "__page_after")
LUX_BROWSER_MEMBER("page",     "clicked",   "__page_clicked")

// ── methods by receiver type ────────────────────────────────────────────────
LUX_BROWSER_METHOD("Element", "text",        0, 0, "string")
LUX_BROWSER_METHOD("Element", "set_text",    1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "append",      1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "remove",      0, 0, nullptr)
LUX_BROWSER_METHOD("Element", "click",       1, 1, nullptr)
LUX_BROWSER_METHOD("Element", "attr",        1, 1, "string")
LUX_BROWSER_METHOD("Element", "set_attr",    2, 2, nullptr)
LUX_BROWSER_METHOD("Element", "tag",         0, 0, "string")
LUX_BROWSER_METHOD("Element", "id",          0, 0, "string")
LUX_BROWSER_METHOD("Element", "child_count", 0, 0, "int")

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

#undef LUX_BROWSER_NATIVE
#undef LUX_BROWSER_MEMBER
#undef LUX_BROWSER_METHOD
