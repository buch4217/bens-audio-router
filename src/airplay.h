// airplay.h - AirPlay-Empfaenger (RAOP / "AirPlay 1") fuer Bens Audio Router
// iPhone/iPad/Mac sehen den Router als AirPlay-Lautsprecher. Audio: Apple Lossless (ALAC, 16 Bit / 44,1 kHz).
//   * mDNS-Ankuendigung (_raop._tcp), eigener kleiner Responder
//   * RTSP-Server (OPTIONS inkl. Apple-Challenge, ANNOUNCE, SETUP, RECORD, SET_PARAMETER, FLUSH, TEARDOWN)
//   * RTP-Audio ueber UDP, AES-128-CBC-Entschluesselung, ALAC-Dekodierung, Wiederholungsanforderung (Resend)
//   * Der Router entnimmt die Audiodaten zeitgesteuert aus einer Warteschlange (Jitterpuffer)
// ALAC-Decoder: (c) 2005 David Hammerton (MIT), aus shairport-sync (MIT). Der RAOP-Schluessel ist oeffentlich dokumentiert.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "alac.h"
}
#include "ap_crypto.h"
#include "ap_key.h"

double NowSec();
void Log(const char* fmt, ...);

struct ApSession {
    uint8_t key[16] = {0}, iv[16] = {0};
    bool encrypted = false;
    alac_file* alac = nullptr;
    int framesPerPacket = 352;
    in_addr client = {};
    int cPortCtl = 0, cPortTim = 0;
    SOCKET sAudio = INVALID_SOCKET, sCtl = INVALID_SOCKET, sTim = INVALID_SOCKET;
    int pAudio = 0, pCtl = 0, pTim = 0;
    // Reihenfolge / Wiederholung
    bool haveExpect = false;
    int32_t expect = 0;
    std::map<int32_t, std::vector<float>> pending;   // ausstehende Pakete (erweiterte Sequenznummer)
    double gapSince = 0;
    uint64_t lastResendReq = 0;
};

struct AirplayCtx {
    // ---- Konfiguration (UI)
    std::wstring name = L"Bens Audio Router";
    int port = 5000;
    std::atomic<bool> latAuto{true};
    std::atomic<int> latManualMs{300};
    std::atomic<int> bufferMs{250};         // Jitterpuffer vor dem Start (ms)
    std::atomic<bool> useVol{true};         // Lautstaerke des iPhones uebernehmen
    int (*latencyFn)(void*) = nullptr;      // liefert die Ausgabelatenz (ms) fuer die "Auto"-Latenz
    void* latencyArg = nullptr;

    // ---- Laufzeit
    std::atomic<bool> stop{false}, running{false};
    std::atomic<int> state{0};              // 0 aus, 1 wartet, 2 verbunden
    std::atomic<int> mdnsState{0};          // 0 unbekannt, 1 aktiv, 2 Port 5353 nicht verfuegbar
    CRITICAL_SECTION cs;
    std::string clientIp;
    std::atomic<uint64_t> packets{0}, lost{0}, resent{0};
    std::atomic<float> volDb{0.0f};
    std::atomic<int> reportedLatMs{0};

    // Warteschlange Netz -> Audio-Thread (einfacher Erzeuger/Verbraucher)
    std::vector<float> q;
    size_t qcap = 0;
    std::atomic<uint64_t> qw{0}, qr{0};
    std::atomic<bool> flushReq{false};
    // Verbraucher-Zustand (nur Audio-Thread)
    bool playing = false;
    double t0 = 0, lastPacketT = 0, lastFillT = 0;
    uint64_t consumed = 0;

    SOCKET lis = INVALID_SOCKET, mdns = INVALID_SOCKET;
    HANDLE thAcc = nullptr, thMdns = nullptr, thRtp = nullptr;
    ApSession* ses = nullptr;
    std::atomic<bool> rtpStop{false};
    uint8_t mac[6] = {0};
    std::vector<uint32_t> ips;              // lokale IPv4 (Netzwerk-Byte-Reihenfolge)

    AirplayCtx() { InitializeCriticalSection(&cs); }
    ~AirplayCtx() { DeleteCriticalSection(&cs); }
};

