// Proof of concept: how much do we gain by compiling Lux Script logic to
// native C++ instead of interpreting it in the bytecode VM?
//
// This is NOT a transpiler. It's fib()/cuenta_primos() hand-written in C++,
// copying EXACTLY the logic of bench/lux/app.lux (same names, same
// algorithm, same validation limits), served by the simplest HTTP server
// that could be written with no dependencies (raw sockets, no epoll, no
// keep-alive) so the number measures "cost of executing the logic" and not
// "how good my toy HTTP server is". It doesn't compete with Gin/Fastify
// as a framework -- it competes with lux_script::VM as an execution engine.
//
// See experiments/native_poc/RESULTS.md for the comparison and the write-up.

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// ---- logic copied 1:1 from bench/lux/app.lux (fib / cuenta_primos) ----

static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static int64_t cuenta_primos(int64_t limite) {
    int64_t contador = 0;
    int64_t i = 2;
    while (i < limite) {
        bool es_primo = true;
        int64_t d = 2;
        while (d * d <= i) {
            if (i % d == 0) {
                es_primo = false;
                break;
            }
            d++;
        }
        if (es_primo) contador++;
        i++;
    }
    return contador;
}

// ---- minimal HTTP server (no framework, no keep-alive) ----

static void send_all(int fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = write(fd, s.data() + sent, s.size() - sent);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

static void respond_json(int fd, int status, const std::string& body) {
    const char* status_text = status == 200 ? "OK" : (status == 400 ? "Bad Request" : "Error");
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    send_all(fd, resp);
}

static void respond_empty(int fd, int status) {
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " Bad Request\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(fd, resp);
}

static void handle_client(int fd) {
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = 0;

    // Minimal parsing of the first line: "GET /compute/fib/28 HTTP/1.1"
    std::string req(buf);
    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 + 1);
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
        ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "";

    auto last_segment = [&](const std::string& prefix) -> long long {
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return -1;
        try { return std::stoll(path.substr(prefix.size())); } catch (...) { return -1; }
    };

    if (long long n_fib = last_segment("/compute/fib/"); n_fib >= 0) {
        if (n_fib < 1 || n_fib > 32) { respond_empty(fd, 400); close(fd); return; }
        int64_t r = fib(n_fib);
        respond_json(fd, 200, "{\"n\":" + std::to_string(n_fib) + ",\"result\":" + std::to_string(r) + "}");
    } else if (long long n_primes = last_segment("/compute/primes/"); n_primes >= 0) {
        if (n_primes < 2 || n_primes > 200000) { respond_empty(fd, 400); close(fd); return; }
        int64_t c = cuenta_primos(n_primes);
        respond_json(fd, 200, "{\"n\":" + std::to_string(n_primes) + ",\"count\":" + std::to_string(c) + "}");
    } else if (path == "/health") {
        respond_json(fd, 200, "{\"status\":\"ok\"}");
    } else {
        respond_empty(fd, 400);
    }
    close(fd);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8096);
    bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(server_fd, 1024);

    for (;;) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        std::thread(handle_client, client_fd).detach();
    }
}
