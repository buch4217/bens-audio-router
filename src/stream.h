// stream.h - Netzwerk-Schicht von Bens Audio Router
// * SSDP-Suche nach Sonos-Zonenplayern, kleiner HTTP-Client, UPnP/SOAP (AVTransport, RenderingControl)
// * HTTP/1.0-Server, der einen unendlichen 16-Bit-WAV-Stream (PCM) ausliefert (ohne Chunking)
// * Steuer-Thread: SetAVTransportURI + Play, Wiederaufnahme, automatische Latenzmessung
//   (RelTime-Uebergaenge der Sonos-Positionsabfrage, statistisch gemittelt)
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <algorithm>
#include <deque>
#include <set>

#include "core.h"
#include "flac.h"

double NowSec();
void Log(const char* fmt, ...);

// ------------------------------------------------------------------ Hilfsfunktionen

static std::string U8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring W8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string XmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}
static std::string XmlUnesc(std::string s) {
    const char* from[] = {"&lt;", "&gt;", "&quot;", "&apos;", "&amp;"};
    const char* to[] = {"<", ">", "\"", "'", "&"};
    for (int i = 0; i < 5; i++) {
        size_t p = 0;
        while ((p = s.find(from[i], p)) != std::string::npos) {
            s.replace(p, strlen(from[i]), to[i]);
            p += strlen(to[i]);
        }
    }
    return s;
}
static std::string XmlTag(const std::string& xml, const char* tag) {
    std::string open = std::string("<") + tag;
    size_t a = 0;
    while ((a = xml.find(open, a)) != std::string::npos) {
        char nx = a + open.size() < xml.size() ? xml[a + open.size()] : 0;
        if (nx == '>' || nx == ' ' || nx == '/') break;
        a += open.size();
    }
    if (a == std::string::npos) return "";
    size_t b = xml.find('>', a);
    if (b == std::string::npos || xml[b - 1] == '/') return "";
    size_t c = xml.find(std::string("</") + tag, b);
    if (c == std::string::npos) return "";
    return XmlUnesc(xml.substr(b + 1, c - b - 1));
}

// Dekodiert einen "Transfer-Encoding: chunked"-Body (Sonos antwortet so)
static std::string DechunkBody(const std::string& raw) {
    std::string out;
    size_t p = 0;
    while (p < raw.size()) {
        size_t e = raw.find("\r\n", p);
        if (e == std::string::npos) break;
        std::string line = raw.substr(p, e - p);
        size_t sc = line.find(';');
        if (sc != std::string::npos) line = line.substr(0, sc);
        unsigned long sz = strtoul(line.c_str(), nullptr, 16);
        p = e + 2;
        if (sz == 0) break;
        if (p + sz > raw.size()) { out.append(raw, p, std::string::npos); break; }
        out.append(raw, p, sz);
        p += sz + 2;
    }
    return out;
}

// Steuerzeichen entfernen, Leerraum trimmen (Namen aus XML)
static std::wstring CleanText(std::wstring s) {
    std::wstring o;
    for (wchar_t c : s) if (c >= 32) o += c;
    size_t a = o.find_first_not_of(L' '), b = o.find_last_not_of(L' ');
    return a == std::wstring::npos ? L"" : o.substr(a, b - a + 1);
}

// ------------------------------------------------------------------ HTTP-Client (blockierend)

static SOCKET ConnectTo(const std::string& host, int port, int timeoutMs, std::string& err) {
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0 || !res) { err = "Adresse nicht auflösbar"; return INVALID_SOCKET; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); err = "Socket-Fehler"; return INVALID_SOCKET; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); err = "Verbindung abgelehnt"; return INVALID_SOCKET; }
    fd_set wf, ef;
    FD_ZERO(&wf); FD_ZERO(&ef);
    FD_SET(s, &wf); FD_SET(s, &ef);
    timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    r = select(0, nullptr, &wf, &ef, &tv);
    if (r <= 0 || FD_ISSET(s, &ef)) { closesocket(s); err = r == 0 ? "Zeitüberschreitung" : "Verbindung fehlgeschlagen"; return INVALID_SOCKET; }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD to = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    return s;
}