// ------------------------------------------------------------------ Hilfen
static std::string ApLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string ApTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string ApU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void ApLocalInfo(AirplayCtx* ap) {
    ap->ips.clear();
    memset(ap->mac, 0, 6);
    ULONG sz = 16384;
    std::vector<uint8_t> buf(sz);
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, fl, nullptr, aa, &sz);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(sz); aa = (IP_ADAPTER_ADDRESSES*)buf.data(); rc = GetAdaptersAddresses(AF_INET, fl, nullptr, aa, &sz); }
    bool gotMac = false;
    if (rc == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                ap->ips.push_back(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
                if (!gotMac && a->PhysicalAddressLength == 6) { memcpy(ap->mac, a->PhysicalAddress, 6); gotMac = true; }
            }
        }
    }
    if (ap->ips.empty()) ap->ips.push_back(htonl(INADDR_LOOPBACK));
    if (!gotMac) { uint8_t m[6] = {0x02, 0xBA, 0x0A, 0x01, 0x04, 0x00}; memcpy(ap->mac, m, 6); }
}

static std::string ApMacHex(const AirplayCtx* ap) {
    char b[16];
    snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", ap->mac[0], ap->mac[1], ap->mac[2], ap->mac[3], ap->mac[4], ap->mac[5]);
    return b;
}
static std::string ApInstance(const AirplayCtx* ap) {
    std::string n = ApU8(ap->name);
    std::string c;
    for (char ch : n) if (ch != '.' && (unsigned char)ch >= 32) c += ch;
    if (c.empty()) c = "Bens Audio Router";
    return ApMacHex(ap) + "@" + c;
}
static std::string ApHost(const AirplayCtx* ap) { return "Bens-Router-" + ApMacHex(ap).substr(6); }

