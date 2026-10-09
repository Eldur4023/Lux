// Sending email over SMTP, through libcurl (built with the http module).
//
//     app:
//         mail:
//             host     "smtp.example.com"
//             port     587                  # 587 STARTTLS (default), 465 TLS, 25/1025 plain
//             user     "apikey"
//             password env("SMTP_PASSWORD")
//             from     "My App <noreply@example.com>"
//             tls      "starttls"           # "starttls" | "tls" | "none"
//
// Several accounts: add "smtp": {"host", "port", "user", "password" | "token" (OAuth2), "tls", "from"}
// to the Dict and it replaces the app: block for that call. `mail.compose(d)` returns the finished
// message text instead of sending it (for imap.append); pass that text back as "raw" (with to/cc/bcc/from)
// to send exactly it. An attachment Dict with a "cid" is an image the "html" refers to as `<img src="cid:...">`
// (sent inside multipart/related). `"draft": true` lets compose build a message with no recipient or body yet and keeps
// Bcc in it (for imap.append to Drafts). "in_reply_to"/"references" set threading.
//
//     await mail.send({ "to": "ana@example.com", "subject": "Hi", "text": "...", "html": "...",
//                       "attachments": ["invoice.pdf", { "name": "data.csv", "content": csv_text }] })
//
// Every header value has CR/LF stripped: a contact form's subject or name
// can never smuggle in a Bcc: line.
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>
#include <lux/mime.hpp>
#include <lux/percent_encoding.hpp>

#include <curl/curl.h>
#include <lux_script/curl_init.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <cstring>
#include <ctime>

