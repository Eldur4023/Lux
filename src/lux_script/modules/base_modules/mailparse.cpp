// Reading a raw RFC 822 message (what imap.fetch returns) into plain fields.
//
//     Json m = mailparse.parse(raw)
//     # {subject, from, from_email, to, cc, bcc, date (unix, 0 if unreadable), message_id, in_reply_to,
//     #  references, text, html, attachments: [{name, type, size, cid, part}]}
//     string data = mailparse.attachment(raw, 2)   # the decoded bytes of attachments[i].part
//
// Everything comes back as UTF-8: RFC 2047 headers, base64 / quoted-printable bodies and the
// declared charsets (iconv) are decoded. `text` / `html` are the first of each kind found.
// ponytail: addresses stay as the decoded header text (from_email is the first address);
// a proper address-list parse (groups, comments) when something needs it.
#include <lux_script/builtin_module.hpp>

#include <curl/curl.h>   // curl_getdate: RFC 822 dates, already linked with the http module
#include <iconv.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>

namespace lux_script {

namespace {

constexpr int kMaxDepth = 8;
constexpr size_t kMaxInlineImageBytes = 1536 * 1024;   // larger inline images are not embedded in the HTML   // nested multiparts: a hostile message cannot recurse forever

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string to_utf8(const std::string& in, const std::string& charset) {
    const std::string cs = lower(charset);
    if (cs.empty() || cs == "utf-8" || cs == "utf8" || cs == "us-ascii") return in;
    iconv_t cd = iconv_open("UTF-8//TRANSLIT", cs.c_str());
    if (cd == reinterpret_cast<iconv_t>(-1)) return in;
    std::string out(in.size() * 4 + 8, '\0');
    char* ip = const_cast<char*>(in.data());
    char* op = out.data();
    size_t il = in.size(), ol = out.size();
    while (il > 0 && iconv(cd, &ip, &il, &op, &ol) == static_cast<size_t>(-1)) {
        if (errno != EILSEQ && errno != EINVAL) break;
        if (ol < 4) break;
        ++ip; --il; *op++ = '?'; --ol;   // skip the bad byte
    }
    iconv_close(cd);
    out.resize(static_cast<size_t>(op - out.data()));
    return out;
}

std::string decode_qp(const std::string& s, bool header) {
    std::string out;
    auto hexv = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::tolower(c) - 'a' + 10; };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '=' && i + 1 < s.size() && (s[i + 1] == '\n' || s[i + 1] == '\r')) {   // soft line break
            i += (s[i + 1] == '\r' && i + 2 < s.size() && s[i + 2] == '\n') ? 2 : 1;
        } else if (s[i] == '=' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        } else {
            out += header && s[i] == '_' ? ' ' : s[i];
        }
    }
    return out;
}

