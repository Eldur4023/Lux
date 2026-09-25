// IP address checks: allowlists (an admin route only from the VPN), trusted
// proxies, telling a private address from a public one. IPv4 and IPv6.
#include <lux_script/builtin_module.hpp>

#include "netaddr.hpp"

namespace lux_script {

namespace {

// ip_in(ip, "10.0.0.0/8") or ip_in(ip, ["10.0.0.0/8", "::1"]). An invalid
// ip is in nothing.
Value fn_ip_in(NativeCtx&, std::vector<Value>& a, std::string&) {
    netaddr::Addr ip;
    if (!netaddr::parse(a[0].as_str(), ip)) return Value::boolean(false);
    if (a[1].is_str()) return Value::boolean(netaddr::in_cidr(ip, a[1].as_str()));
    for (const Value& c : a[1].as_list())
        if (c.is_str() && netaddr::in_cidr(ip, c.as_str())) return Value::boolean(true);
    return Value::boolean(false);
}

Value fn_is_private(NativeCtx&, std::vector<Value>& a, std::string&) {
    netaddr::Addr ip;
    return Value::boolean(netaddr::parse(a[0].as_str(), ip) && netaddr::is_private(ip));
}

// 4, 6, or null for something that is not an IP address.
Value fn_ip_version(NativeCtx&, std::vector<Value>& a, std::string&) {
    netaddr::Addr ip;
    if (!netaddr::parse(a[0].as_str(), ip)) return Value::null();
    return Value::integer(netaddr::is_v4(ip) && a[0].as_str().find(':') == std::string::npos ? 4 : 6);
}

} // namespace

LUX_MODULE(net, {
    {"ip_in",      "sx", fn_ip_in},
    {"is_private", "s",  fn_is_private},
    {"ip_version", "s",  fn_ip_version},
})

} // namespace lux_script