namespace lux_script {

namespace {

struct Config {
    std::string url, user, password, token, from, tls = "starttls";
    bool        configured = false;
};

Config& config() {
    static Config c;
    return c;
}

std::string one_line(std::string s) {
    std::erase_if(s, [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
    return s;
}

// "Ana <ana@x.com>" -> "<ana@x.com>", what the SMTP envelope wants.
std::string envelope(const std::string& addr) {
    const size_t lt = addr.rfind('<'), gt = addr.rfind('>');
    return lt != std::string::npos && gt > lt ? addr.substr(lt, gt - lt + 1) : "<" + addr + ">";
}

// RFC 2047 for anything outside printable ASCII: "Café" -> =?UTF-8?B?...?=
std::string header_text(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7E) return "=?UTF-8?B?" + crypto::base64_encode(s) + "?=";
    return s;
}

std::string field(const Value::Dict& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() || it->second.is_null() ? "" : it->second.to_string();
}

std::string wrapped_base64(const std::string& data) {
    const std::string b = crypto::base64_encode(data);
    std::string out;
    for (size_t i = 0; i < b.size(); i += 76) out += b.substr(i, 76) + "\r\n";
    return out;
}

// Base64 body wrapped at 76 columns (RFC 2045).
std::string body_part(const std::string& type, const std::string& text) {
    return "Content-Type: " + type + "; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" + wrapped_base64(text);
}

constexpr size_t kMaxAttachmentBytes = 25u << 20;   // what common mail servers accept

// A path, or {"name", "content"} for something made on the spot (a CSV).
// A name outside ASCII goes as RFC 2231 (filename*=UTF-8''...).
bool attachment_part(const Value& a, std::string& part, size_t& total, std::string& error, bool& is_inline) {
    std::string name, content, cid;
    is_inline = false;
    if (a.is_dict()) {
        name = field(a.as_dict(), "name");
        content = field(a.as_dict(), "content");
        cid = one_line(field(a.as_dict(), "cid"));
        std::erase_if(cid, [](char c) { return c == '<' || c == '>' || c == ' '; });
        if (name.empty()) { error = "mail.send(): an attachment Dict needs a name"; return false; }
    } else {
        const std::string path = a.to_string();
        std::ifstream f(path, std::ios::binary);
        if (!f) { error = "mail.send(): cannot read the attachment '" + path + "'"; return false; }
        content.assign(std::istreambuf_iterator<char>(f), {});
        name = path.substr(path.find_last_of('/') + 1);
    }
    total += content.size();
    if (total > kMaxAttachmentBytes) { error = "mail.send(): the attachments are over 25 MB"; return false; }
    name = one_line(name);
    std::erase_if(name, [](char c) { return c == '"' || c == '\\'; });
    const std::string dot = name.substr(std::min(name.size(), name.rfind('.')));
    const bool ascii = std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
    const std::string disp = ascii ? "filename=\"" + name + "\"" : "filename*=UTF-8''" + lux::percent_encode(name);
    is_inline = !cid.empty();   // an image the HTML refers to as cid:<cid>
    part = "Content-Type: " + std::string(lux::mime_for_ext(dot)) + "\r\nContent-Transfer-Encoding: base64\r\n" +
           (is_inline ? "Content-ID: <" + cid + ">\r\nContent-Disposition: inline; " : std::string("Content-Disposition: attachment; ")) +
           disp + "\r\n\r\n" + wrapped_base64(content);
    return true;
}

// A string or a List of them.
std::vector<std::string> addresses(const Value::Dict& m, const char* key) {
    std::vector<std::string> out;
    auto it = m.find(key);
    if (it == m.end() || it->second.is_null()) return out;
    if (it->second.is_list()) for (const Value& v : it->second.as_list()) out.push_back(one_line(v.to_string()));
    else out.push_back(one_line(it->second.to_string()));
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

struct Upload { std::string data; size_t pos = 0; };

size_t feed(char* buf, size_t size, size_t n, void* u) {
    auto* up = static_cast<Upload*>(u);
    const size_t len = std::min(size * n, up->data.size() - up->pos);
    std::memcpy(buf, up->data.data() + up->pos, len);
    up->pos += len;
    return len;
}

// A per-call "smtp" Dict ({host, port, user, password | token, tls, from}) replaces the
// app: block, so one app can send as several accounts.
bool resolve_config(const Value::Dict& m, const char* fn, Config& cfg, std::string& error) {
    auto it = m.find("smtp");
    if (it == m.end() || !it->second.is_dict()) {
        cfg = config();
        if (!cfg.configured) { error = std::string(fn) + "(): no mail: block (host, from) in app: and no smtp Dict"; return false; }
        return true;
    }
    const auto& o = it->second.as_dict();
    cfg = Config{};
    const std::string host = field(o, "host");
    cfg.tls = field(o, "tls").empty() ? "starttls" : field(o, "tls");
    if (host.empty() || !std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-'; })) {
        error = std::string(fn) + "(): smtp needs a valid host"; return false;
    }
    if (cfg.tls != "starttls" && cfg.tls != "tls" && cfg.tls != "none") {
        error = std::string(fn) + "(): smtp tls is \"starttls\", \"tls\" or \"none\""; return false;
    }
    const std::string port = field(o, "port").empty() ? (cfg.tls == "tls" ? "465" : "587") : field(o, "port");
    cfg.url = std::string(cfg.tls == "tls" ? "smtps://" : "smtp://") + host + ":" + port;
    cfg.user = field(o, "user");
    cfg.password = field(o, "password");
    cfg.token = field(o, "token");
    cfg.from = field(o, "from");
    cfg.configured = true;
    return true;
}

// The RFC 822 message for `m` (headers, body, attachments), CRLF-terminated.
bool build_message(const Value::Dict& m, const Config& cfg, const char* fn, std::string& msg, std::string& env_from,
                   std::vector<std::string>& rcpts, std::string& error) {
    const auto to = addresses(m, "to"), cc = addresses(m, "cc"), bcc = addresses(m, "bcc");
    const std::string text = field(m, "text"), html = field(m, "html");
    auto draft_it = m.find("draft");
    const bool draft = draft_it != m.end() && draft_it->second.truthy();   // a draft may have no recipient or body yet
    if (!draft && to.empty() && cc.empty() && bcc.empty()) { error = std::string(fn) + "(): no recipient (to, cc or bcc)"; return false; }
    const std::string raw = field(m, "raw");
    if (!draft && raw.empty() && text.empty() && html.empty()) { error = std::string(fn) + "(): no body (text or html)"; return false; }

    char date[64];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S +0000", &tm);
    const std::string from = one_line(field(m, "from").empty() ? cfg.from : field(m, "from"));
    if (from.empty()) { error = std::string(fn) + "(): no from address"; return false; }
    env_from = envelope(from);
    if (!raw.empty()) {   // an already composed message (mail.compose): sent as is, same Message-ID
        msg = raw;
        for (const auto* list : {&to, &cc, &bcc}) rcpts.insert(rcpts.end(), list->begin(), list->end());
        return true;
    }
    const size_t at = env_from.find('@');
    const std::string domain = at == std::string::npos ? "lux" : env_from.substr(at + 1, env_from.size() - at - 2);

    msg = "Date: " + std::string(date) + "\r\nFrom: " + from + "\r\n";
    if (!to.empty()) msg += "To: " + joined(to) + "\r\n";
    if (!cc.empty()) msg += "Cc: " + joined(cc) + "\r\n";
    if (draft && !bcc.empty()) msg += "Bcc: " + joined(bcc) + "\r\n";   // a draft keeps its Bcc; a sent message never has it
    if (auto r = one_line(field(m, "reply_to")); !r.empty()) msg += "Reply-To: " + r + "\r\n";
    if (auto r = one_line(field(m, "in_reply_to")); !r.empty()) msg += "In-Reply-To: " + r + "\r\n";
    if (auto r = one_line(field(m, "references")); !r.empty()) msg += "References: " + r + "\r\n";
    msg += "Subject: " + header_text(one_line(field(m, "subject"))) + "\r\n";
    msg += "Message-ID: <" + crypto::hex_encode(crypto::random_bytes(12)) + "@" + domain + ">\r\n";
    msg += "MIME-Version: 1.0\r\n";
    auto boundary = [] { return "lux-" + crypto::hex_encode(crypto::random_bytes(12)); };
    // Attachments first: those with a "cid" are images the HTML points at (multipart/related with the HTML),
    // the rest travel beside the body (multipart/mixed).
    std::vector<std::string> inline_parts, normal_parts;
    auto att = m.find("attachments");
    if (att != m.end() && att->second.is_list()) {
        size_t total = 0;
        for (const Value& x : att->second.as_list()) {
            std::string part;
            bool is_inline = false;
            if (!attachment_part(x, part, total, error, is_inline)) return false;
            (is_inline && !html.empty() ? inline_parts : normal_parts).push_back(std::move(part));
        }
    }
    std::string html_part;   // its own headers, then content
    if (!html.empty()) {
        html_part = body_part("text/html", html);
        if (!inline_parts.empty()) {
            const std::string r = boundary();
            html_part = "Content-Type: multipart/related; boundary=\"" + r + "\"\r\n\r\n--" + r + "\r\n" + html_part;
            for (const auto& p : inline_parts) html_part += "--" + r + "\r\n" + p;
            html_part += "--" + r + "--\r\n";
        }
    }
    std::string body;   // its own headers, then content
    if (!text.empty() && !html.empty()) {
        const std::string b = boundary();
        body = "Content-Type: multipart/alternative; boundary=\"" + b + "\"\r\n\r\n--" + b + "\r\n" +
               body_part("text/plain", text) + "--" + b + "\r\n" + html_part + "--" + b + "--\r\n";
    } else {
        body = html.empty() ? body_part("text/plain", text) : html_part;
    }
    if (!normal_parts.empty()) {
        const std::string b = boundary();
        msg += "Content-Type: multipart/mixed; boundary=\"" + b + "\"\r\n\r\n--" + b + "\r\n" + body;
        for (const auto& p : normal_parts) msg += "--" + b + "\r\n" + p;
        msg += "--" + b + "--\r\n";
    } else {
        msg += body;
    }
    for (const auto* list : {&to, &cc, &bcc}) rcpts.insert(rcpts.end(), list->begin(), list->end());
    return true;
}

// The finished message as text (bcc left out), e.g. to append it to the Sent folder.
Value fn_compose(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const auto& m = a[0].as_dict();
    Config cfg;
    if (!resolve_config(m, "mail.compose", cfg, error)) return Value::null();
    std::string msg, env_from;
    std::vector<std::string> rcpts;
    if (!build_message(m, cfg, "mail.compose", msg, env_from, rcpts, error)) return Value::null();
    return Value::str(std::move(msg));
}

Value fn_send(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const auto& m = a[0].as_dict();
    Config cfg;
    if (!resolve_config(m, "mail.send", cfg, error)) return Value::null();
    std::string msg, env_from;
    std::vector<std::string> rcpts;
    if (!build_message(m, cfg, "mail.send", msg, env_from, rcpts, error)) return Value::null();

    CURL* curl = lux_curl_init();
    if (!curl) { error = "mail.send(): could not initialize"; return Value::null(); }
    curl_slist* rcpt = nullptr;
    for (const auto& r : rcpts) rcpt = curl_slist_append(rcpt, envelope(r).c_str());
    Upload up{std::move(msg)};
    curl_easy_setopt(curl, CURLOPT_URL, cfg.url.c_str());
    if (cfg.tls == "starttls") curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    if (!cfg.user.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.user.c_str());
        if (!cfg.token.empty()) curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, cfg.token.c_str());
        else curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, env_from.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, feed);
    curl_easy_setopt(curl, CURLOPT_READDATA, &up);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30'000L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { error = std::string("mail.send(): ") + curl_easy_strerror(rc); return Value::null(); }
    return Value::boolean(true);
}

// Connects, negotiates TLS and logs in to the smtp Dict's server, then says NOOP and hangs up: nothing is sent.
// Returns true, or fails with the reason (login refused, no connection...).
Value fn_check(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (!a[0].is_dict()) { error = "mail.check: the argument must be a Dict"; return Value::null(); }
    Value::Dict wrapper;
    wrapper["smtp"] = a[0];
    Config cfg;
    if (!resolve_config(wrapper, "mail.check", cfg, error)) return Value::null();
    CURL* curl = lux_curl_init();
    if (!curl) { error = "mail.check(): could not initialize"; return Value::null(); }
    char err[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, cfg.url.c_str());
    if (cfg.tls == "starttls") curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    if (!cfg.user.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.user.c_str());
        if (!cfg.token.empty()) curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, cfg.token.c_str());
        else curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "NOOP");
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20'000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc == CURLE_LOGIN_DENIED) { error = "mail.check(): login failed (wrong user or password)"; return Value::null(); }
    if (rc != CURLE_OK) { error = std::string("mail.check(): ") + (err[0] ? err : curl_easy_strerror(rc)); return Value::null(); }
    return Value::boolean(true);
}

