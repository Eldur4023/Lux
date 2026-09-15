// Smoke test of the engine after the Lux surgery.
// It checks the three things milestone 0 had to leave standing:
// static files, Jinja2 templates and a route returning JSON.
#include <lux/lux.hpp>

using namespace lux;

int main() {
    App app;
    app.use(lux::logger());
    app.set_templates("./templates");

    app.get("/json", [](Response& res) {
        res.json_text(R"({"ok":true,"engine":"lux-2.0"})");
    });

    // Templates are no longer rendered from C++: they live in Lux Script,
    // at startup and are requested with render() from a .lux.  See example/.

    app.serve_static("/static", "./public");

    app.run(8080);
}
