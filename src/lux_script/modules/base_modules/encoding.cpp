// Text encodings a backend meets every day -- Python's base64, binascii,
// urllib.parse and html.escape in one place, over helpers the framework
// already had (crypto.hpp, percent_encoding.hpp, the template escaper).
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>
#include <lux_script/template.hpp>
#include <lux/percent_encoding.hpp>

#include <algorithm>

namespace lux_script {

namespace {

// base64_encode(s) is standard and padded; base64_encode(s, true) is the
// URL-safe, unpadded variant. The decoder takes either.
Value fn_base64_encode(NativeCtx&, std::vector<Value>& a, std::string&) {
    const bool url = a.size() > 1 && a[1].as_bool();
    return Value::str(url ? crypto::base64url_encode(a[0].as_str()) : crypto::base64_encode(a[0].as_str()));
}

Value fn_base64_decode(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::string out;
    if (!crypto::base64url_decode(a[0].as_str(), out)) { error = "encoding.base64_decode(): invalid base64"; return Value::null(); }
    return Value::str(std::move(out));
}

Value fn_hex_encode(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::str(crypto::hex_encode(a[0].as_str()));
}

Value fn_hex_decode(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string& s = a[0].as_str();
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; };
    std::string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out += static_cast<char>(hi << 4 | lo);
    }
    if (out.size() * 2 != s.size()) { error = "encoding.hex_decode(): invalid hex"; return Value::null(); }
    return Value::str(std::move(out));
}

Value fn_url_encode(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::str(lux::percent_encode(a[0].as_str()));
}

// '+' stays a '+': that folding belongs to query strings (query_decode).
Value fn_url_decode(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::str(lux::percent_decode(a[0].as_str(), false));
}

// {"q": "a b", "tag": ["x", "y"]} -> "q=a%20b&tag=x&tag=y". null is skipped.
Value fn_query_encode(NativeCtx&, std::vector<Value>& a, std::string&) {
    std::string out;
    auto pair = [&](const std::string& k, const Value& v) {
        if (v.is_null()) return;
        if (!out.empty()) out += '&';
        out += lux::percent_encode(k) + "=" + lux::percent_encode(v.to_string());
    };
    for (const auto& [k, v] : a[0].as_dict()) {
        if (v.is_list()) for (const Value& x : v.as_list()) pair(k, x);
        else pair(k, v);
    }
    return Value::str(std::move(out));
}

// "?a=1&b=x+y" -> {"a": "1", "b": "x y"}; values stay strings.
Value fn_query_decode(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& s = a[0].as_str();
    Value::Dict d;
    lux::for_each_form_pair(s.substr(!s.empty() && s[0] == '?'),
                            [&](const std::string& k, std::string v) { d[k] = Value::str(std::move(v)); });
    return Value::dict(std::move(d));
}

Value fn_html_escape(NativeCtx&, std::vector<Value>& a, std::string&) {
    std::string out;
    escape_html(a[0].as_str(), out);
    return Value::str(std::move(out));
}

// "https://u@a.com:8080/p/x?q=1#top" -> scheme, host, port (int or null),
// path, query (a Dict), fragment. A relative URL ("/next?x=1") has an empty
// scheme and host: the check that keeps a `?next=` redirect on this site.
// Parsed the way a browser reads it -- leading spaces and controls dropped,
// '\\' taken as '/' -- or "/\\evil.com" would look relative and still leave.
Value fn_url_parse(NativeCtx& c, std::vector<Value>& a, std::string& e) {
    std::string s = a[0].as_str();
    s.erase(0, std::find_if(s.begin(), s.end(), [](unsigned char ch) { return ch > 0x20; }) - s.begin());
    std::replace(s.begin(), s.end(), '\\', '/');
    auto cut = [&](char sep) {
        const size_t i = s.find(sep);
        std::string tail = i == std::string::npos ? "" : s.substr(i + 1);
        if (i != std::string::npos) s.resize(i);
        return tail;
    };
    Value fragment = Value::str(cut('#'));
    std::vector<Value> q{Value::str(cut('?'))};
    Value query = fn_query_decode(c, q, e);

    std::string scheme, host;
    Value port = Value::null();
    if (const size_t i = s.find("://"); i != std::string::npos && s.find('/') > i) {
        scheme = s.substr(0, i);
        s.erase(0, i + 3);
    }
    if (!scheme.empty() || s.rfind("//", 0) == 0) {
        if (scheme.empty()) s.erase(0, 2);
        const size_t slash = s.find('/');
        host = s.substr(0, slash);
        s = slash == std::string::npos ? "" : s.substr(slash);
        if (const size_t at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
        if (const size_t colon = host.rfind(':'); colon != std::string::npos && host.find(']', colon) == std::string::npos) {
            port = Value::integer(std::atoll(host.c_str() + colon + 1));
            host.resize(colon);
        }
    }
    Value::Dict d;
    d["scheme"]   = Value::str(scheme);
    d["host"]     = Value::str(host);
    d["port"]     = port;
    d["path"]     = Value::str(s);
    d["query"]    = query;
    d["fragment"] = fragment;
    return Value::dict(std::move(d));
}

} // namespace

LUX_MODULE(encoding, {
    {"base64_encode", "s|b>s", fn_base64_encode},
    {"base64_decode", "s>s",   fn_base64_decode},
    {"hex_encode",    "s>s",   fn_hex_encode},
    {"hex_decode",    "s>s",   fn_hex_decode},
    {"url_encode",    "s>s",   fn_url_encode},
    {"url_decode",    "s>s",   fn_url_decode},
    {"query_encode",  "d>s",   fn_query_encode},
    {"query_decode",  "s>d",   fn_query_decode},
    {"url_parse",     "s>d",   fn_url_parse},
    {"html_escape",   "s>s",   fn_html_escape},
})

} // namespace lux_script