// Sendet eine fertige Anfrage und liest die komplette Antwort (bis Content-Length bzw. Verbindungsende).
static bool HttpExchange(const std::string& host, int port, const std::string& req, std::string& status, std::string& body,
                         int timeoutMs, std::string& err) {
    SOCKET s = ConnectTo(host, port, timeoutMs, err);
    if (s == INVALID_SOCKET) return false;
    size_t off = 0;
    while (off < req.size()) {
        int n = send(s, req.data() + off, (int)(req.size() - off), 0);
        if (n <= 0) { closesocket(s); err = "Senden fehlgeschlagen"; return false; }
        off += n;
    }
    std::string resp;
    char buf[4096];
    size_t hdrEnd = std::string::npos;
    long contentLen = -1;
    bool chunked = false;
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, n);
        if (hdrEnd == std::string::npos) {
            hdrEnd = resp.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) {
                std::string h = resp.substr(0, hdrEnd);
                for (auto& c : h) c = (char)tolower((unsigned char)c);
                size_t p = h.find("content-length:");
                if (p != std::string::npos) contentLen = atol(h.c_str() + p + 15);
                chunked = h.find("transfer-encoding: chunked") != std::string::npos;
            }
        }
        if (chunked && resp.size() >= 5 && resp.compare(resp.size() - 5, 5, "0\r\n\r\n") == 0) break;
        if (hdrEnd != std::string::npos && contentLen >= 0 && (long)(resp.size() - hdrEnd - 4) >= contentLen) break;
        if (resp.size() > 4 * 1024 * 1024) break;
    }
    closesocket(s);
    if (hdrEnd == std::string::npos) { err = "Keine gültige HTTP-Antwort"; return false; }
    size_t sp = resp.find(' ');
    status = sp != std::string::npos ? resp.substr(sp + 1, 3) : "";
    body = resp.substr(hdrEnd + 4);
    if (chunked) body = DechunkBody(body);
    return true;
}

static bool HttpGet(const std::string& ip, int port, const std::string& path, std::string& body, std::string& err) {
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + ip + ":" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    std::string st;
    if (!HttpExchange(ip, port, req, st, body, 2500, err)) return false;
    if (st != "200") { err = "HTTP " + st; return false; }
    return true;
}

static int g_sonosPort = 1400;  // Sonos-Standardport (fuer Tests ueberschreibbar)

static bool SoapCall(const std::string& ip, const char* path, const char* svc, const char* action, const std::string& args,
                     std::string& out, std::string& err, int timeoutMs = 2500) {
    std::string body = std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                                   "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:") +
                       action + " xmlns:u=\"urn:schemas-upnp-org:service:" + svc + ":1\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    std::string req = std::string("POST ") + path + " HTTP/1.1\r\nHost: " + ip + ":" + std::to_string(g_sonosPort) +
                      "\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"urn:schemas-upnp-org:service:" + svc + ":1#" + action +
                      "\"\r\nConnection: close\r\n\r\n" + body;
    std::string st;
    if (!HttpExchange(ip, g_sonosPort, req, st, out, timeoutMs, err)) return false;
    if (st != "200") {
        std::string code = XmlTag(out, "errorCode");
        err = "UPnP " + st + (code.empty() ? "" : " / Fehler " + code);
        return false;
    }
    return true;
}

static const char* AVT = "/MediaRenderer/AVTransport/Control";
static const char* RCS = "/MediaRenderer/RenderingControl/Control";

// ------------------------------------------------------------------ SSDP-Suche

struct SonosDev {
    std::string ip;
    std::wstring name, model;
};

static std::vector<SonosDev> SsdpDiscover(int ms) {
    std::vector<SonosDev> found;
    std::vector<SOCKET> socks;
    ULONG sz = 16384;
    std::vector<uint8_t> buf(sz);
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &sz);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(sz); aa = (IP_ADAPTER_ADDRESSES*)buf.data(); rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &sz); }
    const char* msearch = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                          "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);
    if (rc == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                sockaddr_in local = *(sockaddr_in*)u->Address.lpSockaddr;
                local.sin_port = 0;
                SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (s == INVALID_SOCKET) continue;
                if (bind(s, (sockaddr*)&local, sizeof(local)) != 0) { closesocket(s); continue; }
                in_addr ifa = local.sin_addr;
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
                int ttl = 2;
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
                sendto(s, msearch, (int)strlen(msearch), 0, (sockaddr*)&dst, sizeof(dst));
                sendto(s, msearch, (int)strlen(msearch), 0, (sockaddr*)&dst, sizeof(dst));
                socks.push_back(s);
            }
        }
    }
    std::vector<std::string> ips;
    double tEnd = NowSec() + ms / 1000.0;
    while (NowSec() < tEnd && !socks.empty()) {
        fd_set rf;
        FD_ZERO(&rf);
        for (SOCKET s : socks) FD_SET(s, &rf);
        timeval tv = {0, 150000};
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
        for (SOCKET s : socks) {
            if (!FD_ISSET(s, &rf)) continue;
            char b[2048];
            sockaddr_in from;
            int fl = sizeof(from);
            int n = recvfrom(s, b, sizeof(b) - 1, 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            b[n] = 0;
            std::string r = b;
            std::string low = r;
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            if (low.find("zoneplayer") == std::string::npos && low.find("sonos") == std::string::npos) continue;
            char ipb[64];
            inet_ntop(AF_INET, &from.sin_addr, ipb, sizeof(ipb));
            std::string ip = ipb;
            bool dup = false;
            for (auto& x : ips) if (x == ip) dup = true;
            if (!dup) ips.push_back(ip);
        }
    }
    for (SOCKET s : socks) closesocket(s);
    for (auto& ip : ips) {
        SonosDev d;
        d.ip = ip;
        std::string body, err;
        if (HttpGet(ip, g_sonosPort, "/xml/device_description.xml", body, err)) {
            std::string room = XmlTag(body, "roomName");
            if (room.empty()) room = XmlTag(body, "friendlyName");
            d.name = CleanText(W8(room));
            d.model = CleanText(W8(XmlTag(body, "modelName")));
        }
        if (d.name.empty()) d.name = W8(ip);
        found.push_back(d);
    }
    return found;
}

