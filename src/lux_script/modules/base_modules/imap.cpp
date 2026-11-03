// Reading mail over IMAP, through libcurl (built with the http module).
//
// Stateless: every call carries its own connection Dict, so one app can talk
// to any number of accounts and nothing outlives a call.
//
//     Json c = { "host": "imap.example.com", "user": "me", "password": pw }   # port 993, tls "tls"
//     Json c = { "host": "imap.gmail.com",   "user": "me@gmail.com", "token": access_token }  # XOAUTH2
//     "tls": "tls" (993, default) | "starttls" (143) | "none" (143)
//
//     List folders = await imap.list_folders(c)         # [{name, attrs: ["\\Sent", ...]}]
//     Json st      = await imap.status(c, "INBOX")      # {uidvalidity, uidnext, messages, unseen}
//     List msgs    = await imap.uids(c, "INBOX", 1200)  # [{uid, flags}] for uid >= 1200
//     string head  = await imap.fetch(c, "INBOX", 1203, "HEADER")  # raw RFC 822; section "" = whole
//     await imap.set_flags(c, "INBOX", 1203, "add", ["\\Seen"])    # "add" | "remove" | "set"
//     await imap.move(c, "INBOX", 1203, "Trash")
//     await imap.append(c, "Sent", raw_message)
//     await imap.create_folder(c, "Año")  await imap.rename_folder(c, "Año", "Año 2026")  await imap.delete_folder(c, "Año 2026")
//
// Folder names are UTF-8 on both sides: they go to the server as modified UTF-7 ("A&APE-o" for "Año") and
// come back decoded. Every value that reaches the wire has
// CR/LF stripped or is validated, so account data cannot inject a command.
//
// ponytail: one connection per call (curl handle is not kept). Fine for a poll every
// few minutes; keep a per-account handle if sync of many folders gets slow.
#include <lux_script/builtin_module.hpp>
#include <lux/percent_encoding.hpp>

#include <curl/curl.h>
#include <lux_script/curl_init.hpp>

#include <algorithm>
#include <cstring>

namespace lux_script {

namespace {

constexpr size_t kMaxFetchBytes = 64u << 20;
constexpr long   kTimeoutMs     = 60'000;

struct Conn {
    std::string host, user, password, token, tls = "tls";
    long        port = 993;
};

std::string field(const Value::Dict& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() || it->second.is_null() ? "" : it->second.to_string();
}

bool plain(const std::string& s, const char* extra) {
    return std::all_of(s.begin(), s.end(), [&](unsigned char c) { return std::isalnum(c) || std::strchr(extra, c); });
}

bool parse_conn(const Value& v, const char* fn, Conn& c, std::string& error) {
    const auto& m = v.as_dict();
    c.host = field(m, "host");
    c.user = field(m, "user");
    c.password = field(m, "password");
    c.token = field(m, "token");
    if (auto t = field(m, "tls"); !t.empty()) c.tls = t;
    if (c.tls != "tls" && c.tls != "starttls" && c.tls != "none") {
        error = std::string(fn) + "(): tls is \"tls\", \"starttls\" or \"none\"";
        return false;
    }
    c.port = c.tls == "tls" ? 993 : 143;
    if (auto p = field(m, "port"); !p.empty()) c.port = std::atol(p.c_str());
    // A host that is not a bare name could turn the URL into something else (user@, /path).
    if (c.host.empty() || !plain(c.host, ".-:[]") || c.port <= 0 || c.port > 65535) {
        error = std::string(fn) + "(): the connection needs a valid host (and port)";
        return false;
    }
    if (c.user.empty() || (c.password.empty() && c.token.empty())) {
        error = std::string(fn) + "(): the connection needs user and password (or token)";
        return false;
    }
    return true;
}

// Folder names travel as modified UTF-7 ("A&APE-o" for "Año"): the caller always sees UTF-8.
std::string b64_encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";   // ',' instead of '/'
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; out += t[(acc >> bits) & 63]; }
    }
    if (bits) out += t[(acc << (6 - bits)) & 63];   // no '=' padding in modified UTF-7
    return out;
}

