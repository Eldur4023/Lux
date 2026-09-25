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

#include <algorithm>
#include <fstream>
#include <iterator>
#include <cstring>
#include <ctime>

namespace lux_script {

namespace {

struct Config {
    std::string url, user, password, from, tls = "starttls";
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
bool attachment_part(const Value& a, std::string& part, size_t& total, std::string& error) {
    std::string name, content;
    if (a.is_dict()) {
        name = field(a.as_dict(), "name");
        content = field(a.as_dict(), "content");
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
    part = "Content-Type: " + std::string(lux::mime_for_ext(dot)) + "\r\nContent-Transfer-Encoding: base64\r\n"
           "Content-Disposition: attachment; " + disp + "\r\n\r\n" + wrapped_base64(content);
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

Value fn_send(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const Config& cfg = config();
    if (!cfg.configured) { error = "mail.send(): no mail: block (host, from) in app:"; return Value::null(); }
    const auto& m = a[0].as_dict();
    const auto to = addresses(m, "to"), cc = addresses(m, "cc"), bcc = addresses(m, "bcc");
    const std::string text = field(m, "text"), html = field(m, "html");
    if (to.empty() && cc.empty() && bcc.empty()) { error = "mail.send(): no recipient (to, cc or bcc)"; return Value::null(); }
    if (text.empty() && html.empty()) { error = "mail.send(): no body (text or html)"; return Value::null(); }

    char date[64];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S +0000", &tm);
    const std::string from = one_line(field(m, "from").empty() ? cfg.from : field(m, "from"));
    const std::string env_from = envelope(from);
    const size_t at = env_from.find('@');
    const std::string domain = at == std::string::npos ? "lux" : env_from.substr(at + 1, env_from.size() - at - 2);

    std::string msg = "Date: " + std::string(date) + "\r\nFrom: " + from + "\r\n";
    if (!to.empty()) msg += "To: " + joined(to) + "\r\n";
    if (!cc.empty()) msg += "Cc: " + joined(cc) + "\r\n";
    if (auto r = one_line(field(m, "reply_to")); !r.empty()) msg += "Reply-To: " + r + "\r\n";
    msg += "Subject: " + header_text(one_line(field(m, "subject"))) + "\r\n";
    msg += "Message-ID: <" + crypto::hex_encode(crypto::random_bytes(12)) + "@" + domain + ">\r\n";
    msg += "MIME-Version: 1.0\r\n";
    auto boundary = [] { return "lux-" + crypto::hex_encode(crypto::random_bytes(12)); };
    std::string body;   // its own headers, then content
    if (!text.empty() && !html.empty()) {
        const std::string b = boundary();
        body = "Content-Type: multipart/alternative; boundary=\"" + b + "\"\r\n\r\n--" + b + "\r\n" +
               body_part("text/plain", text) + "--" + b + "\r\n" + body_part("text/html", html) + "--" + b + "--\r\n";
    } else {
        body = html.empty() ? body_part("text/plain", text) : body_part("text/html", html);
    }
    auto att = m.find("attachments");
    if (att != m.end() && att->second.is_list() && !att->second.as_list().empty()) {
        const std::string b = boundary();
        msg += "Content-Type: multipart/mixed; boundary=\"" + b + "\"\r\n\r\n--" + b + "\r\n" + body;
        size_t total = 0;
        for (const Value& x : att->second.as_list()) {
            std::string part;
            if (!attachment_part(x, part, total, error)) return Value::null();
            msg += "--" + b + "\r\n" + part;
        }
        msg += "--" + b + "--\r\n";
    } else {
        msg += body;
    }

    CURL* curl = curl_easy_init();
    if (!curl) { error = "mail.send(): could not initialize"; return Value::null(); }
    curl_slist* rcpt = nullptr;
    for (const auto* list : {&to, &cc, &bcc})
        for (const auto& r : *list) rcpt = curl_slist_append(rcpt, envelope(r).c_str());
    Upload up{std::move(msg)};
    curl_easy_setopt(curl, CURLOPT_URL, cfg.url.c_str());
    if (cfg.tls == "starttls") curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    if (!cfg.user.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());
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

class MailModule : public BuiltinModule {
public:
    const char* name() const override { return "mail"; }
    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"send", "d>b", fn_send, /*is_async=*/true},
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
