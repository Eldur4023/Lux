#pragma once
// Starting a child process, shared by os.run() and proc.start().
//
// posix_spawn, not fork+exec: in a multithreaded process the child of a
// fork may only make async-signal-safe calls before exec, which rules out
// nearly all of C++. Never through a shell -- argv goes straight to the
// program, so no metacharacter in an argument is ever interpreted.
#include <lux_script/value.hpp>

#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace lux_script::spawn {

// Where a child's stdin/stdout/stderr goes. For stdin, File is read from.
struct Stream {
    enum Kind { Null, Pipe, File } kind = Null;
    std::string path;
};

struct Options {
    std::string cwd;                                      // empty = ours
    std::vector<std::pair<std::string, Value>> env;       // set (string) or unset (null)
    Stream in, out, err;
};

struct Child {
    pid_t pid = -1;
    int   in = -1, out = -1, err = -1;   // our end of each Pipe stream
};

// {"cwd": "...", "env": {"K": "v", "UNSET_ME": null}} out of an options
// Dict; stream fields are the caller's to read.
inline bool read_common(const Value::Dict& opts, Options& o, const char* fn, std::string& error) {
    if (auto it = opts.find("cwd"); it != opts.end()) {
        if (!it->second.is_str()) { error = std::string(fn) + "(): cwd must be a string"; return false; }
        o.cwd = it->second.as_str();
    }
    if (auto it = opts.find("env"); it != opts.end()) {
        if (!it->second.is_dict()) { error = std::string(fn) + "(): env must be a Dict"; return false; }
        for (const auto& [k, v] : it->second.as_dict()) {
            if (!v.is_str() && !v.is_null()) { error = std::string(fn) + "(): env values are strings (or null to unset)"; return false; }
            o.env.emplace_back(k, v);
        }
    }
    return true;
}

// argv[0] is the program, looked up in PATH.
inline bool start(const std::vector<std::string>& argv, const Options& o, Child& c, std::string& error) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    std::vector<int> ours, theirs;

    // O_CLOEXEC on both ends: another thread spawning at the same moment
    // must not inherit them -- an unrelated child holding our pipe's write
    // end open would keep this one's reader waiting for an EOF that never
    // comes.
    auto wire = [&](const Stream& s, int target, int& our_end) {
        if (s.kind == Stream::Pipe) {
            int p[2];
            if (pipe2(p, O_CLOEXEC) != 0) return false;
            const bool reading = target == STDIN_FILENO;
            our_end = reading ? p[1] : p[0];
            const int child_end = reading ? p[0] : p[1];
            ours.push_back(our_end);
            theirs.push_back(child_end);
            posix_spawn_file_actions_adddup2(&fa, child_end, target);
        } else {
            const char* path = s.kind == Stream::File ? s.path.c_str() : "/dev/null";
            const int flags  = target == STDIN_FILENO ? O_RDONLY
                             : s.kind == Stream::File ? O_WRONLY | O_CREAT | O_APPEND : O_WRONLY;
            posix_spawn_file_actions_addopen(&fa, target, path, flags, 0644);
        }
        return true;
    };
    bool ok = wire(o.in, STDIN_FILENO, c.in) && wire(o.out, STDOUT_FILENO, c.out) &&
              wire(o.err, STDERR_FILENO, c.err);
    if (ok && !o.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());

    // The environment: ours, minus every key the options touch, plus what
    // they set.
    std::vector<std::string> env_storage;
    for (char** e = environ; *e; ++e) {
        const std::string_view kv(*e);
        const auto key = kv.substr(0, kv.find('='));
        bool touched = false;
        for (const auto& [k, _] : o.env) touched |= k == key;
        if (!touched) env_storage.emplace_back(kv);
    }
    for (const auto& [k, v] : o.env)
        if (v.is_str()) env_storage.push_back(k + "=" + v.as_str());
    std::vector<char*> envp, args;
    for (auto& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    int rc = ok ? posix_spawnp(&c.pid, argv[0].c_str(), &fa, nullptr, args.data(), envp.data()) : errno;
    posix_spawn_file_actions_destroy(&fa);
    for (int fd : theirs) ::close(fd);   // only the child needs its ends
    if (rc != 0) {
        for (int fd : ours) ::close(fd);
        c = Child{};
        error = "could not start '" + argv[0] + "': " + std::strerror(rc);
        return false;
    }
    return true;
}

// argv out of (command, [args...]) as the Lux functions receive them.
inline std::vector<std::string> argv_of(const std::vector<Value>& a) {
    std::vector<std::string> argv{a[0].as_str()};
    if (a.size() > 1 && a[1].is_list())
        for (const Value& v : a[1].as_list()) argv.push_back(v.to_string());
    return argv;
}

} // namespace lux_script::spawn