std::string b64_decode(const std::string& in) {
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (char c : in) {
        const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
        const char* p = std::strchr(t, c);
        if (!p || !c) continue;
        acc = (acc << 6) | static_cast<unsigned>(p - t);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += static_cast<char>((acc >> bits) & 0xFF); }
    }
    return out;
}

std::string utf7_encode(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c <= 0x7E) { out += c == '&' ? "&-" : std::string(1, static_cast<char>(c)); ++i; continue; }
        std::string utf16;   // a run of characters outside printable ASCII, as UTF-16BE
        while (i < s.size() && !(static_cast<unsigned char>(s[i]) >= 0x20 && static_cast<unsigned char>(s[i]) <= 0x7E)) {
            const unsigned char b = static_cast<unsigned char>(s[i]);
            const int len = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
            unsigned cp = len == 1 ? b : len == 2 ? b & 0x1F : len == 3 ? b & 0x0F : b & 0x07;
            for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            i += static_cast<size_t>(len);
            auto unit = [&](unsigned u) { utf16 += static_cast<char>(u >> 8); utf16 += static_cast<char>(u & 0xFF); };
            if (cp >= 0x10000) { cp -= 0x10000; unit(0xD800 + (cp >> 10)); unit(0xDC00 + (cp & 0x3FF)); }
            else unit(cp);
        }
        out += "&" + b64_encode(utf16) + "-";
    }
    return out;
}

std::string utf7_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t end = s.find('-', i);
        if (end == std::string::npos) { out += s.substr(i); break; }
        if (end == i + 1) { out += '&'; i = end; continue; }
        const std::string raw = b64_decode(s.substr(i + 1, end - i - 1));
        for (size_t k = 0; k + 1 < raw.size(); k += 2) {
            unsigned cp = (static_cast<unsigned char>(raw[k]) << 8) | static_cast<unsigned char>(raw[k + 1]);
            if (cp >= 0xD800 && cp < 0xDC00 && k + 3 < raw.size()) {
                const unsigned lo = (static_cast<unsigned char>(raw[k + 2]) << 8) | static_cast<unsigned char>(raw[k + 3]);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                k += 2;
            }
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        }
        i = end;
    }
    return out;
}

// A mailbox name inside the URL path: '/' stays a separator, everything else is encoded.
std::string url_box(const std::string& box) {
    std::string e = lux::percent_encode(utf7_encode(box)), out;
    for (size_t i = 0; i < e.size(); ++i) {
        if (e.compare(i, 3, "%2F") == 0) { out += '/'; i += 2; }
        else out += e[i];
    }
    return out;
}

// A mailbox name as an IMAP quoted string (custom commands), already in modified UTF-7.
bool quoted(const std::string& s, std::string& out) {
    out = "\"";
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return false;   // no CR/LF/NUL smuggling a second command
        if (c == '"' || c == '\\') out += '\\';
        out += static_cast<char>(c);
    }
    out += '"';
    return true;
}

struct Sink { std::string body; };

size_t on_body(char* p, size_t sz, size_t n, void* u) {
    auto* s = static_cast<Sink*>(u);
    if (s->body.size() + sz * n > kMaxFetchBytes) return 0;   // short write aborts the transfer
    s->body.append(p, sz * n);
    return sz * n;
}

struct Upload { const std::string* data; size_t pos = 0; };

size_t on_read(char* buf, size_t sz, size_t n, void* u) {
    auto* up = static_cast<Upload*>(u);
    const size_t len = std::min(sz * n, up->data->size() - up->pos);
    std::memcpy(buf, up->data->data() + up->pos, len);
    up->pos += len;
    return len;
}

// One curl handle with the account's connection options, a body sink, and an error buffer.
struct Session {
    CURL* curl = lux_curl_init();
    Conn  conn;
    Sink  sink;
    char  err[CURL_ERROR_SIZE] = "";

