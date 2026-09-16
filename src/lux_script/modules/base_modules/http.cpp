// An outbound HTTP(S) client module (NATIVE-MODULES.md), built on libcurl
// rather than a hand-rolled socket client: README.md/CMakeLists.txt commit
// Lux to never linking TLS for the SERVER side ("TLS... belongs to the
// reverse proxy"), but that principle was about not being an inbound TLS
// terminator -- it does not, and cannot, extend to outbound calls, since
// there is no reverse proxy sitting between Lux and a third-party HTTPS
// API. Hand-rolling TLS from scratch is not a responsible way to get there,
// so this is a narrow, explicit exception: libcurl brings its own mature
// TLS backend, used here with its secure defaults (peer/host verification
// ON, never disabled).
//
// Every function here is `is_async` (BuiltinModuleFn::is_async): `await
// http.get(...)` runs the call on lux::blocking_pool() and resumes the
// handler back on its own event loop thread when it finishes, the same
// shape DbDriver already has for a slow query -- see run_builtin_module_async
// (project.cpp) for the driver side of this. A slow/hung remote server still
// ties up a pool worker for the duration of the request (bounded by the
// fixed timeout below so a hung server cannot hold one forever), but no
// longer the event-loop thread serving every OTHER connection on that core --
// this was the one module where NATIVE-MODULES.md §6 called that a real, not
// theoretical, cost, and the worker-pool/await generalization it flagged as
// the clearest next phase.
#include <lux_script/builtin_module.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace lux_script {

namespace {

// A hung remote server would otherwise pin the calling thread (and every
// connection it was serving) forever -- there is no way to interrupt a
// blocking call from outside once it starts, so the bound has to be set
// before curl_easy_perform() ever runs. 15s matches most HTTP clients'
// defaults (requests, axios) closely enough to not surprise anyone.
constexpr long kTimeoutMs = 15000;
constexpr long kMaxRedirects = 5;

// A remote server behind http.* is attacker-controlled just as often as any
// client hitting this framework's own listener is (it is a URL a handler
// built from user input, or a third-party API that gets compromised) --
// yet unlike the inbound side (http_parser.hpp's kMaxBodySize), nothing
// capped what write_body()/write_header() below would accumulate. A
// malicious or hung-but-still-sending server could pin this pool worker's
// memory to whatever size it liked for the whole kTimeoutMs window.
// Returning anything other than the full byte count from a libcurl
// write/header callback aborts the transfer with CURLE_WRITE_ERROR, which
// do_request() already turns into a normal `error` result below -- no new
// failure path needed.
constexpr size_t kMaxResponseBodyBytes   = 16 * 1024 * 1024; // matches inbound kMaxBodySize
constexpr size_t kMaxResponseHeaderBytes = 64 * 1024;

// curl_global_init() is NOT thread-safe against concurrent calls, but IS
// safe to call once, early, before any other thread touches curl -- exactly
// what a call from BuiltinModuleRegistry's constructor gives (single-
// threaded startup, before the event loop threads exist).
struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto*  out = static_cast<std::string*>(userdata);
    size_t n   = size * nmemb;
    if (out->size() + n > kMaxResponseBodyBytes) return 0; // short write -> curl aborts
    out->append(ptr, n);
    return n;
}

// Bundles the Dict together with a running byte total: the total has to be
// tracked incrementally here since summing the Dict's own strings on every
// call would make the cap itself O(n^2) over a header block with many lines.
struct HeaderSink {
    Value::Dict headers;
    size_t      bytes = 0;
};

// Collects response headers into a Dict -- a repeated header keeps its LAST
// occurrence, documented (GUIDE.md), not an oversight.
size_t write_header(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    auto*  sink  = static_cast<HeaderSink*>(userdata);
    if (sink->bytes + total > kMaxResponseHeaderBytes) return 0; // short write -> curl aborts
    sink->bytes += total;

    std::string_view line(buffer, total);
    // The status line ("HTTP/1.1 200 OK") and the blank line that ends the
    // header block both arrive through this callback too -- neither has a
    // ':', so both are skipped by the same check.
    size_t colon = line.find(':');
    if (colon == std::string_view::npos) return total;
    std::string name(line.substr(0, colon));
    size_t vstart = colon + 1;
    while (vstart < line.size() && (line[vstart] == ' ' || line[vstart] == '\t')) ++vstart;
    size_t vend = line.size();
    while (vend > vstart && (line[vend - 1] == '\r' || line[vend - 1] == '\n')) --vend;
    std::string value(line.substr(vstart, vend - vstart));
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    sink->headers[name] = Value::str(value);
    return total;
}