// ------------------------------------------------------------------ mDNS
struct DnsW {
    std::vector<uint8_t> b;
    void u16(uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
    void u32(uint32_t v) { u16((uint16_t)(v >> 16)); u16((uint16_t)v); }
    void name(const std::string& n) {   // "a.b.c" (Labels duerfen Leerzeichen enthalten)
        size_t p = 0;
        while (p < n.size()) {
            size_t e = n.find('.', p);
            if (e == std::string::npos) e = n.size();
            size_t l = std::min<size_t>(e - p, 63);
            b.push_back((uint8_t)l);
            b.insert(b.end(), n.begin() + p, n.begin() + p + l);
            p = e + 1;
        }
        b.push_back(0);
    }
    void rrHead(const std::string& n, uint16_t type, uint16_t cls, uint32_t ttl) { name(n); u16(type); u16(cls); u32(ttl); }
    size_t lenPos() { size_t p = b.size(); u16(0); return p; }
    void setLen(size_t p) { uint16_t l = (uint16_t)(b.size() - p - 2); b[p] = (uint8_t)(l >> 8); b[p + 1] = (uint8_t)l; }
};

static std::vector<std::string> ApTxt(const AirplayCtx* ap) {
    return {"txtvers=1", "ch=2", "cn=0,1", "da=true", "et=0,1", "ek=1", "md=0,1,2", "pw=false", "sv=false", "sr=44100", "ss=16",
            "tp=TCP,UDP", "vn=65537", "vs=105.1", "am=BensAudioRouter1,1", "sf=0x4", "fv=76400.10"};
}

// baut eine komplette Antwort (PTR + SRV + TXT + A); ttl=0 => "Goodbye"
static std::vector<uint8_t> ApMdnsPacket(const AirplayCtx* ap, uint32_t ttl, const std::vector<uint32_t>& ips) {
    DnsW w;
    std::string svc = "_raop._tcp.local", inst = ApInstance(ap) + "." + svc, host = ApHost(ap) + ".local";
    w.u16(0); w.u16(0x8400); w.u16(0); w.u16((uint16_t)(3 + ips.size())); w.u16(0); w.u16(0);
    w.rrHead(svc, 12, 1, ttl);
    size_t lp = w.lenPos(); w.name(inst); w.setLen(lp);
    w.rrHead(inst, 33, 0x8001, ttl);
    lp = w.lenPos(); w.u16(0); w.u16(0); w.u16((uint16_t)ap->port); w.name(host); w.setLen(lp);
    w.rrHead(inst, 16, 0x8001, ttl);
    lp = w.lenPos();
    for (const std::string& t : ApTxt(ap)) { w.b.push_back((uint8_t)t.size()); w.b.insert(w.b.end(), t.begin(), t.end()); }
    w.setLen(lp);
    for (uint32_t ip : ips) {
        w.rrHead(host, 1, 0x8001, ttl);
        w.u16(4);
        const uint8_t* p = (const uint8_t*)&ip;
        w.b.insert(w.b.end(), p, p + 4);
    }
    return w.b;
}

static bool ApDnsName(const uint8_t* p, int len, int& pos, std::string& out, int depth = 0) {
    out.clear();
    int ptr = pos;
    bool jumped = false;
    int guard = 0;
    while (guard++ < 128) {
        if (ptr >= len) return false;
        uint8_t l = p[ptr];
        if (l == 0) { if (!jumped) pos = ptr + 1; return true; }
        if ((l & 0xC0) == 0xC0) {
            if (ptr + 1 >= len || depth > 4) return false;
            int off = ((l & 0x3F) << 8) | p[ptr + 1];
            if (!jumped) pos = ptr + 2;
            jumped = true;
            ptr = off;
            continue;
        }
        if (ptr + 1 + l > len) return false;
        if (!out.empty()) out += '.';
        out.append((const char*)p + ptr + 1, l);
        ptr += 1 + l;
    }
    return false;
}

static void ApMdnsSend(AirplayCtx* ap, const std::vector<uint8_t>& pkt, const sockaddr_in* unicastTo) {
    if (ap->mdns == INVALID_SOCKET) return;
    if (unicastTo) { sendto(ap->mdns, (const char*)pkt.data(), (int)pkt.size(), 0, (const sockaddr*)unicastTo, sizeof(*unicastTo)); return; }
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    for (uint32_t ip : ap->ips) {
        in_addr ia;
        ia.s_addr = ip;
        setsockopt(ap->mdns, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof(ia));
        sendto(ap->mdns, (const char*)pkt.data(), (int)pkt.size(), 0, (const sockaddr*)&dst, sizeof(dst));
    }
}

static DWORD WINAPI ApMdnsProc(LPVOID p) {
    AirplayCtx* ap = (AirplayCtx*)p;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(5353);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) { Log("AirPlay: mDNS-Port 5353 nicht verfuegbar"); ap->mdnsState = 2; closesocket(s); return 0; }
    ap->mdnsState = 1;
    for (uint32_t ip : ap->ips) {
        ip_mreq mr = {};
        inet_pton(AF_INET, "224.0.0.251", &mr.imr_multiaddr);
        mr.imr_interface.s_addr = ip;
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof(mr));
    }
    int ttl = 255;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    ap->mdns = s;
    std::string inst = ApLower(ApInstance(ap) + "._raop._tcp.local"), svc = "_raop._tcp.local", host = ApLower(ApHost(ap) + ".local");
    int announced = 0;
    double nextAnn = NowSec();
    while (!ap->stop) {
        double now = NowSec();
        if (announced < 3 && now >= nextAnn) {   // Ankuendigung beim Start
            ApMdnsSend(ap, ApMdnsPacket(ap, 120, ap->ips), nullptr);
            announced++;
            nextAnn = now + 1.0;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv = {0, 200000};
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t b[1500];
        sockaddr_in from;
        int fl = sizeof(from);
        int n = recvfrom(s, (char*)b, sizeof(b), 0, (sockaddr*)&from, &fl);
        if (n < 12) continue;
        if (b[2] & 0x80) continue;   // Antwort, keine Anfrage
        int qd = (b[4] << 8) | b[5], pos = 12;
        bool hit = false;
        for (int i = 0; i < qd; i++) {
            std::string qn;
            if (!ApDnsName(b, n, pos, qn) || pos + 4 > n) break;
            pos += 4;
            qn = ApLower(qn);
            if (qn == svc || qn == inst || qn == host) hit = true;
        }
        if (!hit) continue;
        // Anfragen von Absendern mit anderem Quellport als 5353: direkt (unicast) beantworten
        bool legacy = ntohs(from.sin_port) != 5353;
        ApMdnsSend(ap, ApMdnsPacket(ap, 120, ap->ips), legacy ? &from : nullptr);
    }
    ApMdnsSend(ap, ApMdnsPacket(ap, 0, ap->ips), nullptr);   // Goodbye
    ap->mdns = INVALID_SOCKET;
    closesocket(s);
    return 0;
}