    explicit Session(const Conn& c) : conn(c) {
        if (!curl) return;
        curl_easy_setopt(curl, CURLOPT_USERNAME, conn.user.c_str());
        if (!conn.token.empty()) curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, conn.token.c_str());
        else curl_easy_setopt(curl, CURLOPT_PASSWORD, conn.password.c_str());
        if (conn.tls == "starttls") curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        curl_easy_setopt(curl, CURLOPT_PORT, conn.port);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    }
    ~Session() { if (curl) curl_easy_cleanup(curl); }

    // `path` is "" or "/Folder" or "/Folder;UID=5". `command` empty = plain fetch/list.
    bool run(const std::string& path, const std::string& command, const char* fn, std::string& error) {
        if (!curl) { error = std::string(fn) + "(): could not initialize"; return false; }
        const std::string url = std::string(conn.tls == "tls" ? "imaps://" : "imap://") + conn.host + path;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, command.empty() ? nullptr : command.c_str());
        sink.body.clear();
        err[0] = 0;
        const CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) return true;
        // Login failures are the common one (bad password, expired OAuth token): say so plainly.
        error = std::string(fn) + "(): " +
                (rc == CURLE_LOGIN_DENIED ? "login failed (wrong password or expired token)"
                                          : err[0] ? err : curl_easy_strerror(rc));
        return false;
    }
};

// Folder + connection common to most functions.
#define IMAP_PREP(fn)                                                          \
    Conn c;                                                                    \
    if (!parse_conn(a[0], fn, c, error)) return Value::null();                 \
    Session s(c);

std::vector<std::string> lines(const std::string& body) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        size_t end = e;
        if (end > i && body[end - 1] == '\r') --end;
        out.push_back(body.substr(i, end - i));
        i = e + 1;
    }
    return out;
}

// The number following `key` ("UIDNEXT 9"), or -1.
long long number_after(const std::string& line, const std::string& key) {
    size_t p = line.find(key + " ");
    if (p == std::string::npos) return -1;
    p += key.size() + 1;
    if (p >= line.size() || !std::isdigit(static_cast<unsigned char>(line[p]))) return -1;
    return std::atoll(line.c_str() + p);
}

// Space-separated tokens inside "(" ")" starting at `open`.
std::vector<std::string> paren_words(const std::string& line, size_t open) {
    std::vector<std::string> out;
    const size_t close = line.find(')', open);
    if (open == std::string::npos || close == std::string::npos) return out;
    std::string cur;
    for (size_t i = open + 1; i <= close; ++i) {
        if (i == close || line[i] == ' ') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += line[i];
    }
    return out;
}

Value words_list(const std::vector<std::string>& w) {
    Value::List l;
    for (const auto& x : w) l.push_back(Value::str(x));
    return Value::list(std::move(l));
}

// * LIST (\HasNoChildren \Sent) "/" "[Gmail]/Sent Mail"
Value fn_list_folders(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.list_folders")
    if (!s.run("/", "LIST \"\" \"*\"", "imap.list_folders", error)) return Value::null();
    Value::List out;
    for (const auto& line : lines(s.sink.body)) {
        if (line.rfind("* LIST ", 0) != 0) continue;
        const size_t open = line.find('(');
        const size_t close = line.find(')', open);
        if (close == std::string::npos) continue;
        // After the attrs: a delimiter ("/" or NIL), then the name (quoted or an atom).
        size_t p = line.find(' ', close + 2);
        if (p == std::string::npos) continue;
        std::string name = line.substr(p + 1);
        if (name.size() >= 2 && name.front() == '"') {
            std::string un;
            for (size_t i = 1; i + 1 < name.size(); ++i) {
                if (name[i] == '\\' && i + 2 < name.size()) ++i;
                un += name[i];
            }
            name = un;
        }
        Value::Dict d;
        d["name"] = Value::str(utf7_decode(name));
        d["attrs"] = words_list(paren_words(line, open));
        out.push_back(Value::dict(std::move(d)));
    }
    return Value::list(std::move(out));
}

