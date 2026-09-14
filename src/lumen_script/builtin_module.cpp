#include <lumen_script/builtin_module.hpp>

namespace lumen_script {

// Every compiled-in module is declared here, the same way DbRegistry (db.cpp)
// declares sqlite/postgres/mysql -- a plain factory call, `#ifdef`-gated for
// any module that carries an external dependency (hash carries none, so it
// is unconditional; see NATIVE-MODULES.md for a module that would need a
// cmake option).
std::unique_ptr<BuiltinModule> make_hash_module();
std::unique_ptr<BuiltinModule> make_csv_module();
std::unique_ptr<BuiltinModule> make_os_module();
std::unique_ptr<BuiltinModule> make_math_module();
std::unique_ptr<BuiltinModule> make_time_module();
#ifdef LUMEN_PDF
std::unique_ptr<BuiltinModule> make_pdf_module();
#endif
#ifdef LUMEN_HTTP
std::unique_ptr<BuiltinModule> make_http_module();
#endif

BuiltinModuleRegistry::BuiltinModuleRegistry() {
    { Slot s; s.module = make_hash_module(); slots_["hash"] = std::move(s); }
    { Slot s; s.module = make_csv_module();  slots_["csv"]  = std::move(s); }
    { Slot s; s.module = make_os_module();   slots_["os"]   = std::move(s); }
    { Slot s; s.module = make_math_module(); slots_["math"] = std::move(s); }
    { Slot s; s.module = make_time_module(); slots_["time"] = std::move(s); }
#ifdef LUMEN_PDF
    { Slot s; s.module = make_pdf_module();  slots_["pdf"]  = std::move(s); }
#endif
#ifdef LUMEN_HTTP
    { Slot s; s.module = make_http_module(); slots_["http"] = std::move(s); }
#endif

    build_flat_table();
}

void BuiltinModuleRegistry::build_flat_table() {
    for (const auto& [name, slot] : slots_)
        for (const auto& fn : slot.module->functions())
            flat_.push_back({name, fn});
}

BuiltinModuleRegistry& BuiltinModuleRegistry::instance() {
    static BuiltinModuleRegistry r;
    return r;
}

std::vector<std::string> BuiltinModuleRegistry::available() const {
    std::vector<std::string> out;
    for (const auto& [name, _] : slots_) out.push_back(name);
    return out;
}

bool BuiltinModuleRegistry::has(const std::string& name) const {
    return slots_.count(name) > 0;
}

bool BuiltinModuleRegistry::activate(const std::string& name,
                                    const std::map<std::string, std::string>& options,
                                    std::string& error) {
    auto it = slots_.find(name);
    if (it == slots_.end()) {
        error = "module '" + name + "' is not compiled into this binary";
        return false;
    }
    Slot& slot = it->second;
    if (slot.activated) return true;
    if (!slot.module->configure(options, error)) return false;
    slot.activated = true;
    return true;
}

bool BuiltinModuleRegistry::is_active(const std::string& name) const {
    auto it = slots_.find(name);
    return it != slots_.end() && it->second.activated;
}

const BuiltinModuleFn* BuiltinModuleRegistry::find(const std::string& module,
                                                  const std::string& function) const {
    auto it = slots_.find(module);
    if (it == slots_.end()) return nullptr;
    for (const auto& fn : it->second.module->functions())
        if (fn.name == function) return &fn;
    return nullptr;
}

int BuiltinModuleRegistry::id_of(const std::string& module, const std::string& function) const {
    for (size_t i = 0; i < flat_.size(); ++i)
        if (flat_[i].module == module && flat_[i].fn.name == function)
            return static_cast<int>(i);
    return -1;
}

const BuiltinModuleFn& builtin_module_function_at(int id) {
    return BuiltinModuleRegistry::instance().function_at(id);
}

} // namespace lumen_script