// ------------------------------------------------------------------ Audio-Warteschlange
static void ApQueuePush(AirplayCtx* ap, const float* st, size_t frames) {
    uint64_t w = ap->qw.load(std::memory_order_relaxed), r = ap->qr.load(std::memory_order_acquire);
    if (w - r + frames * 2 > ap->qcap) return;   // voll: verwerfen
    for (size_t i = 0; i < frames * 2; i++) ap->q[(size_t)((w + i) % ap->qcap)] = st[i];
    ap->qw.store(w + frames * 2, std::memory_order_release);
}

static void ApDeliver(AirplayCtx* ap, const std::vector<float>& fr) {
    ApQueuePush(ap, fr.data(), fr.size() / 2);
}

// ------------------------------------------------------------------ RTP
static void ApSendResend(AirplayCtx* ap, ApSession* s, uint16_t seq, uint16_t count) {
    if (s->sCtl == INVALID_SOCKET || !s->cPortCtl) return;
    uint8_t p[8] = {0x80, 0xD5, 0x00, 0x01, (uint8_t)(seq >> 8), (uint8_t)seq, (uint8_t)(count >> 8), (uint8_t)count};
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr = s->client;
    to.sin_port = htons((u_short)s->cPortCtl);
    sendto(s->sCtl, (const char*)p, 8, 0, (sockaddr*)&to, sizeof(to));
    ap->resent += count;
}

static bool ApDecodePacket(ApSession* s, const uint8_t* pl, int plen, std::vector<float>& out) {
    static thread_local apc::Aes128 aes;
    std::vector<uint8_t> dec((size_t)plen);
    if (s->encrypted) {
        aes.setKey(s->key);
        aes.cbcDecrypt(s->iv, pl, dec.data(), (size_t)plen);
    } else memcpy(dec.data(), pl, (size_t)plen);
    std::vector<int16_t> pcm((size_t)s->framesPerPacket * 2 + 64);
    int bytes = (int)(pcm.size() * sizeof(int16_t));   // Eingabe: Puffergroesse, Ausgabe: erzeugte Bytes
    alac_decode_frame(s->alac, dec.data(), pcm.data(), &bytes);
    int frames = bytes / 4;
    static int failLogged = 0;
    if (frames <= 0 || frames > s->framesPerPacket + 16) { if (failLogged++ < 3) Log("AirPlay: ALAC-Dekodierung fehlgeschlagen (bytes=%d, Paketnutzlast=%d)", bytes, plen); return false; }
    out.resize((size_t)frames * 2);
    for (int i = 0; i < frames * 2; i++) out[(size_t)i] = pcm[(size_t)i] / 32768.0f;
    return true;
}

static void ApOnAudio(AirplayCtx* ap, ApSession* s, const uint8_t* p, int len, bool resent) {
    if (len < 12 + 8) return;
    uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
    std::vector<float> fr;
    if (!ApDecodePacket(s, p + 12, len - 12, fr)) return;
    ap->packets++;
    (void)resent;
    if (!s->haveExpect) { s->expect = seq; s->haveExpect = true; s->pending.clear(); }
    int16_t diff = (int16_t)(seq - (uint16_t)s->expect);
    if (diff < 0) return;   // alt / doppelt
    int32_t ext = s->expect + diff;
    if (diff == 0) {
        ApDeliver(ap, fr);
        s->expect++;
    } else {
        if (diff > 512) { s->expect = ext; s->pending.clear(); ApDeliver(ap, fr); s->expect++; return; }   // grosser Sprung: neu aufsetzen
        if (s->pending.empty()) { s->gapSince = NowSec(); ApSendResend(ap, s, (uint16_t)s->expect, (uint16_t)diff); }
        s->pending[ext] = fr;
    }
    // zusammenhaengende Pakete freigeben
    for (;;) {
        auto it = s->pending.find(s->expect);
        if (it == s->pending.end()) break;
        ApDeliver(ap, it->second);
        s->pending.erase(it);
        s->expect++;
    }
    // zu lange Luecke: aufgeben (Verlust), mit Stille auffuellen
    if (!s->pending.empty() && (NowSec() - s->gapSince > 0.35 || s->pending.size() > 48)) {
        int32_t first = s->pending.begin()->first;
        int missing = first - s->expect;
        if (missing > 0 && missing < 512) {
            std::vector<float> sil((size_t)s->framesPerPacket * 2, 0.0f);
            for (int i = 0; i < missing; i++) ApDeliver(ap, sil);
            ap->lost += (uint64_t)missing;
        }
        s->expect = first;
        for (;;) {
            auto it = s->pending.find(s->expect);
            if (it == s->pending.end()) break;
            ApDeliver(ap, it->second);
            s->pending.erase(it);
            s->expect++;
        }
    }
}