// * STATUS "INBOX" (MESSAGES 3 UIDNEXT 9 UIDVALIDITY 123 UNSEEN 1)
Value fn_status(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.status")
    std::string q;
    if (!quoted(utf7_encode(a[1].as_str()), q)) { error = "imap.status(): invalid folder name"; return Value::null(); }
    if (!s.run("/", "STATUS " + q + " (MESSAGES UIDNEXT UIDVALIDITY UNSEEN)", "imap.status", error)) return Value::null();
    Value::Dict d;
    for (const auto& line : lines(s.sink.body)) {
        if (line.rfind("* STATUS", 0) != 0) continue;
        const char* keys[][2] = {{"MESSAGES", "messages"}, {"UIDNEXT", "uidnext"},
                                 {"UIDVALIDITY", "uidvalidity"}, {"UNSEEN", "unseen"}};
        for (auto& k : keys) d[k[1]] = Value::integer(std::max(0LL, number_after(line, k[0])));
    }
    return Value::dict(std::move(d));
}

// * 12 FETCH (UID 345 FLAGS (\Seen \Answered))
Value fn_uids(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.uids")
    const long long since = std::max<long long>(1, a[2].as_int());
    if (!s.run("/" + url_box(a[1].as_str()), "UID FETCH " + std::to_string(since) + ":* (FLAGS)", "imap.uids", error))
        return Value::null();
    Value::List out;
    for (const auto& line : lines(s.sink.body)) {
        if (line.find(" FETCH ") == std::string::npos) continue;
        const long long uid = number_after(line, "UID");
        if (uid < since) continue;   // "n:*" always includes the last message, even below n
        Value::Dict d;
        d["uid"] = Value::integer(uid);
        d["flags"] = words_list(paren_words(line, line.find('(', line.find("FLAGS"))));
        out.push_back(Value::dict(std::move(d)));
    }
    return Value::list(std::move(out));
}

Value fn_fetch(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.fetch")
    const std::string section = a.size() > 3 ? a[3].as_str() : "";
    if (!plain(section, ".")) { error = "imap.fetch(): invalid section"; return Value::null(); }
    std::string path = "/" + url_box(a[1].as_str()) + ";UID=" + std::to_string(a[2].as_int());
    if (!section.empty()) path += ";SECTION=" + section;
    if (!s.run(path, "", "imap.fetch", error)) return Value::null();
    return Value::str(std::move(s.sink.body));
}

Value fn_set_flags(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.set_flags")
    const std::string mode = a[3].as_str();
    const char* op = mode == "add" ? "+FLAGS.SILENT" : mode == "remove" ? "-FLAGS.SILENT" : mode == "set" ? "FLAGS.SILENT" : nullptr;
    if (!op) { error = "imap.set_flags(): mode is \"add\", \"remove\" or \"set\""; return Value::null(); }
    std::string flags;
    for (const Value& f : a[4].as_list()) {
        const std::string x = f.to_string();
        if (x.empty() || !plain(x, "\\$-_")) { error = "imap.set_flags(): invalid flag '" + x + "'"; return Value::null(); }
        flags += (flags.empty() ? "" : " ") + x;
    }
    if (!s.run("/" + url_box(a[1].as_str()),
               "UID STORE " + std::to_string(a[2].as_int()) + " " + op + " (" + flags + ")", "imap.set_flags", error))
        return Value::null();
    return Value::boolean(true);
}

// UID MOVE (RFC 6851); servers without it get COPY + \Deleted + UID EXPUNGE (UIDPLUS).
Value fn_move(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.move")
    std::string dest;
    if (!quoted(utf7_encode(a[3].as_str()), dest)) { error = "imap.move(): invalid folder name"; return Value::null(); }
    const std::string path = "/" + url_box(a[1].as_str()), uid = std::to_string(a[2].as_int());
    std::string first;
    if (s.run(path, "UID MOVE " + uid + " " + dest, "imap.move", first)) return Value::boolean(true);
    if (!s.run(path, "UID COPY " + uid + " " + dest, "imap.move", error) ||
        !s.run(path, "UID STORE " + uid + " +FLAGS.SILENT (\\Deleted)", "imap.move", error) ||
        !s.run(path, "UID EXPUNGE " + uid, "imap.move", error))
        return Value::null();
    return Value::boolean(true);
}

