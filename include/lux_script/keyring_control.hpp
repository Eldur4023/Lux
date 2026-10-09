#pragma once
#include <functional>
#include <optional>
#include <string>

namespace lux_script {

// Optional replacement for the `keyring` module's default backend (secret-tool). The Android shell
// installs these (src/android.cpp -> Android Keystore); if `get` is unset the default is used.
// Hooks run on blocking_pool threads (the module functions are `is_async`) and may block.
struct KeyringControl {
    std::function<std::optional<std::string>(const std::string& service, const std::string& key)> get;
    std::function<bool(const std::string& service, const std::string& key, const std::string& secret)> set;
    std::function<bool(const std::string& service, const std::string& key)> del;
};

KeyringControl& keyring_control();

} // namespace lux_script