static DWORD WINAPI ApRtpProc(LPVOID p) {
    AirplayCtx* ap = (AirplayCtx*)p;
    ApSession* s = ap->ses;
    double lastTim = 0;
    std::vector<uint8_t> buf(4096);
    while (!ap->rtpStop && !ap->stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s->sAudio, &rf);
        FD_SET(s->sCtl, &rf);
        FD_SET(s->sTim, &rf);
        timeval tv = {0, 100000};
        int r = select(0, &rf, nullptr, nullptr, &tv);
        double now = NowSec();
        if (r > 0) {
            if (FD_ISSET(s->sAudio, &rf)) {
                int n = recv(s->sAudio, (char*)buf.data(), (int)buf.size(), 0);
                if (n > 0 && (buf[1] & 0x7F) == 0x60) ApOnAudio(ap, s, buf.data(), n, false);
            }
            if (FD_ISSET(s->sCtl, &rf)) {
                int n = recv(s->sCtl, (char*)buf.data(), (int)buf.size(), 0);
                if (n > 4 && (buf[1] & 0x7F) == 0x56) ApOnAudio(ap, s, buf.data() + 4, n - 4, true);   // Wiederholung
            }
            if (FD_ISSET(s->sTim, &rf)) recv(s->sTim, (char*)buf.data(), (int)buf.size(), 0);
        }
        static double lastStat = 0;
        if (now - lastStat > 2.0) { lastStat = now; Log("AirPlay: Pakete %llu, verloren %llu, Resend %llu, Warteschlange %llu Frames", (unsigned long long)ap->packets.load(), (unsigned long long)ap->lost.load(), (unsigned long long)ap->resent.load(), (unsigned long long)((ap->qw.load() - ap->qr.load()) / 2)); }
        if (now - lastTim > 2.0 && s->cPortTim) {   // Zeitabgleich-Anfrage an den Sender
            lastTim = now;
            uint8_t t[32] = {0x80, 0xD2, 0x00, 0x07};
            uint64_t ntp = (uint64_t)(now * 4294967296.0);
            for (int i = 0; i < 8; i++) t[24 + i] = (uint8_t)(ntp >> (56 - 8 * i));
            sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_addr = s->client;
            to.sin_port = htons((u_short)s->cPortTim);
            sendto(s->sTim, (const char*)t, 32, 0, (sockaddr*)&to, sizeof(to));
        }
    }
    return 0;
}

static int ApBindUdp(SOCKET& sock, int startPort) {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return 0;
    int rb = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof(rb));
    for (int port = startPort; port < startPort + 200; port++) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)port);
        if (bind(sock, (sockaddr*)&a, sizeof(a)) == 0) return port;
    }
    closesocket(sock);
    sock = INVALID_SOCKET;
    return 0;
}

static void ApEndSession(AirplayCtx* ap) {
    ap->rtpStop = true;
    if (ap->thRtp) { WaitForSingleObject(ap->thRtp, 2000); CloseHandle(ap->thRtp); ap->thRtp = nullptr; }
    ApSession* s = ap->ses;
    ap->ses = nullptr;
    if (s) {
        if (s->sAudio != INVALID_SOCKET) closesocket(s->sAudio);
        if (s->sCtl != INVALID_SOCKET) closesocket(s->sCtl);
        if (s->sTim != INVALID_SOCKET) closesocket(s->sTim);
        if (s->alac) alac_free(s->alac);
        delete s;
    }
    ap->rtpStop = false;
    ap->flushReq = true;
    ap->state = ap->running ? 1 : 0;
    EnterCriticalSection(&ap->cs);
    ap->clientIp.clear();
    LeaveCriticalSection(&ap->cs);
}

// ------------------------------------------------------------------ RTSP
struct RtspReq {
    std::string method, uri, body;
    std::map<std::string, std::string> h;
    std::string get(const char* k) const { auto it = h.find(k); return it == h.end() ? "" : it->second; }
};