static std::string LocalIpFor(const std::string& remote) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    std::string out;
    if (inet_pton(AF_INET, remote.c_str(), &a.sin_addr) == 1 && connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        sockaddr_in me;
        int l = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &l) == 0) {
            char b[64];
            inet_ntop(AF_INET, &me.sin_addr, b, sizeof(b));
            out = b;
        }
    }
    closesocket(s);
    return out;
}

// ------------------------------------------------------------------ Stream-Kontext

struct StreamCtx {
    // Konfiguration (UI-Thread schreibt, andere lesen)
    std::string ip;
    std::wstring room;
    int port = 8100;
    int rate = 44100;     // 44100 / 48000 (Sonos: bis 48 kHz, 16 Bit)
    int burstMs = 1500;   // Vorlauf = Puffer-Reserve beim Sonos
    int scheme = 1;       // 0 = x-rincon-mp3radio://, 1 = http:// (Standard: funktioniert mit WAV/FLAC-Streams)
    int format = 1;       // 0 = WAV 16 Bit, 1 = FLAC 24 Bit (verlustfrei)
    std::atomic<bool> volCtl{false};   // Sonos-Lautstaerke ueberhaupt setzen?
    std::atomic<bool> autoLat{true};
    std::atomic<bool> dither{true};
    std::atomic<int> sonosVol{-1};    // -1 = nicht anfassen
    std::atomic<bool> playWanted{true};
    std::atomic<bool> autoFallback{false};   // FLAC vom Sonos abgelehnt -> auf WAV 16 Bit umschalten
    std::atomic<int> volDirty{0};
    // Sonos-Gruppe: weitere Zonen, die der Koordinator (diese IP) in seine Gruppe holt.
    // Die Synchronisation untereinander uebernimmt Sonos mit der eigenen Clock.
    std::vector<std::string> groupIps;
    std::vector<std::wstring> groupNames;
    std::atomic<int> coordVol{-1}, coordWant{-1};   // Lautstaerke Haupt-Zone (-1 unbekannt/keine Aenderung)
    std::vector<int> groupVol, groupWant;           // Lautstaerke je Gruppenzone (Ist / gewuenscht)
    std::vector<int> groupStat;     // je Zone: 0 unbekannt, 1 beigetreten, 2 fehlgeschlagen
    std::atomic<int> groupOk{0};
    std::atomic<bool> groupDirty{true};

    // Server
    std::vector<uint8_t> ring;
    size_t ringBytes = 0;
    std::atomic<uint64_t> wr{0};
    double t0 = 0;
    uint64_t framesOut = 0;
    SOCKET lis = INVALID_SOCKET;
    HANDLE accThread = nullptr, ctlThread = nullptr;
    std::atomic<bool> stop{false};
    CRITICAL_SECTION cs;
    std::vector<SOCKET> clientSocks;
    std::atomic<int> nClients{0};
    std::atomic<double> clientT0prod{0};
    std::atomic<int> clientEpoch{0};
    std::atomic<uint64_t> bytesSent{0};
    Dither dth;
    bool running = false;
    // FLAC
    std::vector<uint8_t> hdr;
    struct FrameRec { uint64_t bytePos, startSample; };
    std::deque<FrameRec> frames;
    std::vector<int32_t> fL, fR;
    int fFill = 0;
    uint64_t fSamples = 0, fFrameNo = 0;

    // Sonos-Status
    std::atomic<int> ctl{0};  // 0 aus, 1 verbinde, 2 spielt, 3 Fehler, 4 wartet
    wchar_t msg[200] = {};
    std::atomic<double> measLatMs{0}, measErrMs{0};
    std::atomic<int> measN{0}, measState{0};  // 0 keine, 1 sammelt, 2 ok, 3 nicht verfuegbar
    std::atomic<int> restarts{0};

    StreamCtx() { InitializeCriticalSection(&cs); }
    ~StreamCtx() { DeleteCriticalSection(&cs); }
    void EnsureVol() {   // cs halten!
        if (groupVol.size() < groupIps.size()) groupVol.resize(groupIps.size(), -1);
        if (groupWant.size() < groupIps.size()) groupWant.resize(groupIps.size(), -1);
        if (groupVol.size() > groupIps.size()) groupVol.resize(groupIps.size());
        if (groupWant.size() > groupIps.size()) groupWant.resize(groupIps.size());
    }
    void SetMsg(const std::wstring& m) {
        wcsncpy(msg, m.c_str(), 199);
        msg[199] = 0;
    }
    int ByteRate() const { return rate * 4; }
    bool Flac() const { return format == 1; }
};