std::string decode_b64(const std::string& s) {
    std::string clean;
    for (char c : s) if (!std::isspace(static_cast<unsigned char>(c))) clean += c;
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (unsigned char c : clean) {
        const char* p = std::strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", c);
        if (c == '=' ) break;
        if (!p || !c) continue;   // junk between groups is skipped
        acc = (acc << 6) | static_cast<unsigned>(p - "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
        if ((bits += 6) >= 8) { bits -= 8; out += static_cast<char>((acc >> bits) & 0xFF); }
    }
    return out;
}

// "=?UTF-8?B?...?=" words inside a header value (adjacent encoded words join without the space).
std::string decode_words(const std::string& v) {
    std::string out;
    size_t i = 0;
    bool prev_encoded = false;
    while (i < v.size()) {
        const size_t s = v.find("=?", i);
        size_t q1 = s == std::string::npos ? s : v.find('?', s + 2);
        size_t q2 = q1 == std::string::npos ? q1 : v.find('?', q1 + 1);
        size_t e = q2 == std::string::npos ? q2 : v.find("?=", q2 + 1);
        if (e == std::string::npos || q2 != q1 + 2) { out += v.substr(i); break; }
        const std::string gap = v.substr(i, s - i);
        if (!(prev_encoded && trim(gap).empty())) out += gap;
        const std::string charset = v.substr(s + 2, q1 - s - 2), payload = v.substr(q2 + 1, e - q2 - 1);
        const char enc = static_cast<char>(std::tolower(static_cast<unsigned char>(v[q1 + 1])));
        out += to_utf8(enc == 'b' ? decode_b64(payload) : decode_qp(payload, true), charset.substr(0, charset.find('*')));
        prev_encoded = true;
        i = e + 2;
    }
    return out;
}

using Headers = std::multimap<std::string, std::string>;

// Splits at the first blank line; unfolds continuation lines.
void split_message(const std::string& raw, Headers& h, std::string& body) {
    size_t pos = 0, end = raw.size();
    std::string last_name;
    while (pos < end) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = end;
        std::string line = raw.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = std::min(eol + 1, end);
        if (line.empty()) break;
        if ((line[0] == ' ' || line[0] == '\t') && !last_name.empty()) {
            auto it = h.upper_bound(last_name);
            --it;
            it->second += " " + trim(line);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        last_name = lower(line.substr(0, colon));
        h.emplace(last_name, trim(line.substr(colon + 1)));
    }
    body = pos < end ? raw.substr(pos) : "";
}

std::string header(const Headers& h, const char* name) {
    auto it = h.find(name);
    return it == h.end() ? "" : it->second;
}

// A parameter of a structured header: boundary / charset / name / filename (also RFC 2231 filename*=).
std::string param(const std::string& value, const std::string& key) {
    const std::string v = lower(value);
    for (const std::string& k : {key + "*=", key + "="}) {
        size_t p = 0;
        while ((p = v.find(k, p)) != std::string::npos) {
            if (p > 0 && v[p - 1] != ';' && v[p - 1] != ' ' && v[p - 1] != '\t') { ++p; continue; }
            p += k.size();
            std::string r;
            if (p < value.size() && value[p] == '"') {
                const size_t q = value.find('"', p + 1);
                r = value.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
            } else {
                r = value.substr(p, value.find(';', p) - p);
            }
            r = trim(r);
            if (k.back() == '=' && k[k.size() - 2] == '*') {   // charset'lang'percent-encoded
                const size_t a = r.find('\''), b = a == std::string::npos ? a : r.find('\'', a + 1);
                if (b == std::string::npos) return r;
                std::string out;
                const std::string enc = r.substr(b + 1);
                for (size_t i = 0; i < enc.size(); ++i)
                    out += enc[i] == '%' && i + 2 < enc.size() ? static_cast<char>(std::stoi(enc.substr(i + 1, 2), nullptr, 16)) : enc[i],
                           i += enc[i] == '%' && i + 2 < enc.size() ? 2 : 0;
                return to_utf8(out, r.substr(0, a));
            }
            return r;
        }
    }
    return "";
}

struct Parsed {
    std::string text, html;
    Value::List attachments;
    std::vector<std::string> attachment_bytes;   // parallel to `attachments`, decoded
    bool want_bytes = false;
};

std::string decode_body(const std::string& body, const std::string& encoding) {
    const std::string e = lower(trim(encoding));
    return e == "base64" ? decode_b64(body) : e == "quoted-printable" ? decode_qp(body, false) : body;
}

void walk(const std::string& raw, Parsed& out, int depth) {
    Headers h;
    std::string body;
    split_message(raw, h, body);
    const std::string ctype = header(h, "content-type"), type = lower(trim(ctype.substr(0, ctype.find(';'))));
    const std::string disp = header(h, "content-disposition");

    if (type.rfind("multipart/", 0) == 0 && depth < kMaxDepth) {
        const std::string boundary = param(ctype, "boundary");
        if (boundary.empty()) return;
        const std::string delim = "--" + boundary;
        size_t pos = body.find(delim);
        while (pos != std::string::npos) {
            pos += delim.size();
            if (body.compare(pos, 2, "--") == 0) break;
            pos = body.find('\n', pos);
            if (pos == std::string::npos) break;
            ++pos;
            const size_t next = body.find("\n" + delim, pos);
            std::string part = body.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            if (!part.empty() && part.back() == '\r') part.pop_back();
            walk(part, out, depth + 1);
            pos = next == std::string::npos ? next : next + 1;
        }
        return;
    }
    const std::string data = decode_body(body, header(h, "content-transfer-encoding"));
    const std::string name = decode_words(param(disp, "filename").empty() ? param(ctype, "name") : param(disp, "filename"));
    const bool is_attachment = lower(disp).rfind("attachment", 0) == 0 || !name.empty() || (type != "text/plain" && type != "text/html" && !type.empty());
    if (!is_attachment && (type == "text/plain" || type == "text/html" || type.empty())) {
        std::string& slot = type == "text/html" ? out.html : out.text;
        if (slot.empty()) slot = to_utf8(data, param(ctype, "charset"));
        return;
    }
    Value::Dict a;
    a["name"] = Value::str(name.empty() ? "attachment" : name);
    a["type"] = Value::str(type.empty() ? "application/octet-stream" : type);
    a["size"] = Value::integer(static_cast<long long>(data.size()));
    std::string cid = header(h, "content-id");
    if (cid.size() > 1 && cid.front() == '<') cid = cid.substr(1, cid.size() - 2);
    a["cid"] = Value::str(cid);
    a["part"] = Value::integer(static_cast<long long>(out.attachments.size()));
    out.attachments.push_back(Value::dict(std::move(a)));
    // Bytes are kept for mailparse.attachment (want_bytes), and for small inline images so parse can embed them.
    const bool small_inline_image = !cid.empty() && type.rfind("image/", 0) == 0 && data.size() <= kMaxInlineImageBytes;
    out.attachment_bytes.push_back(out.want_bytes || small_inline_image ? data : std::string());
}

std::string b64_standard(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; out += t[(acc >> bits) & 63]; }
    }
    if (bits) out += t[(acc << (6 - bits)) & 63];
    while (out.size() % 4) out += '=';
    return out;
}

