#include <lux_script/luxp_serve.hpp>

#include <lux/logger.hpp>
#include <lux/request.hpp>
#include <lux/response.hpp>
#include "luxp_internal.hpp"
#include <html/parser.hpp>
#include <page/doc_codec.hpp>
#include <page/luxp.hpp>
#include <script/bytecode_io.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace lux_script {

namespace {

namespace fs = std::filesystem;
using luxium::page::Blob;
using luxium::page::Translation;
using luxium::page::blob_deflate;
using luxium::page::blob_plain;
using luxium::page::PageSections;
namespace li = luxp_internal;

std::atomic<std::shared_ptr<const std::vector<StaticMount>>> g_statics{
    std::make_shared<const std::vector<StaticMount>>()};

std::optional<std::string> read_disk(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A URL as the browser would request it (relative to the page's path) -> the
// file a static mount serves for it, never outside the mount's root.  `file`
// gets the path that answered (so a cache can notice when it changes).
std::optional<std::string> fetch_static(const std::string& url, const std::string& page_path, fs::path* file_out) {
    std::string abs = url;
    if (abs.empty() || abs.find(':') != std::string::npos) return std::nullopt;   // http:, data:...
    if (abs[0] != '/') {
        const size_t slash = page_path.rfind('/');
        abs = (slash == std::string::npos ? "/" : page_path.substr(0, slash + 1)) + abs;
    }
    abs = fs::path(abs.substr(0, abs.find_first_of("?#"))).lexically_normal().generic_string();
    const auto mounts = g_statics.load();
    for (const auto& m : *mounts) {
        std::string prefix = m.url_prefix;
        if (!prefix.ends_with('/')) prefix += '/';
        if (!abs.starts_with(prefix)) continue;
        const fs::path root = fs::path(m.fs_root).lexically_normal();
        const fs::path file = (root / abs.substr(prefix.size())).lexically_normal();
        if (file.lexically_relative(root).generic_string().starts_with("..")) return std::nullopt;
        if (auto bytes = read_disk(file)) {
            if (file_out) *file_out = file;
            return bytes;
        }
    }
    return std::nullopt;
}

constexpr const char* kUsesJavaScript = "the page uses JavaScript";

// ── what translating a page repeatedly keeps ────────────────────────────────
// The stylesheet and the program are what the page's <style>/<link> and
// <script> say, whatever the data is.  Cached by that source; a sheet is also
// dropped when a file it came from changes (checked at most once a second).
// ponytail: bounded by clearing, not LRU -- these only fill up if every
// request carries a different script or stylesheet.

struct ProgramEntry {
    Blob        blob;        // empty: no script
    std::string error;       // non-empty: the page is HTML (JavaScript, or LuxScript that does not compile)
};

struct SheetEntry {
    std::shared_ptr<const li::SheetData> data;
    int64_t checked = 0;
};

std::mutex g_cache_m;
std::unordered_map<std::string, std::shared_ptr<const ProgramEntry>> g_programs;
std::unordered_map<std::string, SheetEntry> g_sheets;
constexpr size_t kCacheEntries = 256;

bool typed_luxscript(luxium::dom::Node& doc) {
    for (const auto* s : doc.find_all("script"))
        if (s->attr_or("type").find("luxscript") != std::string::npos) return true;
    return false;
}

std::shared_ptr<const ProgramEntry> program_for(luxium::dom::Node& doc) {
    const std::string key = luxium::page::scripts_key(doc);
    {
        std::lock_guard<std::mutex> lk(g_cache_m);
        auto it = g_programs.find(key);
        if (it != g_programs.end()) return it->second;
    }
    auto entry = std::make_shared<ProgramEntry>();
    luxium::page::ScriptsResult r = luxium::page::translate_scripts(doc);
    if (r.uses_javascript) {
        entry->error = kUsesJavaScript;
    } else if (!r.errors.empty()) {
        // A bare <script> that does not compile is the web's JavaScript (the
        // same call Luxium makes); only an explicit text/luxscript is an error.
        if (typed_luxscript(doc)) {
            entry->error = "LuxScript of the page:";
            for (const auto& e : r.errors) entry->error += "\n  " + e;
        } else {
            entry->error = kUsesJavaScript;
        }
    } else if (r.program) {
        entry->blob = blob_deflate(luxium::script::serialize(*r.program));
    }
    std::lock_guard<std::mutex> lk(g_cache_m);
    if (g_programs.size() >= kCacheEntries) g_programs.clear();
    g_programs[key] = entry;
    return entry;
}

std::shared_ptr<const li::SheetData> sheet_for(luxium::dom::Node& doc, const std::string& url_path) {
    const size_t slash = url_path.rfind('/');
    const std::string dir = slash == std::string::npos ? "/" : url_path.substr(0, slash + 1);
    const std::string key = luxium::page::styles_key(doc) + '\1' + dir;
    const int64_t now = li::now_ms();
    {
        std::lock_guard<std::mutex> lk(g_cache_m);
        auto it = g_sheets.find(key);
        if (it != g_sheets.end()) {
            if (now - it->second.checked < 1000) return it->second.data;
            if (!li::sheet_stale(*it->second.data)) { it->second.checked = now; return it->second.data; }
            g_sheets.erase(it);
        }
    }
    auto data = li::build_sheet(doc, url_path);
    std::lock_guard<std::mutex> lk(g_cache_m);
    if (g_sheets.size() >= kCacheEntries) g_sheets.clear();
    g_sheets[key] = {data, now};
    return data;
}

} // namespace

namespace luxp_internal {

std::shared_ptr<const SheetData> build_sheet(luxium::dom::Node& doc, const std::string& url_path, bool* relative_used) {
    auto data = std::make_shared<SheetData>();
    luxium::page::StylesResult styles = luxium::page::translate_styles(doc, [&](const std::string& url) {
        if (relative_used && (url.empty() || url[0] != '/') && url.find(':') == std::string::npos) *relative_used = true;
        fs::path file;
        auto bytes = fetch_static(url, url_path, &file);
        if (bytes) data->deps.emplace_back(file, fs::last_write_time(file));
        return bytes;
    });
    data->plain = luxium::page::encode_sheet(styles.rules);
    data->blob = blob_deflate(data->plain);
    return data;
}

bool sheet_stale(const SheetData& s) {
    std::error_code ec;
    for (const auto& [file, stamp] : s.deps) {
        const auto now = fs::last_write_time(file, ec);
        if (ec || now != stamp) return true;
    }
    return false;
}

} // namespace luxp_internal

std::optional<std::string> luxp_fetch_static(const std::string& url, const std::string& page_path) {
    return fetch_static(url, page_path, nullptr);
}

std::vector<std::string> check_page_scripts(const std::string& template_source) {
    // Only the <script> bodies matter here; the rest of a template is not
    // HTML yet ({{ }} anywhere), so the page itself is not translated.
    const auto parsed = luxium::html::parse(template_source);
    std::vector<std::pair<std::string, int>> blocks;
    for (luxium::dom::Node* s : parsed.root->find_all("script")) {
        // Only an explicit LuxScript script is an error the project owns: a
        // bare <script> on the web is JavaScript, and that page is simply
        // always answered as HTML.
        if (s->attr_or("type").find("luxscript") == std::string::npos || s->has_attr("src")) continue;
        const std::string body = s->text_content();
        if (body.find("{{") != std::string::npos || body.find("{%") != std::string::npos)
            return {};   // interpolated: the whole program can only compile once rendered
        blocks.emplace_back(body, s->line);
    }
    if (blocks.empty()) return {};
    auto prog = luxium::script::compile_blocks(blocks, "page");
    return prog->errors;
}

bool wants_luxp(const lux::Request& req) {
    const std::string* accept = req.header("accept");
    return accept && accept->find(luxium::page::kLuxpMime) != std::string::npos;
}

void set_luxp_statics(const std::vector<StaticMount>& mounts) {
    g_statics.store(std::make_shared<const std::vector<StaticMount>>(mounts));
    std::lock_guard<std::mutex> lk(g_cache_m);   // what was built from the old mounts is stale
    g_programs.clear();
    g_sheets.clear();
}

std::optional<std::string> html_to_luxp(const std::string& html, const std::string& url_path,
                                        std::string* err) {
    luxium::page::ParsedPage page = luxium::page::parse_page(html);
    luxium::dom::Node& doc = *page.document;

    auto program = program_for(doc);
    if (!program->error.empty()) {
        if (err) *err = program->error;
        return std::nullopt;
    }
    auto sheet = sheet_for(doc, url_path);
    return luxium::page::encode_single_page("index.html", blob_plain(luxium::page::encode_document(doc)),
                                            sheet->blob, program->blob);
}

void send_page(const lux::Request& req, lux::Response& res, const std::string& html, bool allow_luxp) {
    res.header("Vary", "Accept");   // one URL, two representations
    if (allow_luxp && wants_luxp(req)) {
        std::string err;
        if (auto luxp = html_to_luxp(html, req.path, &err)) {
            res.header("Content-Type", luxium::page::kLuxpMime).send(std::move(*luxp));
            return;
        }
        if (err != kUsesJavaScript) lux::log().warn("luxp: ", req.path, " answered as HTML: ", err);
    }
    res.header("Content-Type", "text/html; charset=utf-8").send(html);
}

bool pack_site(const std::string& entry_html, const std::string& out_path, std::string* report) {
    auto fail = [&](const std::string& why) { *report = why; return false; };
    // Anything outside the working directory stays out: packing someone
    // else's page must not sweep "../../.ssh/id_rsa" into the bundle.
    const fs::path cwd = fs::current_path();
    auto absn = [](const fs::path& p) { return fs::absolute(p).lexically_normal(); };
    auto inside_cwd = [&](const fs::path& p) {
        return !absn(p).lexically_relative(cwd).generic_string().starts_with("..");
    };

    struct Page { fs::path path; PageSections s; };
    std::vector<Page> pages;
    std::vector<std::pair<fs::path, std::string>> resources;
    std::set<fs::path> seen;
    std::vector<fs::path> todo{absn(entry_html)};

    while (!todo.empty()) {
        const fs::path p = todo.front();
        todo.erase(todo.begin());
        if (!seen.insert(p).second) continue;
        if (!inside_cwd(p)) return fail("page outside the working directory: " + p.string());
        auto html = read_disk(p);
        if (!html) return fail("cannot read " + p.string());

        Translation t = luxium::page::translate_html(*html, [&](const std::string& url) {
            return read_disk(p.parent_path() / url);
        });
        if (!t.errors.empty()) {
            std::string why = p.string() + ": LuxScript";
            for (const auto& e : t.errors) why += "\n  " + e;
            return fail(why);
        }
        if (t.uses_javascript) return fail(p.string() + ": uses JavaScript; a .luxp runs LuxScript only");
        for (const auto& w : t.warnings)
            if (w.starts_with("stylesheet not found")) return fail(p.string() + ": " + w);

        Page out{p, luxium::page::encode_page(t)};
        // Everything written must load: decode it back with the browser's
        // own verifiers.  A failure here is a bug in an encoder.
        std::string why;
        std::vector<luxium::style::Rule> rules;
        if (!luxium::page::decode_document(out.s.document, &why) ||
            !luxium::page::decode_sheet(out.s.sheet, rules, &why) ||
            (!out.s.program.empty() && !luxium::script::load_bytecode(out.s.program, &why)))
            return fail(p.string() + ": compiled output does not verify (" + why + ")");

        for (const auto& r : t.resources) {
            const fs::path rp = (p.parent_path() / r).lexically_normal();
            if (!seen.insert(rp).second) continue;
            if (!inside_cwd(rp)) return fail("resource outside the working directory: " + rp.string());
            auto bytes = read_disk(rp);
            if (!bytes) return fail("resource not found: " + rp.string());
            resources.emplace_back(rp, std::move(*bytes));
        }
        for (const auto& l : t.links) todo.push_back((p.parent_path() / l).lexically_normal());
        pages.push_back(std::move(out));
    }

    // Names are relative to the deepest folder holding everything, so a page
    // in paint/ using ../assets/x.png keeps that exact relative path.
    fs::path root = pages[0].path.parent_path();
    auto widen = [&](const fs::path& p) {
        while (p.lexically_relative(root).generic_string().starts_with("..")) root = root.parent_path();
    };
    for (const auto& pg : pages) widen(pg.path);
    for (const auto& [rp, _] : resources) widen(rp);

    luxium::page::Container c;
    for (auto& pg : pages) {
        c.pages.push_back({pg.path.lexically_relative(root).generic_string(), std::move(pg.s)});
        *report += "  page      " + c.pages.back().first + "\n";
    }
    for (auto& [rp, bytes] : resources) {
        c.resources.push_back({rp.lexically_relative(root).generic_string(), std::move(bytes)});
        *report += "  resource  " + c.resources.back().first + "\n";
    }
    const std::string bytes = luxium::page::encode_container(c);
    std::ofstream f(out_path, std::ios::binary);
    if (!f.write(bytes.data(), std::streamsize(bytes.size()))) return fail("cannot write " + out_path);
    *report += out_path + ": " + std::to_string(c.pages.size()) + " page(s), " +
               std::to_string(c.resources.size()) + " resource(s), " + std::to_string(bytes.size()) + " bytes\n";
    return true;
}

} // namespace lux_script
