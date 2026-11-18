// The non-.luxp half of luxp_serve.hpp: what a build without -DLUX_LUXP links instead of
// luxp_serve.cpp. Everything else in that header is an inline stub.
#include <lux_script/luxp_serve.hpp>

#include <lux/request.hpp>
#include <lux/response.hpp>

namespace lux_script {

void send_page(const lux::Request&, lux::Response& res, const std::string& html, bool) {
    res.header("Content-Type", "text/html; charset=utf-8").send(html);
}

} // namespace lux_script