// html with every `cid:<id>` of an inline image replaced by a data: URI, so it displays without the message.
std::string embed_cid_images(std::string html, const Value::List& atts, const std::vector<std::string>& bytes) {
    for (size_t i = 0; i < atts.size() && i < bytes.size(); ++i) {
        const auto& d = atts[i].as_dict();
        auto cid = d.find("cid"), type = d.find("type");
        if (bytes[i].empty() || cid == d.end() || cid->second.as_str().empty()) continue;
        const std::string needle = "cid:" + cid->second.as_str(), uri = "data:" + type->second.as_str() + ";base64," + b64_standard(bytes[i]);
        for (size_t pos = 0; (pos = html.find(needle, pos)) != std::string::npos; pos += uri.size()) html.replace(pos, needle.size(), uri);
    }
    return html;
}

Value fn_parse(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (!a[0].is_str()) { error = "mailparse.parse(): the message must be a string"; return Value::null(); }
    const std::string& raw = a[0].as_str();
    Headers h;
    std::string body;
    split_message(raw, h, body);
    Parsed p;
    walk(raw, p, 0);

    const std::string from = decode_words(header(h, "from"));
    std::string email = from;
    const size_t lt = from.find('<'), gt = from.find('>', lt);
    if (lt != std::string::npos && gt != std::string::npos) email = from.substr(lt + 1, gt - lt - 1);
    const time_t ts = curl_getdate(header(h, "date").c_str(), nullptr);

    Value::Dict d;
    d["subject"] = Value::str(decode_words(header(h, "subject")));
    d["from"] = Value::str(from);
    d["from_email"] = Value::str(lower(trim(email)));
    d["to"] = Value::str(decode_words(header(h, "to")));
    d["cc"] = Value::str(decode_words(header(h, "cc")));
    d["bcc"] = Value::str(decode_words(header(h, "bcc")));   // only present in drafts
    d["date"] = Value::integer(ts < 0 ? 0 : static_cast<long long>(ts));
    d["message_id"] = Value::str(header(h, "message-id"));
    d["in_reply_to"] = Value::str(header(h, "in-reply-to"));
    d["references"] = Value::str(header(h, "references"));
    d["text"] = Value::str(p.text);
    d["html"] = Value::str(embed_cid_images(p.html, p.attachments, p.attachment_bytes));
    d["html_cid"] = Value::str(p.html);   // the original, still pointing at cid:... (to edit a draft)
    d["attachments"] = Value::list(std::move(p.attachments));
    return Value::dict(std::move(d));
}

Value fn_attachment(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (!a[0].is_str() || !a[1].is_int()) { error = "mailparse.attachment(): (message string, part int)"; return Value::null(); }
    Parsed p;
    p.want_bytes = true;
    walk(a[0].as_str(), p, 0);
    const long long i = a[1].as_int();
    if (i < 0 || i >= static_cast<long long>(p.attachment_bytes.size())) { error = "mailparse.attachment(): no such part"; return Value::null(); }
    return Value::str(p.attachment_bytes[static_cast<size_t>(i)]);
}

class MailParseModule : public BuiltinModule {
public:
    const char* name() const override { return "mailparse"; }
    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"parse", "s>d", fn_parse},
            {"attachment", "si>s", fn_attachment},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(MailParseModule)

} // namespace lux_script
