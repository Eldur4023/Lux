// The system keyring (GNOME Keyring / KWallet through the freedesktop Secret Service), by running
// `secret-tool` (package libsecret-tools). The secret travels over the child's stdin, never in
// its arguments, which any local user can read in /proc.
//
//     await keyring.set("lux-mail", "account-3", password)    # true
//     string s = await keyring.get("lux-mail", "account-3")   # "" if there is none
//     await keyring.delete("lux-mail", "account-3")
//
// `service` and `key` identify the entry (attributes "service" and "account" in secret-tool).
#include <lux_script/builtin_module.hpp>
#include <lux_script/keyring_control.hpp>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>

extern char** environ;

namespace lux_script {

KeyringControl& keyring_control() { static KeyringControl c; return c; }

namespace {

constexpr size_t kMaxSecretBytes = 64 * 1024;

// Runs secret-tool, feeds `input` to its stdin, returns its stdout. `ok` = exited 0.
std::string run(const std::vector<std::string>& args, const std::string& input, bool& ok, std::string& error, const char* fn) {
    ok = false;
    int in[2], out[2];
    if (pipe(in) != 0) { error = std::string(fn) + "(): pipe failed"; return ""; }
    if (pipe(out) != 0) { close(in[0]); close(in[1]); error = std::string(fn) + "(): pipe failed"; return ""; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    for (int fd : {in[0], in[1], out[0], out[1]}) posix_spawn_file_actions_addclose(&fa, fd);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", 1, 0);
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    const int rc = posix_spawnp(&pid, "secret-tool", &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]); close(out[1]);
    if (rc != 0) {
        close(in[1]); close(out[0]);
        error = std::string(fn) + "(): cannot run secret-tool (install libsecret-tools)";
        return "";
    }
    signal(SIGPIPE, SIG_IGN);   // secret-tool exiting early must not kill the server
    for (size_t off = 0; off < input.size();) {
        const ssize_t n = write(in[1], input.data() + off, input.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
    close(in[1]);
    std::string result;
    char buf[4096];
    for (ssize_t n; (n = read(out[0], buf, sizeof buf)) > 0 && result.size() < kMaxSecretBytes;) result.append(buf, static_cast<size_t>(n));
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
}

bool valid(const Value& service, const Value& key, const char* fn, std::string& error) {
    if (!service.is_str() || !key.is_str() || service.as_str().empty() || key.as_str().empty()) {
        error = std::string(fn) + "(): service and key must be non-empty strings";
        return false;
    }
    return true;
}

Value fn_set(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (!valid(a[0], a[1], "keyring.set", error)) return Value::null();
    if (!a[2].is_str() || a[2].as_str().empty() || a[2].as_str().size() > kMaxSecretBytes) {
        error = "keyring.set(): the secret must be a non-empty string";
        return Value::null();
    }
    if (const auto& c = keyring_control(); c.set) return Value::boolean(c.set(a[0].as_str(), a[1].as_str(), a[2].as_str()));
    bool ok;
    run({"secret-tool", "store", "--label=" + a[0].as_str() + " " + a[1].as_str(), "service", a[0].as_str(), "account", a[1].as_str()},
        a[2].as_str(), ok, error, "keyring.set");
    return error.empty() ? Value::boolean(ok) : Value::null();
}

Value fn_get(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (!valid(a[0], a[1], "keyring.get", error)) return Value::null();
    if (const auto& c = keyring_control(); c.get) {
        auto v = c.get(a[0].as_str(), a[1].as_str());
        return Value::str(v ? *v : "");
    }
    bool ok;
    std::string v = run({"secret-tool", "lookup", "service", a[0].as_str(), "account", a[1].as_str()}, "", ok, error, "keyring.get");
    return error.empty() ? Value::str(ok ? v : "") : Value::null();
}

Value fn_delete(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (!valid(a[0], a[1], "keyring.delete", error)) return Value::null();
    if (const auto& c = keyring_control(); c.del) return Value::boolean(c.del(a[0].as_str(), a[1].as_str()));
    bool ok;
    run({"secret-tool", "clear", "service", a[0].as_str(), "account", a[1].as_str()}, "", ok, error, "keyring.delete");
    return error.empty() ? Value::boolean(ok) : Value::null();
}

} // namespace

LUX_MODULE(keyring, {
    {"set",    "sss>b", fn_set,    /*is_async=*/true},
    {"get",    "ss>s",  fn_get,    true},
    {"delete", "ss>b",  fn_delete, true},
})

} // namespace lux_script