static void StreamPush(StreamCtx* sc, const int16_t* pcm, size_t frames) {
    size_t bytes = frames * 4;
    uint64_t w = sc->wr.load(std::memory_order_relaxed);
    const uint8_t* src = (const uint8_t*)pcm;
    size_t pos = (size_t)(w % sc->ringBytes);
    size_t first = std::min(bytes, sc->ringBytes - pos);
    memcpy(&sc->ring[pos], src, first);
    if (first < bytes) memcpy(&sc->ring[0], src + first, bytes - first);
    sc->wr.store(w + bytes, std::memory_order_release);
}

static void StreamPushBytes(StreamCtx* sc, const uint8_t* src, size_t bytes) {
    uint64_t w = sc->wr.load(std::memory_order_relaxed);
    size_t pos = (size_t)(w % sc->ringBytes);
    size_t first = std::min(bytes, sc->ringBytes - pos);
    memcpy(&sc->ring[pos], src, first);
    if (first < bytes) memcpy(&sc->ring[0], src + first, bytes - first);
    sc->wr.store(w + bytes, std::memory_order_release);
}

// Ein komplettes FLAC-Frame (inkl. Index-Eintrag) in den Ring legen
static void StreamPushFlacFrame(StreamCtx* sc, const std::vector<uint8_t>& frame, uint64_t startSample) {
    EnterCriticalSection(&sc->cs);
    sc->frames.push_back({sc->wr.load(), startSample});
    StreamPushBytes(sc, frame.data(), frame.size());
    uint64_t w = sc->wr.load();
    while (!sc->frames.empty() && sc->frames.front().bytePos + sc->ringBytes - 262144 < w) sc->frames.pop_front();
    LeaveCriticalSection(&sc->cs);
}

static void MakeWavHeader(uint8_t* h, int rate) {
    auto w32 = [&](int o, uint32_t v) { memcpy(h + o, &v, 4); };
    auto w16 = [&](int o, uint16_t v) { memcpy(h + o, &v, 2); };
    memcpy(h, "RIFF", 4);
    w32(4, 0xFFFFFFFFu);
    memcpy(h + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);
    w16(22, 2);
    w32(24, rate);
    w32(28, rate * 4);
    w16(32, 4);
    w16(34, 16);
    memcpy(h + 36, "data", 4);
    w32(40, 0xFFFFFFFFu);
}