// Folder management. Names are UTF-8; control characters are refused.
Value fn_create_folder(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.create_folder")
    std::string q;
    if (a[1].as_str().empty() || !quoted(utf7_encode(a[1].as_str()), q)) { error = "imap.create_folder(): invalid folder name"; return Value::null(); }
    if (!s.run("/", "CREATE " + q, "imap.create_folder", error)) return Value::null();
    return Value::boolean(true);
}

Value fn_rename_folder(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.rename_folder")
    std::string from, to;
    if (a[2].as_str().empty() || !quoted(utf7_encode(a[1].as_str()), from) || !quoted(utf7_encode(a[2].as_str()), to)) {
        error = "imap.rename_folder(): invalid folder name";
        return Value::null();
    }
    if (!s.run("/", "RENAME " + from + " " + to, "imap.rename_folder", error)) return Value::null();
    return Value::boolean(true);
}

Value fn_delete_folder(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.delete_folder")
    std::string q;
    if (a[1].as_str().empty() || !quoted(utf7_encode(a[1].as_str()), q)) { error = "imap.delete_folder(): invalid folder name"; return Value::null(); }
    if (!s.run("/", "DELETE " + q, "imap.delete_folder", error)) return Value::null();
    return Value::boolean(true);
}

// UID of the message whose Message-ID header is `id` in `folder`, or 0 (APPEND does not tell us the
// uid it gave a new message, so a draft is found again by its Message-ID).
Value fn_find(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.find")
    std::string q;
    if (!quoted(a[2].as_str(), q)) { error = "imap.find(): invalid Message-ID"; return Value::null(); }
    if (!s.run("/" + url_box(a[1].as_str()), "UID SEARCH HEADER Message-ID " + q, "imap.find", error)) return Value::null();
    for (const auto& line : lines(s.sink.body)) {
        if (line.rfind("* SEARCH", 0) != 0) continue;
        const size_t p = line.find_first_of("0123456789", 8);
        return Value::integer(p == std::string::npos ? 0 : std::atoll(line.c_str() + p));
    }
    return Value::integer(0);
}

// Flags the message \Deleted and expunges it (UIDPLUS); on a server without UIDPLUS it stays flagged.
Value fn_remove(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.remove")
    const std::string path = "/" + url_box(a[1].as_str()), uid = std::to_string(a[2].as_int());
    if (!s.run(path, "UID STORE " + uid + " +FLAGS.SILENT (\\Deleted)", "imap.remove", error)) return Value::null();
    std::string ignored;
    s.run(path, "UID EXPUNGE " + uid, "imap.remove", ignored);
    return Value::boolean(true);
}

// curl's APPEND marks the message \Seen and gives no way to choose other flags; the uid the server
// assigns is not reported either (find it again by Message-ID with imap.find).
Value fn_append(NativeCtx&, std::vector<Value>& a, std::string& error) {
    IMAP_PREP("imap.append")
    const std::string& raw = a[2].as_str();
    Upload up{&raw};
    curl_easy_setopt(s.curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(s.curl, CURLOPT_READFUNCTION, on_read);
    curl_easy_setopt(s.curl, CURLOPT_READDATA, &up);
    curl_easy_setopt(s.curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(raw.size()));
    if (!s.run("/" + url_box(a[1].as_str()), "", "imap.append", error)) return Value::null();
    return Value::boolean(true);
}

} // namespace

LUX_MODULE(imap, {
    {"list_folders", "d>l",       fn_list_folders, /*is_async=*/true},
    {"status",       "ds>d",      fn_status,       true},
    {"uids",         "dsi>l",     fn_uids,         true},
    {"fetch",        "dsi|s>s",   fn_fetch,        true},
    {"set_flags",    "dsisl>b",   fn_set_flags,    true},
    {"move",         "dsis>b",    fn_move,         true},
    {"append",       "dss>b",     fn_append,       true},
    {"find",         "dss>i",     fn_find,         true},
    {"remove",       "dsi>b",     fn_remove,       true},
    {"create_folder", "ds>b",     fn_create_folder, true},
    {"rename_folder", "dss>b",    fn_rename_folder, true},
    {"delete_folder", "ds>b",     fn_delete_folder, true},
})

} // namespace lux_script
