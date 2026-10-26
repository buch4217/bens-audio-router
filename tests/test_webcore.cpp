// Nativer Testserver fuer webcore.h: POSIX-TCP (nur Test, ohne TLS) + Fake-Backend. Gegenstelle: web/test/cpp-server.test.js
//   ./test_webcore --index web/index.html --state /tmp/dir [--port 0] [--code 12345678] [--ttl 120]
#include "webcore.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using namespace web;
struct Sock : Transport { int fd; std::string ip; Sock(int f, std::string p) : fd(f), ip(p) {} ~Sock() { Close(); }
    int Read(void* p, int n) override { return (int)recv(fd, p, n, 0); }
    bool WriteAll(const void* p, int n) override { const char* c = (const char*)p; while (n > 0) { ssize_t k = send(fd, c, n, MSG_NOSIGNAL); if (k <= 0) return false; c += k; n -= (int)k; } return true; }
    void SetTimeout(int ms) override { timeval tv{ms / 1000, (ms % 1000) * 1000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
    void Close() override { if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); fd = -1; } }
    std::string PeerIp() override { return ip; } };
struct Fake : Backend { std::mutex m; double gain1 = 0; bool mute1 = false; std::atomic<long> bin{0};
    void OnBinary(const std::string&, const std::string& d) override { bin += (long)d.size(); }
    std::string StateFields() override { std::lock_guard<std::mutex> l(m); char b[512]; snprintf(b, sizeof b, "\"running\":true,\"autopilot\":false,\"nodes\":[{\"id\":1,\"kind\":\"cap\",\"name\":\"Quelle \\u00e4\",\"gain\":%.1f,\"mute\":%s,\"ports\":\"out\"},{\"id\":2,\"kind\":\"out\",\"name\":\"Ausgang\",\"gain\":0,\"mute\":false,\"ports\":\"in\"}],\"conns\":[],\"scenes\":[\"Party\"],\"bin\":%ld", gain1, mute1 ? "true" : "false", bin.load()); return b; }
    std::string MeterJson() override { return "{\"t\":\"m\",\"a\":[[1,-12.0,-13.0,0],[2,-90.0,-90.0,0]]}"; }
    bool Exec(const JObj& c, std::string& err) override { std::string op; GetStr(c, "op", op); std::lock_guard<std::mutex> l(m);
        if (op == "gain") { double n, db; if (!GetNum(c, "node", n) || n != 1 || !GetNum(c, "db", db) || db < -60 || db > 0) { err = "Ungueltig"; return false; } gain1 = db; return true; }
        if (op == "mute") { bool on; if (!GetBool(c, "on", on)) { err = "Ungueltig"; return false; } mute1 = on; return true; }
        err = "Unbekannter Befehl"; return false; } };
int main(int argc, char** argv) {
    std::string index, state = "/tmp"; int port = 0, ttl = 120; std::string code;
    for (int i = 1; i + 1 < argc; i += 2) { std::string k = argv[i]; if (k == "--index") index = argv[i + 1]; else if (k == "--state") state = argv[i + 1]; else if (k == "--port") port = atoi(argv[i + 1]); else if (k == "--code") code = argv[i + 1]; else if (k == "--ttl") ttl = atoi(argv[i + 1]); }
    RngHook() = [](uint8_t* p, size_t n) { FILE* f = fopen("/dev/urandom", "rb"); if (!f || fread(p, 1, n, f) != n) abort(); fclose(f); };
    Fake be; Server srv(&be, state); srv.scheme = "http"; srv.pairTtlSec = ttl; srv.AddAsset("/manifest.webmanifest", "application/manifest+json", "{}"); srv.SetCaPem("-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n");
    { FILE* f = fopen(index.c_str(), "rb"); std::string h; char b[4096]; size_t k; while (f && (k = fread(b, 1, sizeof b, f)) > 0) h.append(b, k); if (f) fclose(f); srv.SetIndex(h); }
    if (code.empty()) code = srv.NewPairCode(); else srv.SetPairCode(code);
    int ls = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (bind(ls, (sockaddr*)&a, sizeof a) || listen(ls, 16)) { perror("bind"); return 1; } socklen_t al = sizeof a; getsockname(ls, (sockaddr*)&a, &al);
    printf("PORT=%d CODE=%s\n", ntohs(a.sin_port), code.c_str()); fflush(stdout); srv.Start();
    for (;;) { sockaddr_in pa{}; socklen_t pl = sizeof pa; int c = accept(ls, (sockaddr*)&pa, &pl); if (c < 0) continue; char ip[64]; inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
        std::thread([&srv, c, s = std::string(ip)] { srv.Handle(std::unique_ptr<Transport>(new Sock(c, s))); }).detach(); }
}