class MailModule : public BuiltinModule {
public:
    const char* name() const override { return "mail"; }
    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"send", "d>b", fn_send, /*is_async=*/true},
            {"compose", "d>s", fn_compose},
            {"check", "d>b", fn_check, /*is_async=*/true},
        };
        return fns;
    }
    bool configure(const std::map<std::string, std::string>& o, std::string& error) override {
        auto get = [&](const char* k, const char* def) { auto it = o.find(k); return it == o.end() ? std::string(def) : it->second; };
        Config& c = config();
        const std::string host = get("host", "");
        if (host.empty() && o.empty()) return true;   // imported with no block: send() says so
        c.tls = get("tls", "starttls");
        if (c.tls != "starttls" && c.tls != "tls" && c.tls != "none") { error = "mail: tls is \"starttls\", \"tls\" or \"none\""; return false; }
        c.from = get("from", "");
        if (host.empty() || c.from.empty()) { error = "mail: the block needs host and from"; return false; }
        c.url = std::string(c.tls == "tls" ? "smtps://" : "smtp://") + host + ":" +
                get("port", c.tls == "tls" ? "465" : "587");
        c.user = get("user", "");
        c.password = get("password", "");
        c.configured = true;
        return true;
    }
};

} // namespace

LUX_REGISTER_MODULE(MailModule)

} // namespace lux_script