static bool ApReadReq(SOCKET s, RtspReq& rq, AirplayCtx* ap) {
    std::string buf;
    char tmp[4096];
    size_t hdrEnd = std::string::npos;
    while (!ap->stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv = {0, 300000};
        int r = select(0, &rf, nullptr, nullptr, &tv);
        if (r == 0) continue;
        if (r < 0) return false;
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        if (hdrEnd == std::string::npos) hdrEnd = buf.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            std::string head = buf.substr(0, hdrEnd);
            size_t clen = 0;
            size_t p = ApLower(head).find("content-length:");
            if (p != std::string::npos) clen = (size_t)atol(head.c_str() + p + 15);
            if (buf.size() >= hdrEnd + 4 + clen) {
                size_t le = head.find("\r\n");
                std::string line = head.substr(0, le);
                size_t a = line.find(' '), b = line.find(' ', a + 1);
                rq.method = line.substr(0, a);
                rq.uri = b == std::string::npos ? "" : line.substr(a + 1, b - a - 1);
                size_t pos = le + 2;
                while (le != std::string::npos && pos < head.size()) {
                    size_t e = head.find("\r\n", pos);
                    if (e == std::string::npos) e = head.size();
                    std::string hl = head.substr(pos, e - pos);
                    size_t c = hl.find(':');
                    if (c != std::string::npos) rq.h[ApLower(hl.substr(0, c))] = ApTrim(hl.substr(c + 1));
                    pos = e + 2;
                }
                rq.body = buf.substr(hdrEnd + 4, clen);
                return true;
            }
        }
    }
    return false;
}