// Shared by get/post/put/patch/delete_ -- the five verbs differ only in
// method name and whether a body is sent, so one function doing the actual
// curl work keeps that difference the only thing each caller states.
Value do_request(const std::string& method, const std::string& url,
                 const std::string* body, bool body_is_json,
                 const Value* headers_arg, std::string& error) {
    static CurlGlobal global;

    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        error = "http." + method + "(): url must start with http:// or https://";
        return Value::null();
    }

    CURL* curl = curl_easy_init();
    if (!curl) { error = "http: could not initialize the request"; return Value::null(); }

    std::string response_body;
    HeaderSink  response_headers;
    struct curl_slist* header_list = nullptr;
    bool content_type_set = false;

    if (headers_arg && headers_arg->is_dict()) {
        for (const auto& [key, val] : headers_arg->as_dict()) {
            if (!val.is_str()) continue; // a non-string header value is silently skipped, not an error
            // Strip CR/LF and NUL before handing the line to curl_slist_append:
            // Response::header() (response.hpp) and build_set_cookie()
            // (cookies.hpp) both strip CR/LF on the INBOUND side of this
            // framework for exactly this reason -- without it here, a
            // key/value built from request data lets a handler inject
            // arbitrary extra header lines (or truncate this one at an
            // embedded NUL) into the OUTBOUND request this module sends.
            auto strip = [](std::string s) {
                s.erase(std::remove_if(s.begin(), s.end(),
                    [](char c) { return c == '\r' || c == '\n' || c == '\0'; }), s.end());
                return s;
            };
            std::string line = strip(key) + ": " + strip(val.as_str());
            header_list = curl_slist_append(header_list, line.c_str());
            std::string lower = key;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "content-type") content_type_set = true;
        }
    }
    if (body && body_is_json && !content_type_set) {
        // Only for a body the CALLER passed as a Dict/List/Json (auto-
        // serialized in do_body_verb below) -- a raw string body is sent
        // exactly as given, with no assumption about its content type.
        header_list = curl_slist_append(header_list, "Content-Type: application/json");
    }
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (method == "post") curl_easy_setopt(curl, CURLOPT_POST, 1L);
    else if (method == "put") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    else if (method == "patch") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    else if (method == "delete") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    // "get" needs neither option: CURLOPT_HTTPGET is curl's default state.

    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
    // A redirect is server-controlled, not just the initial URL: without
    // this, a 30x response could point CURLOPT_FOLLOWLOCATION at
    // file:///etc/passwd or another non-HTTP scheme curl happens to support,
    // regardless of what scheme this module's own `url.rfind(...)` check
    // above validated on the way in. The bitmask form (not the _STR variant
    // added in curl 7.85) is used so this keeps building against whatever
    // older libcurl-dev a distro ships.
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lux/1.0");
    // Secure by default, and never made configurable to "off" from Lux
    // Script: the whole reason this module exists as an exception to "no
    // TLS" is that libcurl's TLS is trustworthy, which stops being true the
    // moment peer/host verification can be switched off from a route.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        error = std::string("http.") + method + "(): " + curl_easy_strerror(rc);
        if (header_list) curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        return Value::null();
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    Value::Dict out;
    out["status"] = Value::integer(status);
    out["headers"] = Value::dict(std::move(response_headers.headers));
    // The body is handed back parsed whenever it looks like JSON -- the
    // common case for an API client -- and as the raw text otherwise (HTML,
    // plain text, an error page that is not JSON): Value::parse_json()
    // is the exact function every incoming request body already goes
    // through (project.cpp), so this is the same rule applied symmetrically
    // to what comes back.
    Value parsed;
    if (Value::parse_json(response_body, parsed)) out["body"] = parsed;
    else out["body"] = Value::str(response_body);
    return Value::dict(std::move(out));
}

const Value* opt_headers(std::vector<Value>& args, size_t idx) {
    return (args.size() > idx && args[idx].is_dict()) ? &args[idx] : nullptr;
}

Value fn_http_get(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "http.get() expects a url"; return Value::null(); }
    return do_request("get", args[0].as_str(), nullptr, false, opt_headers(args, 1), error);
}

Value fn_http_delete(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "http.delete() expects a url"; return Value::null(); }
    return do_request("delete", args[0].as_str(), nullptr, false, opt_headers(args, 1), error);
}

// Shared by post/put/patch: a url, a body (a string sent as-is, or any
// other Value JSON-serialized first, see do_request), and optional headers.
Value do_body_verb(const char* method, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = std::string("http.") + method + "() expects a url"; return Value::null(); }
    std::string body_text;
    bool is_json = !args[1].is_str();
    if (is_json) body_text = args[1].to_json_text();
    else         body_text = args[1].as_str();
    return do_request(method, args[0].as_str(), &body_text, is_json, opt_headers(args, 2), error);
}

Value fn_http_post(NativeCtx&, std::vector<Value>& args, std::string& error)  { return do_body_verb("post", args, error); }
Value fn_http_put(NativeCtx&, std::vector<Value>& args, std::string& error)   { return do_body_verb("put", args, error); }
Value fn_http_patch(NativeCtx&, std::vector<Value>& args, std::string& error) { return do_body_verb("patch", args, error); }

class HttpModule : public BuiltinModule {
public:
    const char* name() const override { return "http"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        // Every one is is_async: a slow/hung remote server is the real cost
        // this module exists to bound (see the module comment above), so
        // `await http.*(...)` is mandatory -- routed through
        // lux::blocking_pool() (BuiltinModuleFn::is_async) instead of the
        // event loop thread.
        static const std::vector<BuiltinModuleFn> fns = {
            {"get",    1, 2, fn_http_get,    /*is_async=*/true},
            {"post",   2, 3, fn_http_post,   /*is_async=*/true},
            {"put",    2, 3, fn_http_put,    /*is_async=*/true},
            {"patch",  2, 3, fn_http_patch,  /*is_async=*/true},
            {"delete", 1, 2, fn_http_delete, /*is_async=*/true},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(HttpModule)

} // namespace lux_script