static bool SendAll(SOCKET s, const void* d, size_t n) {
    const char* p = (const char*)d;
    while (n > 0) {
        int k = send(s, p, (int)std::min<size_t>(n, 65536), 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

struct ClientArg {
    StreamCtx* sc;
    SOCKET s;
};

static DWORD WINAPI ClientProc(LPVOID p) {
    ClientArg* ca = (ClientArg*)p;
    StreamCtx* sc = ca->sc;
    SOCKET s = ca->s;
    delete ca;
    sc->nClients++;
    char rq[4096];
    int got = 0;
    DWORD to = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    while (got < (int)sizeof(rq) - 1) {
        int n = recv(s, rq + got, (int)sizeof(rq) - 1 - got, 0);
        if (n <= 0) break;
        got += n;
        rq[got] = 0;
        if (strstr(rq, "\r\n\r\n")) break;
    }
    rq[got] = 0;
    bool isHead = strncmp(rq, "HEAD ", 5) == 0;
    bool isGet = strncmp(rq, "GET ", 4) == 0;
    std::string line(rq, strcspn(rq, "\r\n"));
    Log("Stream-Client: %s", line.c_str());
    const bool flacFmt = sc->Flac();
    if (!(isGet || isHead) || line.find("/stream.") == std::string::npos) {
        const char* nf = "HTTP/1.0 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        SendAll(s, nf, strlen(nf));
    } else {
        std::string hdr = std::string("HTTP/1.0 200 OK\r\nContent-Type: ") + (flacFmt ? "audio/flac" : "audio/wav") +
                          "\r\nCache-Control: no-cache\r\nPragma: no-cache\r\nConnection: close\r\nServer: BensAudioRouter\r\n"
                          "icy-name: Bens Audio Router\r\n\r\n";
        bool ok = SendAll(s, hdr.data(), hdr.size());
        if (ok && !isHead) {
            const int64_t byteRate = sc->ByteRate();
            const int64_t burstSamples = (int64_t)sc->burstMs * sc->rate / 1000;
            // Startpunkt waehlen (WAV: Byte-Position; FLAC: Frame-Grenze) und Produktionszeit des ersten Samples merken
            auto pickStart = [&]() -> int64_t {
                if (!flacFmt) {
                    int64_t pos = (int64_t)sc->wr.load() - burstSamples * 4;
                    pos -= ((pos % 4) + 4) % 4;
                    sc->clientT0prod = sc->t0 + (double)pos / (double)byteRate;
                    return pos;
                }
                int64_t pos = 0;
                EnterCriticalSection(&sc->cs);
                if (!sc->frames.empty()) {
                    int64_t target = (int64_t)sc->fSamples - burstSamples;
                    size_t k = 0;
                    for (size_t i = sc->frames.size(); i-- > 0;)
                        if ((int64_t)sc->frames[i].startSample <= target) { k = i; break; }
                    pos = (int64_t)sc->frames[k].bytePos;
                    sc->clientT0prod = sc->t0 + (double)sc->frames[k].startSample / (double)sc->rate;
                } else pos = (int64_t)sc->wr.load();
                LeaveCriticalSection(&sc->cs);
                return pos;
            };
            if (flacFmt) ok = SendAll(s, sc->hdr.data(), sc->hdr.size());
            else { uint8_t wav[44]; MakeWavHeader(wav, sc->rate); ok = SendAll(s, wav, 44); }
            int64_t pos = pickStart();
            sc->clientEpoch++;
            sc->measState = 1;
            sc->measN = 0;
            std::vector<uint8_t> tmp(16384);
            DWORD sto = 5000;
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sto, sizeof(sto));
            while (ok && !sc->stop) {
                int64_t w = (int64_t)sc->wr.load(std::memory_order_acquire);
                if (w - pos > (int64_t)sc->ringBytes - 262144) {  // zu weit zurueck -> neu aufsetzen
                    pos = pickStart();
                    sc->clientEpoch++;
                }
                int64_t avail = w - pos;
                if (avail < (flacFmt ? 1 : 352)) { Sleep(2); continue; }
                size_t n = (size_t)std::min<int64_t>(avail, 16384);
                if (!flacFmt) n -= n % 4;
                size_t off = (size_t)(pos % (int64_t)sc->ringBytes);
                size_t first = std::min(n, sc->ringBytes - off);
                memcpy(tmp.data(), &sc->ring[off], first);
                if (first < n) memcpy(tmp.data() + first, &sc->ring[0], n - first);
                ok = SendAll(s, tmp.data(), n);
                pos += (int64_t)n;
                sc->bytesSent += n;
            }
        }
    }
    EnterCriticalSection(&sc->cs);
    for (size_t i = 0; i < sc->clientSocks.size(); i++)
        if (sc->clientSocks[i] == s) { sc->clientSocks.erase(sc->clientSocks.begin() + i); break; }
    LeaveCriticalSection(&sc->cs);
    shutdown(s, SD_BOTH);
    closesocket(s);
    sc->nClients--;
    return 0;
}

static DWORD WINAPI AcceptProc(LPVOID p) {
    StreamCtx* sc = (StreamCtx*)p;
    while (!sc->stop) {
        SOCKET c = accept(sc->lis, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        EnterCriticalSection(&sc->cs);
        sc->clientSocks.push_back(c);
        LeaveCriticalSection(&sc->cs);
        ClientArg* ca = new ClientArg{sc, c};
        HANDLE h = CreateThread(nullptr, 0, ClientProc, ca, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete ca; closesocket(c); }
    }
    return 0;
}

// ------------------------------------------------------------------ Sonos-Steuerung

static std::string DidlFor(const std::string& title) {
    return std::string("<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                       "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
                       "<item id=\"R:0/0/0\" parentID=\"R:0/0\" restricted=\"true\"><dc:title>") +
           XmlEsc(title) +
           "</dc:title><upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
           "<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">SA_RINCON65031_</desc></item></DIDL-Lite>";
}

static int ParseRelTime(const std::string& s) {  // "H:MM:SS" -> Sekunden, sonst -1
    int h = 0, m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) return h * 3600 + m * 60 + sec;
    return -1;
}

static bool SonosStartPlayback(StreamCtx* sc, std::string& err) {
    std::string local = LocalIpFor(sc->ip);
    if (local.empty()) { err = "Lokale IP nicht ermittelbar"; return false; }
    std::string url = std::string(sc->scheme == 0 ? "x-rincon-mp3radio://" : "http://") + local + ":" + std::to_string(sc->port) + (sc->Flac() ? "/stream.flac" : "/stream.wav");
    std::string out;
    std::string args = "<InstanceID>0</InstanceID><CurrentURI>" + XmlEsc(url) + "</CurrentURI><CurrentURIMetaData>" +
                       XmlEsc(DidlFor("Bens Audio Router")) + "</CurrentURIMetaData>";
    Log("Sonos %s: SetAVTransportURI %s", sc->ip.c_str(), url.c_str());
    if (!SoapCall(sc->ip, AVT, "AVTransport", "SetAVTransportURI", args, out, err)) return false;
    if (!SoapCall(sc->ip, AVT, "AVTransport", "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", out, err)) return false;
    return true;
}

static DWORD WINAPI ControlProc(LPVOID p) {
    StreamCtx* sc = (StreamCtx*)p;
    bool started = false;
    double tStart = 0, nextStart = 0, lastTransport = 0, lastPoll = 0;
    int lastEpoch = -1, prevR = -1;
    double prevMid = 0;
    std::vector<double> oS;
    bool everInc = false;
    int volApplied = -2;
    int failStreak = 0;
    double lastVolPoll = 0;
    bool attemptPlayed = false;
    std::set<std::string> joined;
    std::string coordUdn;
    double lastGroup = 0;
    while (!sc->stop) {
        Sleep(20);
        double now = NowSec();
        if (sc->ip.empty() || !sc->playWanted) {
            if (started) {
                std::string o, e;
                SoapCall(sc->ip, AVT, "AVTransport", "Stop", "<InstanceID>0</InstanceID>", o, e);
                started = false;
            }
            sc->ctl = sc->ip.empty() ? 0 : 4;
            if (sc->ip.empty()) sc->SetMsg(L"Keine Sonos-IP gesetzt");
            continue;
        }
        if (!started && now >= nextStart) {
            sc->ctl = 1;
            sc->SetMsg(L"Verbinde mit Sonos …");
            std::string err;
            if (SonosStartPlayback(sc, err)) {
                started = true;
                tStart = now;
                prevR = -1;
                oS.clear();
                everInc = false;
                lastEpoch = -1;
                sc->measState = 1;
                sc->measN = 0;
                sc->restarts++;
                attemptPlayed = false;
                sc->groupDirty = true;
                sc->ctl = 2;
                sc->SetMsg(L"Wiedergabe gestartet");
                Log("Sonos: Wiedergabe gestartet");
            } else {
                sc->ctl = 3;
                sc->SetMsg(W8("Fehler: " + err));
                Log("Sonos: Start fehlgeschlagen: %s", err.c_str());
                nextStart = now + 4.0;
                if (err.compare(0, 4, "UPnP") == 0) {   // nur echte Ablehnung durch den Sonos zaehlt, keine Netzwerkfehler
                    failStreak++;
                    if (failStreak >= 2 && sc->format == 1) sc->autoFallback = true;
                }
            }
            continue;
        }
        if (!started) continue;

        // Gruppe: weitere Zonen beitreten lassen (Sonos synchronisiert sie intern, ms-genau)
        if (started && (sc->groupDirty.load() || now - lastGroup > 6.0)) {
            lastGroup = now;
            sc->groupDirty = false;
            std::vector<std::string> want;
            EnterCriticalSection(&sc->cs);
            want = sc->groupIps;
            LeaveCriticalSection(&sc->cs);
            if (coordUdn.empty()) {
                std::string body, e;
                if (HttpGet(sc->ip, g_sonosPort, "/xml/device_description.xml", body, e)) {
                    std::string u = XmlTag(body, "UDN");
                    while (!u.empty() && (u.back() == '\r' || u.back() == '\n' || u.back() == ' ')) u.pop_back();
                    if (u.compare(0, 5, "uuid:") == 0) u = u.substr(5);
                    if (u.compare(0, 7, "RINCON_") == 0) {
                        coordUdn = u;
                        Log("Sonos-Koordinator %s: %s", sc->ip.c_str(), coordUdn.c_str());
                    } else Log("Sonos-Koordinator %s: ungueltige UDN '%s'", sc->ip.c_str(), u.c_str());
                }
            }
            for (auto it = joined.begin(); it != joined.end();) {   // entfernte Zonen aus der Gruppe loesen
                if (std::find(want.begin(), want.end(), *it) == want.end()) {
                    std::string o, e;
                    SoapCall(*it, AVT, "AVTransport", "BecomeCoordinatorOfStandaloneGroup", "<InstanceID>0</InstanceID>", o, e);
                    Log("Sonos %s verlaesst die Gruppe", it->c_str());
                    it = joined.erase(it);
                } else ++it;
            }
            int ok = 0;
            std::vector<int> stat(want.size(), 0);
            for (size_t wi = 0; wi < want.size(); wi++) {
                const std::string& ip = want[wi];
                if (ip.empty() || ip == sc->ip || coordUdn.empty()) continue;
                std::string o, e;
                bool inGroup = false;
                if (SoapCall(ip, AVT, "AVTransport", "GetMediaInfo", "<InstanceID>0</InstanceID>", o, e))
                    inGroup = XmlTag(o, "CurrentURI") == "x-rincon:" + coordUdn;
                if (!inGroup) {
                    std::string args = "<InstanceID>0</InstanceID><CurrentURI>x-rincon:" + coordUdn + "</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>";
                    if (SoapCall(ip, AVT, "AVTransport", "SetAVTransportURI", args, o, e)) {
                        inGroup = true;
                        Log("Sonos %s tritt der Gruppe von %s bei", ip.c_str(), sc->ip.c_str());
                    } else Log("Sonos %s: Gruppenbeitritt fehlgeschlagen: %s", ip.c_str(), e.c_str());
                }
                stat[wi] = inGroup ? 1 : 2;
                if (inGroup) { ok++; joined.insert(ip); }
            }
            EnterCriticalSection(&sc->cs);
            if (sc->groupIps == want) sc->groupStat = stat;
            LeaveCriticalSection(&sc->cs);
            sc->groupOk = ok;
        }

        // Lautstaerke je Zone (0 = Haupt-Zone/Koordinator, 1.. = Gruppenzonen): Wuensche senden, Ist-Werte lesen
        {
            std::vector<std::string> ips;
            std::vector<int> wants;
            EnterCriticalSection(&sc->cs);
            sc->EnsureVol();
            ips.push_back(sc->ip);
            wants.push_back(sc->coordWant.load());
            for (size_t i = 0; i < sc->groupIps.size(); i++) { ips.push_back(sc->groupIps[i]); wants.push_back(sc->groupWant[i]); }
            LeaveCriticalSection(&sc->cs);
            bool pending = false;
            for (size_t z = 0; z < ips.size(); z++) {
                if (wants[z] < 0 || ips[z].empty()) continue;
                pending = true;
                std::string o, e;
                std::string args = "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>" + std::to_string(wants[z]) + "</DesiredVolume>";
                if (SoapCall(ips[z], RCS, "RenderingControl", "SetVolume", args, o, e, 1500)) {
                    EnterCriticalSection(&sc->cs);
                    if (z == 0) { if (sc->coordWant.load() == wants[z]) sc->coordWant = -1; }
                    else if (z - 1 < sc->groupWant.size() && sc->groupWant[z - 1] == wants[z]) sc->groupWant[z - 1] = -1;
                    LeaveCriticalSection(&sc->cs);
                } else Log("Sonos %s: SetVolume fehlgeschlagen: %s", ips[z].c_str(), e.c_str());
            }
            if (!pending && now - lastVolPoll > 3.0) {
                lastVolPoll = now;
                for (size_t z = 0; z < ips.size(); z++) {
                    if (ips[z].empty()) continue;
                    std::string o, e;
                    if (!SoapCall(ips[z], RCS, "RenderingControl", "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>", o, e, 1500)) continue;
                    std::string cv = XmlTag(o, "CurrentVolume");
                    if (cv.empty()) continue;
                    int v = atoi(cv.c_str());
                    EnterCriticalSection(&sc->cs);
                    if (z == 0) { if (sc->coordWant.load() < 0) sc->coordVol = v; }
                    else if (z - 1 < sc->groupVol.size() && z - 1 < sc->groupWant.size() && sc->groupWant[z - 1] < 0) sc->groupVol[z - 1] = v;
                    LeaveCriticalSection(&sc->cs);
                }
            }
        }

        // Transportstatus alle 2 s
        if (now - lastTransport > 2.0) {
            lastTransport = now;
            std::string o, e;
            if (SoapCall(sc->ip, AVT, "AVTransport", "GetTransportInfo", "<InstanceID>0</InstanceID>", o, e)) {
                std::string st = XmlTag(o, "CurrentTransportState");
                if (st == "PLAYING") { attemptPlayed = true; failStreak = 0; }
                bool stuck = !attemptPlayed && now - tStart > 20.0;
                if ((now - tStart > 6.0 && st != "PLAYING" && st != "TRANSITIONING") || stuck) {
                    if (!attemptPlayed) {
                        failStreak++;
                        if (failStreak >= 2 && sc->format == 1) sc->autoFallback = true;
                    }
                    Log("Sonos: Transportstatus %s -> Neustart", st.c_str());
                    sc->SetMsg(W8("Sonos-Status: " + st + " – starte neu"));
                    sc->ctl = 4;
                    started = false;
                    nextStart = now + 1.0;
                    continue;
                }
                if (sc->ctl != 2) sc->ctl = 2;
            } else {
                sc->ctl = 3;
                sc->SetMsg(W8("Sonos nicht erreichbar: " + e));
            }
        }

        // Positionsabfrage fuer die Latenzmessung (~8x pro Sekunde)
        if (sc->autoLat && now - lastPoll > 0.12) {
            lastPoll = now;
            int epoch = sc->clientEpoch.load();
            if (epoch != lastEpoch) {
                lastEpoch = epoch;
                prevR = -1;
                oS.clear();
                sc->measN = 0;
                sc->measState = 1;
                tStart = std::min(tStart, now);
            }
            std::string o, e;
            double t1 = NowSec();
            bool ok = SoapCall(sc->ip, AVT, "AVTransport", "GetPositionInfo", "<InstanceID>0</InstanceID>", o, e, 1500);
            double t2 = NowSec();
            if (ok) {
                int r = ParseRelTime(XmlTag(o, "RelTime"));
                double mid = (t1 + t2) / 2.0;
                if (r >= 0) {
                    if (prevR >= 0 && r == prevR + 1) {
                        double T = (prevMid + mid) / 2.0;
                        oS.push_back(T - (double)r);
                        everInc = true;
                        if (oS.size() > 60) oS.erase(oS.begin());
                    }
                    prevR = r;
                    prevMid = mid;
                }
                if (oS.size() >= 8) {
                    double mean = 0;
                    for (double x : oS) mean += x;
                    mean /= (double)oS.size();
                    double var = 0;
                    for (double x : oS) var += (x - mean) * (x - mean);
                    var /= (double)oS.size();
                    double L = (mean - sc->clientT0prod.load()) * 1000.0;
                    double err = std::sqrt(var / (double)oS.size()) * 1000.0 + 5.0;
                    if (L >= 0 && L < 40000) {
                        sc->measLatMs = L;
                        sc->measErrMs = err;
                        sc->measState = 2;
                    } else sc->measState = 3;
                    sc->measN = (int)oS.size();
                    static double lastLog = 0;
                    if (now - lastLog > 5.0) { lastLog = now; Log("Sonos-Messung: %.0f ms +-%.0f (n=%d, Status %d)", sc->measLatMs.load(), sc->measErrMs.load(), (int)oS.size(), sc->measState.load()); }
                } else sc->measN = (int)oS.size();
                if (!everInc && now - tStart > 15.0) sc->measState = 3;  // Sonos liefert keine laufende Position
            }
        }
    }
    if (started) {
        std::string o, e;
        SoapCall(sc->ip, AVT, "AVTransport", "Stop", "<InstanceID>0</InstanceID>", o, e);
    }
    for (const std::string& ip : joined) {   // Gruppenmitglieder wieder eigenstaendig machen
        std::string o, e;
        SoapCall(ip, AVT, "AVTransport", "BecomeCoordinatorOfStandaloneGroup", "<InstanceID>0</InstanceID>", o, e);
    }
    sc->ctl = 0;
    return 0;
}

static bool StreamStart(StreamCtx* sc, double now, std::wstring& err) {
    if (sc->running) return true;
    sc->ringBytes = (size_t)sc->rate * (sc->Flac() ? 6 : 4) * 24;
    sc->ring.assign(sc->ringBytes, 0);
    sc->wr = 0;
    sc->stop = false;
    sc->bytesSent = 0;
    sc->measState = 0;
    sc->measN = 0;
    sc->frames.clear();
    // Historie mit Stille vorbelegen (9 s), damit jeder Client sofort einen Vorlauf bekommt
    uint64_t pre = ((uint64_t)(9.0 * sc->rate) / 4096 + 1) * 4096;
    sc->t0 = now - (double)pre / (double)sc->rate;
    sc->framesOut = pre;
    sc->fSamples = 0;
    sc->fFrameNo = 0;
    sc->fFill = 0;
    if (sc->Flac()) {
        sc->hdr = flac::StreamHeader(sc->rate, 24, 4096);
        sc->fL.assign(4096, 0);
        sc->fR.assign(4096, 0);
        std::vector<uint8_t> fr;
        for (uint64_t k = 0; k < pre / 4096; k++) {
            fr.clear();
            flac::EncodeFrame(fr, sc->fL.data(), sc->fR.data(), 4096, 24, sc->rate, sc->fFrameNo++);
            StreamPushFlacFrame(sc, fr, sc->fSamples);
            sc->fSamples += 4096;
        }
    } else {
        sc->wr = pre * 4;  // Ring ist bereits mit Nullen gefuellt
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = L"Socket konnte nicht erstellt werden"; return false; }
    int ex = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&ex, sizeof(ex));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)sc->port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
        closesocket(s);
        err = L"Port " + std::to_wstring(sc->port) + L" nicht verfügbar";
        return false;
    }
    sc->lis = s;
    sc->accThread = CreateThread(nullptr, 0, AcceptProc, sc, 0, nullptr);
    sc->ctlThread = CreateThread(nullptr, 0, ControlProc, sc, 0, nullptr);
    sc->running = true;
    Log("Stream-Server läuft auf Port %d (%s, %d Hz)", sc->port, sc->Flac() ? "FLAC 24 Bit" : "WAV 16 Bit", sc->rate);
    return true;
}

static void StreamStop(StreamCtx* sc) {
    if (!sc->running) return;
    sc->stop = true;
    if (sc->lis != INVALID_SOCKET) { closesocket(sc->lis); sc->lis = INVALID_SOCKET; }
    EnterCriticalSection(&sc->cs);
    for (SOCKET c : sc->clientSocks) shutdown(c, SD_BOTH);
    LeaveCriticalSection(&sc->cs);
    if (sc->accThread) { WaitForSingleObject(sc->accThread, 2000); CloseHandle(sc->accThread); sc->accThread = nullptr; }
    if (sc->ctlThread) { WaitForSingleObject(sc->ctlThread, 6000); CloseHandle(sc->ctlThread); sc->ctlThread = nullptr; }
    for (int i = 0; i < 100 && sc->nClients.load() > 0; i++) Sleep(20);
    sc->running = false;
    sc->ctl = 0;
    Log("Stream-Server gestoppt");
}