static void ApReply(SOCKET s, const RtspReq& rq, const std::string& extra, const std::string& status = "200 OK", const std::string& body = "",
                    const char* ctype = nullptr) {
    std::string r = "RTSP/1.0 " + status + "\r\nCSeq: " + rq.get("cseq") + "\r\nServer: AirTunes/105.1\r\n" + extra;
    if (!body.empty()) {
        if (ctype) r += std::string("Content-Type: ") + ctype + "\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    } else r += "Content-Length: 0\r\n";
    r += "\r\n" + body;
    send(s, r.data(), (int)r.size(), 0);
}

static int ApAutoLatencyMs(AirplayCtx* ap) {
    int ms = ap->latAuto.load() ? (ap->latencyFn ? ap->latencyFn(ap->latencyArg) : 0) + ap->bufferMs.load() : ap->latManualMs.load();
    return std::max(0, std::min(4000, ms));
}

static void ApHandleAnnounce(AirplayCtx* ap, SOCKET s, const RtspReq& rq) {
    ApEndSession(ap);
    ApSession* ses = new ApSession();
    std::string fmtp, rsaKey, ivb64;
    size_t pos = 0;
    while (pos < rq.body.size()) {
        size_t e = rq.body.find('\n', pos);
        if (e == std::string::npos) e = rq.body.size();
        std::string ln = ApTrim(rq.body.substr(pos, e - pos));
        pos = e + 1;
        if (ln.compare(0, 7, "a=fmtp:") == 0) fmtp = ln.substr(7);
        else if (ln.compare(0, 12, "a=rsaaeskey:") == 0) rsaKey = ln.substr(12);
        else if (ln.compare(0, 9, "a=aesiv:") == 0 || ln.compare(0, 8, "a=aesiv:") == 0) ivb64 = ln.substr(ln.find(':') + 1);
    }
    int f[12] = {96, 352, 0, 16, 40, 10, 14, 2, 255, 0, 0, 44100};
    {
        std::vector<int> v;
        const char* c = fmtp.c_str();
        char* endp;
        while (*c && v.size() < 12) {
            long x = strtol(c, &endp, 10);
            if (endp == c) break;
            v.push_back((int)x);
            c = endp;
        }
        if (v.size() >= 12) for (int i = 0; i < 12; i++) f[i] = v[(size_t)i];
    }
    ses->framesPerPacket = f[1];
    if (!rsaKey.empty()) {
        static apc::Rsa2048 rsa;
        static bool init = false;
        if (!init) { rsa.init(AP_RSA_N_HEX, AP_RSA_D_HEX); init = true; }
        std::vector<uint8_t> ct = apc::b64dec(rsaKey), pt, iv = apc::b64dec(ivb64);
        if (rsa.oaepDecrypt(ct.data(), ct.size(), pt) && pt.size() >= 16 && iv.size() >= 16) {
            memcpy(ses->key, pt.data(), 16);
            memcpy(ses->iv, iv.data(), 16);
            ses->encrypted = true;
        } else { Log("AirPlay: AES-Schluessel konnte nicht entschluesselt werden"); delete ses; ApReply(s, rq, "", "400 Bad Request"); return; }
    }
    alac_file* a = alac_create(f[3], f[7]);
    if (!a) { delete ses; ApReply(s, rq, "", "500 Internal Server Error"); return; }
    a->setinfo_max_samples_per_frame = (uint32_t)f[1];
    a->setinfo_7a = (uint8_t)f[2];
    a->setinfo_sample_size = (uint8_t)f[3];
    a->setinfo_rice_historymult = (uint8_t)f[4];
    a->setinfo_rice_initialhistory = (uint8_t)f[5];
    a->setinfo_rice_kmodifier = (uint8_t)f[6];
    a->setinfo_7f = (uint8_t)f[7];
    a->setinfo_80 = (uint16_t)f[8];
    a->setinfo_82 = (uint32_t)f[9];
    a->setinfo_86 = (uint32_t)f[10];
    a->setinfo_8a_rate = (uint32_t)f[11];
    alac_allocate_buffers(a);
    ses->alac = a;
    sockaddr_in peer;
    int pl = sizeof(peer);
    if (getpeername(s, (sockaddr*)&peer, &pl) == 0) ses->client = peer.sin_addr;
    ap->ses = ses;
    char ipb[64] = "";
    inet_ntop(AF_INET, &ses->client, ipb, sizeof(ipb));
    EnterCriticalSection(&ap->cs);
    ap->clientIp = ipb;
    LeaveCriticalSection(&ap->cs);
    Log("AirPlay: ANNOUNCE von %s (%d Frames/Paket, %s)", ipb, ses->framesPerPacket, ses->encrypted ? "AES" : "unverschluesselt");
    ApReply(s, rq, "");
}

static void ApHandleSetup(AirplayCtx* ap, SOCKET s, const RtspReq& rq) {
    ApSession* ses = ap->ses;
    if (!ses) { ApReply(s, rq, "", "455 Method Not Valid in This State"); return; }
    std::string tr = rq.get("transport");
    auto num = [&](const char* k) {
        size_t p = tr.find(k);
        return p == std::string::npos ? 0 : atoi(tr.c_str() + p + strlen(k));
    };
    ses->cPortCtl = num("control_port=");
    ses->cPortTim = num("timing_port=");
    ses->pAudio = ApBindUdp(ses->sAudio, 6000);
    ses->pCtl = ApBindUdp(ses->sCtl, ses->pAudio + 1);
    ses->pTim = ApBindUdp(ses->sTim, ses->pCtl + 1);
    if (!ses->pAudio || !ses->pCtl || !ses->pTim) { ApReply(s, rq, "", "500 Internal Server Error"); return; }
    ap->rtpStop = false;
    ap->thRtp = CreateThread(nullptr, 0, ApRtpProc, ap, 0, nullptr);
    char t[200];
    snprintf(t, sizeof(t), "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%d;control_port=%d;timing_port=%d\r\nSession: 1\r\nAudio-Jack-Status: connected; type=analog\r\n",
             ses->pAudio, ses->pCtl, ses->pTim);
    ApReply(s, rq, t);
}

static void ApSession_Run(AirplayCtx* ap, SOCKET s) {
    RtspReq rq;
    bool owner = false;
    while (!ap->stop && ApReadReq(s, rq, ap)) {
        const std::string& m = rq.method;
        if (m == "OPTIONS") {
            std::string extra = "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
            std::string chal = rq.get("apple-challenge");
            if (!chal.empty()) {
                static apc::Rsa2048 rsa;
                static bool init = false;
                if (!init) { rsa.init(AP_RSA_N_HEX, AP_RSA_D_HEX); init = true; }
                std::vector<uint8_t> c = apc::b64dec(chal);
                uint8_t buf[32] = {0};
                size_t p = std::min<size_t>(c.size(), 16);
                memcpy(buf, c.data(), p);
                sockaddr_in me;
                int ml = sizeof(me);
                getsockname(s, (sockaddr*)&me, &ml);
                memcpy(buf + p, &me.sin_addr, 4);
                p += 4;
                memcpy(buf + p, ap->mac, 6);
                uint8_t sig[256];
                if (rsa.signRaw(buf, 32, sig)) extra += "Apple-Response: " + apc::b64enc(sig, 256, false) + "\r\n";
            }
            ApReply(s, rq, extra);
        } else if (m == "ANNOUNCE") {
            ApHandleAnnounce(ap, s, rq);
            owner = true;
        } else if (m == "SETUP") {
            ApHandleSetup(ap, s, rq);
        } else if (m == "RECORD") {
            Log("AirPlay: RECORD empfangen");
            Log("AirPlay: berechne Latenz (auto=%d)", (int)ap->latAuto.load());
            int lat = ApAutoLatencyMs(ap);
            Log("AirPlay: Latenz %d ms", lat);
            ap->reportedLatMs = lat;
            ap->state = 2;
            char h[160];
            snprintf(h, sizeof(h), "Audio-Latency: %d\r\nAudio-Jack-Status: connected; type=analog\r\n", (int)((int64_t)lat * 44100 / 1000));
            ApReply(s, rq, h);
            Log("AirPlay: RECORD, gemeldete Latenz %d ms", lat);
        } else if (m == "SET_PARAMETER") {
            std::string ct = ApLower(rq.get("content-type"));
            if (ct.find("text/parameters") != std::string::npos) {
                size_t p = rq.body.find("volume:");
                if (p != std::string::npos) ap->volDb = (float)atof(rq.body.c_str() + p + 7);
            }
            ApReply(s, rq, "");
        } else if (m == "GET_PARAMETER") {
            char b[64];
            snprintf(b, sizeof(b), "volume: %.6f\r\n", ap->volDb.load());
            ApReply(s, rq, "", "200 OK", b, "text/parameters");
        } else if (m == "FLUSH") {
            ap->flushReq = true;
            if (ap->ses) { ap->ses->haveExpect = false; ap->ses->pending.clear(); }
            ApReply(s, rq, "RTP-Info: seq=0;rtptime=0\r\n");
        } else if (m == "TEARDOWN") {
            ApReply(s, rq, "Connection: close\r\n");
            if (owner) { ApEndSession(ap); owner = false; }
            break;
        } else {
            ApReply(s, rq, "");
        }
        rq = RtspReq();
    }
    if (owner) ApEndSession(ap);
}

static DWORD WINAPI ApAcceptProc(LPVOID p) {
    AirplayCtx* ap = (AirplayCtx*)p;
    while (!ap->stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ap->lis, &rf);
        timeval tv = {0, 300000};
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(ap->lis, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        ApSession_Run(ap, c);   // eine Sitzung gleichzeitig; neue Verbindung wartet bis die alte endet
        closesocket(c);
    }
    return 0;
}

static bool ApStart(AirplayCtx* ap, std::wstring& err) {
    if (ap->running) return true;
    ap->stop = false;
    ApLocalInfo(ap);
    ap->qcap = (size_t)44100 * 2 * 8;
    ap->q.assign(ap->qcap, 0.0f);
    ap->qw = ap->qr = 0;
    ap->playing = false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = L"Socket-Fehler"; return false; }
    int ex = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&ex, sizeof(ex));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)ap->port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        closesocket(s);
        err = L"Port " + std::to_wstring(ap->port) + L" nicht verfügbar";
        return false;
    }
    ap->lis = s;
    ap->thAcc = CreateThread(nullptr, 0, ApAcceptProc, ap, 0, nullptr);
    ap->thMdns = CreateThread(nullptr, 0, ApMdnsProc, ap, 0, nullptr);
    ap->running = true;
    ap->state = 1;
    Log("AirPlay-Empfaenger '%s' laeuft auf Port %d", ApInstance(ap).c_str(), ap->port);
    return true;
}

static void ApStop(AirplayCtx* ap) {
    if (!ap->running) return;
    ap->stop = true;
    ap->rtpStop = true;
    if (ap->lis != INVALID_SOCKET) { closesocket(ap->lis); ap->lis = INVALID_SOCKET; }
    if (ap->thAcc) { WaitForSingleObject(ap->thAcc, 3000); CloseHandle(ap->thAcc); ap->thAcc = nullptr; }
    if (ap->thMdns) { WaitForSingleObject(ap->thMdns, 3000); CloseHandle(ap->thMdns); ap->thMdns = nullptr; }
    ApEndSession(ap);
    ap->running = false;
    ap->state = 0;
    Log("AirPlay-Empfaenger gestoppt");
}
