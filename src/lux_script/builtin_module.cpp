#include <lux_script/builtin_module.hpp>

namespace lux_script {

namespace detail {

std::vector<ModuleFactory>& module_factories() {
    // Meyer's singleton -- see builtin_module.hpp's comment on
    // LUX_REGISTER_MODULE for why this has to be a function-local static
    // and not a namespace-scope std::vector.
    static std::vector<ModuleFactory> factories;
    return factories;
}

} // namespace detail

// Every module that ended its .cpp with LUX_REGISTER_MODULE (base_modules/
// for the officially-shipped ones, or a user's own .cpp dropped next to
// that folder -- see NATIVE-MODULES.md) already pushed its factory into
// detail::module_factories() by the time this constructor runs: that list
// is what used to be a hand-written sequence of
// `{ Slot s; s.module = make_x_module(); slots_["x"] = std::move(s); }`
// lines here, one per module, each requiring an edit to THIS file. A
// module's own name() is what keys it in `slots_` -- the single source of
// truth `import` resolves against, never a second string (like the old
// `slots_["x"]` literal) that could silently drift from the class's own
// name() if one changed and the other did not.
BuiltinModuleRegistry::BuiltinModuleRegistry() {
    for (detail::ModuleFactory factory : detail::module_factories()) {
        Slot s;
        s.module = factory();
        std::string name = s.module->name();
        slots_[std::move(name)] = std::move(s);
    }

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

} // namespace lux_script
