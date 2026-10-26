// webwin.h – Windows-Teil des Web-Interfaces: Listener, TLS (SChannel), lokale CA + Server-Zertifikat (CryptoAPI). MIT, © Ben Buchholz · Buccloud Software (DE)
// Keine Fremdbibliothek: nur Windows-Bordmittel (ws2_32, secur32, crypt32, bcrypt, iphlpapi). Nur HTTPS - es gibt keinen HTTP-Listener.
#pragma once
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <security.h>
#include <schnlsp.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <array>
#include "webcore.h"

namespace webwin {
inline std::function<void(const std::string&)>& LogHook() { static std::function<void(const std::string&)> f; return f; }
inline void Log(const std::string& s) { if (LogHook()) LogHook()(s); }
inline std::string Hx(unsigned long v) { char b[16]; snprintf(b, 16, "0x%08lX", v); return b; }
inline void InitRandom() { web::RngHook() = [](uint8_t* p, size_t n) { if (BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) abort(); }; }

// ---------------------------------------------------------------- private Adressen (nur Heimnetz)
inline bool IsPrivateIp(std::string ip) {
    if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7);
    unsigned a, b, c, d; if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
    std::string l; for (char ch : ip) l += (char)tolower((unsigned char)ch); return l == "::1" || l.rfind("fc", 0) == 0 || l.rfind("fd", 0) == 0 || l.rfind("fe8", 0) == 0 || l.rfind("fe9", 0) == 0 || l.rfind("fea", 0) == 0 || l.rfind("feb", 0) == 0;
}
inline std::vector<std::string> LocalIPv4() {
    std::vector<std::string> v; ULONG len = 0; GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &len); std::vector<char> buf(len + 1024);
    auto* aa = (IP_ADAPTER_ADDRESSES*)buf.data(); if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &len) != NO_ERROR) return v;
    for (; aa; aa = aa->Next) { if (aa->OperStatus != IfOperStatusUp || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue; for (auto* u = aa->FirstUnicastAddress; u; u = u->Next) { char ip[64]; auto* sa = (sockaddr_in*)u->Address.lpSockaddr; if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip) && IsPrivateIp(ip)) v.push_back(ip); } }
    return v;
}
inline std::string HostName() { char h[256] = {}; gethostname(h, 255); return h; }

// ---------------------------------------------------------------- lokale CA + Server-Zertifikat
struct Certs { PCCERT_CONTEXT server = nullptr; std::string caPem, names, err; };
namespace detail {
static const wchar_t* CA_CONT = L"BensAudioRouterCA"; static const wchar_t* SRV_CONT = L"BensAudioRouterWeb";
inline std::string Rd(const std::wstring& p) { std::string r; HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); if (f == INVALID_HANDLE_VALUE) return r; char b[4096]; DWORD k; while (ReadFile(f, b, sizeof b, &k, nullptr) && k) r.append(b, k); CloseHandle(f); return r; }
inline bool Wr(const std::wstring& p, const std::string& d) { HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (f == INVALID_HANDLE_VALUE) return false; DWORD k; bool ok = WriteFile(f, d.data(), (DWORD)d.size(), &k, nullptr) && k == d.size(); CloseHandle(f); return ok; }
inline HCRYPTPROV Prov(const wchar_t* cont, bool create) {
    HCRYPTPROV h = 0; if (CryptAcquireContextW(&h, cont, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, 0)) return h;
    if (create && GetLastError() == (DWORD)NTE_BAD_KEYSET && CryptAcquireContextW(&h, cont, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET)) return h; return 0; }
inline void DropProv(const wchar_t* cont) { HCRYPTPROV h; CryptAcquireContextW(&h, cont, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET); }
inline std::vector<BYTE> Name(const std::wstring& s) { DWORD n = 0; CertStrToNameW(X509_ASN_ENCODING, s.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &n, nullptr); std::vector<BYTE> b(n); CertStrToNameW(X509_ASN_ENCODING, s.c_str(), CERT_X500_NAME_STR, nullptr, b.data(), &n, nullptr); b.resize(n); return b; }
inline std::vector<BYTE> Enc(LPCSTR type, const void* info) { DWORD n = 0; std::vector<BYTE> b; if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, info, 0, nullptr, nullptr, &n)) return b; b.resize(n); CryptEncodeObjectEx(X509_ASN_ENCODING, type, info, 0, nullptr, b.data(), &n); b.resize(n); return b; }
inline FILETIME Plus(int days) { SYSTEMTIME st; GetSystemTime(&st); FILETIME ft; SystemTimeToFileTime(&st, &ft); ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; u.QuadPart += (ULONGLONG)((LONGLONG)days * 86400LL * 10000000LL); ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart; return ft; }
inline std::string Pem(const std::string& der) { std::string b = web::B64(der), o = "-----BEGIN CERTIFICATE-----\n"; for (size_t i = 0; i < b.size(); i += 64) o += b.substr(i, 64) + "\n"; return o + "-----END CERTIFICATE-----\n"; }
inline bool NewCa(const std::wstring& dir, std::string& err) {
    DropProv(CA_CONT); HCRYPTPROV hp = Prov(CA_CONT, true); if (!hp) { err = "CA-Schluesselbund: " + Hx(GetLastError()); return false; } HCRYPTKEY k = 0;
    if (!CryptGenKey(hp, AT_SIGNATURE, (2048u << 16), &k)) { err = "CA-Schluessel: " + Hx(GetLastError()); CryptReleaseContext(hp, 0); return false; } CryptDestroyKey(k);
    std::vector<BYTE> subj = Name(L"CN=Bens Audio Router Lokale CA, O=Buccloud Software (DE)"); CERT_NAME_BLOB sb = {(DWORD)subj.size(), subj.data()};
    CERT_BASIC_CONSTRAINTS2_INFO bc = {TRUE, TRUE, 0}; std::vector<BYTE> bce = Enc(X509_BASIC_CONSTRAINTS2, &bc); BYTE ku = CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE; CRYPT_BIT_BLOB kb = {1, &ku, 1}; std::vector<BYTE> kue = Enc(X509_KEY_USAGE, &kb);
    CERT_EXTENSION ex[2] = {{(LPSTR)szOID_BASIC_CONSTRAINTS2, TRUE, {(DWORD)bce.size(), bce.data()}}, {(LPSTR)szOID_KEY_USAGE, TRUE, {(DWORD)kue.size(), kue.data()}}}; CERT_EXTENSIONS exts = {2, ex};
    CRYPT_KEY_PROV_INFO kpi = {}; kpi.pwszContainerName = (LPWSTR)CA_CONT; kpi.pwszProvName = (LPWSTR)MS_ENH_RSA_AES_PROV_W; kpi.dwProvType = PROV_RSA_AES; kpi.dwKeySpec = AT_SIGNATURE; CRYPT_ALGORITHM_IDENTIFIER alg = {(LPSTR)szOID_RSA_SHA256RSA, {0, nullptr}};
    FILETIME nb = Plus(-1), na = Plus(3650); SYSTEMTIME s1, s2; FileTimeToSystemTime(&nb, &s1); FileTimeToSystemTime(&na, &s2);
    PCCERT_CONTEXT ca = CertCreateSelfSignCertificate(hp, &sb, 0, &kpi, &alg, &s1, &s2, &exts); CryptReleaseContext(hp, 0); if (!ca) { err = "CA-Zertifikat: " + Hx(GetLastError()); return false; }
    bool ok = Wr(dir + L"\\ca.cer", std::string((char*)ca->pbCertEncoded, ca->cbCertEncoded)); CertFreeCertificateContext(ca); if (!ok) err = "ca.cer nicht schreibbar"; return ok;
}
inline bool IssueServer(const std::wstring& dir, PCCERT_CONTEXT ca, const std::vector<std::string>& dns, const std::vector<std::string>& ips, std::string& err) {
    DropProv(SRV_CONT); HCRYPTPROV hs = Prov(SRV_CONT, true); if (!hs) { err = "Server-Schluesselbund: " + Hx(GetLastError()); return false; } HCRYPTKEY k = 0;
    if (!CryptGenKey(hs, AT_KEYEXCHANGE, (2048u << 16), &k)) { err = "Server-Schluessel: " + Hx(GetLastError()); CryptReleaseContext(hs, 0); return false; } CryptDestroyKey(k);
    DWORD pl = 0; CryptExportPublicKeyInfo(hs, AT_KEYEXCHANGE, X509_ASN_ENCODING, nullptr, &pl); std::vector<BYTE> pkb(pl); auto* pki = (CERT_PUBLIC_KEY_INFO*)pkb.data();
    if (!CryptExportPublicKeyInfo(hs, AT_KEYEXCHANGE, X509_ASN_ENCODING, pki, &pl)) { err = "Public-Key-Export: " + Hx(GetLastError()); CryptReleaseContext(hs, 0); return false; } CryptReleaseContext(hs, 0);
    std::vector<std::wstring> dw; for (auto& d : dns) dw.push_back(std::wstring(d.begin(), d.end())); std::vector<CERT_ALT_NAME_ENTRY> ae; for (auto& w : dw) { CERT_ALT_NAME_ENTRY e = {}; e.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME; e.pwszDNSName = (LPWSTR)w.c_str(); ae.push_back(e); }
    std::vector<std::array<BYTE, 4>> ib; for (auto& s : ips) { in_addr a; if (inet_pton(AF_INET, s.c_str(), &a) == 1) { std::array<BYTE, 4> x; memcpy(x.data(), &a, 4); ib.push_back(x); } }
    for (auto& x : ib) { CERT_ALT_NAME_ENTRY e = {}; e.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS; e.IPAddress.cbData = 4; e.IPAddress.pbData = x.data(); ae.push_back(e); }
    CERT_ALT_NAME_INFO ani = {(DWORD)ae.size(), ae.data()}; std::vector<BYTE> sane = Enc(X509_ALTERNATE_NAME, &ani); LPSTR eku = (LPSTR)szOID_PKIX_KP_SERVER_AUTH; CERT_ENHKEY_USAGE eu = {1, &eku}; std::vector<BYTE> eue = Enc(X509_ENHANCED_KEY_USAGE, &eu);
    BYTE ku = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE; CRYPT_BIT_BLOB kb = {1, &ku, 5}; std::vector<BYTE> kue = Enc(X509_KEY_USAGE, &kb); CERT_BASIC_CONSTRAINTS2_INFO bc = {FALSE, FALSE, 0}; std::vector<BYTE> bce = Enc(X509_BASIC_CONSTRAINTS2, &bc);
    CERT_EXTENSION ex[4] = {{(LPSTR)szOID_SUBJECT_ALT_NAME2, FALSE, {(DWORD)sane.size(), sane.data()}}, {(LPSTR)szOID_ENHANCED_KEY_USAGE, FALSE, {(DWORD)eue.size(), eue.data()}}, {(LPSTR)szOID_KEY_USAGE, TRUE, {(DWORD)kue.size(), kue.data()}}, {(LPSTR)szOID_BASIC_CONSTRAINTS2, TRUE, {(DWORD)bce.size(), bce.data()}}};
    BYTE ser[16]; web::Random(ser, 16); ser[15] &= 0x7F; ser[15] |= 0x01; std::wstring cn = L"bens-audio-router.local"; std::vector<BYTE> subj = Name(L"CN=" + cn + L", O=Buccloud Software (DE)");
    CERT_INFO ci = {}; ci.dwVersion = CERT_V3; ci.SerialNumber = {16, ser}; ci.SignatureAlgorithm = {(LPSTR)szOID_RSA_SHA256RSA, {0, nullptr}}; ci.Issuer = ca->pCertInfo->Subject; ci.NotBefore = Plus(-1); ci.NotAfter = Plus(365); ci.Subject = {(DWORD)subj.size(), subj.data()}; ci.SubjectPublicKeyInfo = *pki; ci.cExtension = 4; ci.rgExtension = ex;
    HCRYPTPROV hc = Prov(CA_CONT, false); if (!hc) { err = "CA-Schluessel nicht verfuegbar: " + Hx(GetLastError()); return false; } CRYPT_ALGORITHM_IDENTIFIER alg = ci.SignatureAlgorithm; DWORD n = 0;
    if (!CryptSignAndEncodeCertificate(hc, AT_SIGNATURE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci, &alg, nullptr, nullptr, &n)) { err = "Signatur (Laenge): " + Hx(GetLastError()); CryptReleaseContext(hc, 0); return false; }
    std::vector<BYTE> der(n); if (!CryptSignAndEncodeCertificate(hc, AT_SIGNATURE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci, &alg, nullptr, der.data(), &n)) { err = "Signatur: " + Hx(GetLastError()); CryptReleaseContext(hc, 0); return false; } CryptReleaseContext(hc, 0); der.resize(n);
    if (!Wr(dir + L"\\server.cer", std::string((char*)der.data(), der.size()))) { err = "server.cer nicht schreibbar"; return false; } return true;
}
}  // namespace detail

// Stellt sicher, dass CA + Server-Zertifikat existieren (Neuausstellung bei geaenderten Adressen oder < 30 Tagen Restlaufzeit).
inline bool EnsureCerts(const std::wstring& dir, const std::vector<std::string>& ips, Certs& out) {
    using namespace detail; CreateDirectoryW(dir.c_str(), nullptr); std::string err; std::vector<std::string> dns = {"bens-audio-router.local", "localhost"}; std::string hn = HostName(); if (!hn.empty()) { dns.push_back(hn); dns.push_back(hn + ".local"); }
    std::vector<std::string> ipl = ips; ipl.push_back("127.0.0.1"); std::string names; for (auto& d : dns) names += d + ";"; for (auto& i : ipl) names += i + ";";
    std::string caDer = Rd(dir + L"\\ca.cer"); PCCERT_CONTEXT ca = caDer.empty() ? nullptr : CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)caDer.data(), (DWORD)caDer.size());
    HCRYPTPROV hp = Prov(CA_CONT, false); bool caOk = ca && hp;
    if (caOk) { DWORD l = 0; CryptExportPublicKeyInfo(hp, AT_SIGNATURE, X509_ASN_ENCODING, nullptr, &l); std::vector<BYTE> b(l); auto* p = (CERT_PUBLIC_KEY_INFO*)b.data(); caOk = CryptExportPublicKeyInfo(hp, AT_SIGNATURE, X509_ASN_ENCODING, p, &l) && CertComparePublicKeyInfo(X509_ASN_ENCODING, p, &ca->pCertInfo->SubjectPublicKeyInfo); }
    if (hp) CryptReleaseContext(hp, 0);
    if (!caOk) { if (ca) CertFreeCertificateContext(ca); ca = nullptr; if (!NewCa(dir, err)) { out.err = err; return false; } caDer = Rd(dir + L"\\ca.cer"); ca = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)caDer.data(), (DWORD)caDer.size()); Wr(dir + L"\\web_names.txt", ""); Log("Web: neue lokale CA erzeugt"); }
    if (!ca) { out.err = "CA-Zertifikat nicht lesbar"; return false; }
    std::string srvDer = Rd(dir + L"\\server.cer"); PCCERT_CONTEXT sc = srvDer.empty() ? nullptr : CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)srvDer.data(), (DWORD)srvDer.size());
    bool need = !sc || Rd(dir + L"\\web_names.txt") != names; if (sc && !need) { FILETIME soon = Plus(30); need = CompareFileTime(&sc->pCertInfo->NotAfter, &soon) < 0 || CertVerifyTimeValidity(nullptr, sc->pCertInfo) != 0; }
    if (need) { if (sc) CertFreeCertificateContext(sc); sc = nullptr; if (!IssueServer(dir, ca, dns, ipl, err)) { CertFreeCertificateContext(ca); out.err = err; return false; } Wr(dir + L"\\web_names.txt", names); srvDer = Rd(dir + L"\\server.cer"); sc = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)srvDer.data(), (DWORD)srvDer.size()); Log("Web: Server-Zertifikat ausgestellt fuer " + names); }
    if (!sc) { CertFreeCertificateContext(ca); out.err = "Server-Zertifikat nicht lesbar"; return false; }
    CRYPT_KEY_PROV_INFO kpi = {}; kpi.pwszContainerName = (LPWSTR)SRV_CONT; kpi.pwszProvName = (LPWSTR)MS_ENH_RSA_AES_PROV_W; kpi.dwProvType = PROV_RSA_AES; kpi.dwKeySpec = AT_KEYEXCHANGE;
    if (!CertSetCertificateContextProperty(sc, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi)) { out.err = "Schluessel-Zuordnung: " + Hx(GetLastError()); CertFreeCertificateContext(sc); CertFreeCertificateContext(ca); return false; }
    out.server = sc; out.caPem = Pem(caDer); out.names = names; CertFreeCertificateContext(ca); return true;
}

// ---------------------------------------------------------------- TLS-Verbindung (SChannel, Server-Seite)
class TlsConn : public web::Transport {
    SOCKET s_; CredHandle* cred_; CtxtHandle ctx_{}; bool ctxOk_ = false; SecPkgContext_StreamSizes sz_{}; std::string raw_, plain_, ip_; std::mutex cm_;
    bool SendRaw(const void* p, int n) { const char* c = (const char*)p; while (n > 0) { int k = send(s_, c, n, 0); if (k <= 0) return false; c += k; n -= k; } return true; }
public:
    TlsConn(SOCKET s, CredHandle* c, const std::string& ip) : s_(s), cred_(c), ip_(ip) {}
    ~TlsConn() override { if (ctxOk_) DeleteSecurityContext(&ctx_); if (s_ != INVALID_SOCKET) closesocket(s_); }
    bool Handshake() {
        const DWORD fl = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM; bool first = true, need = true;
        for (;;) {
            if (need) { char b[8192]; int r = recv(s_, b, sizeof b, 0); if (r <= 0) return false; raw_.append(b, r); need = false; if (raw_.size() > 65536) return false; }
            SecBuffer in[2] = {{(ULONG)raw_.size(), SECBUFFER_TOKEN, &raw_[0]}, {0, SECBUFFER_EMPTY, nullptr}}; SecBuffer out[1] = {{0, SECBUFFER_TOKEN, nullptr}}; SecBufferDesc id = {SECBUFFER_VERSION, 2, in}, od = {SECBUFFER_VERSION, 1, out}; DWORD at = 0; TimeStamp ts;
            SECURITY_STATUS st = AcceptSecurityContext(cred_, first ? nullptr : &ctx_, &id, fl, 0, &ctx_, &od, &at, &ts); first = false; if (st != SEC_E_INCOMPLETE_MESSAGE) ctxOk_ = true;
            if (out[0].cbBuffer && out[0].pvBuffer) { bool ok = SendRaw(out[0].pvBuffer, (int)out[0].cbBuffer); FreeContextBuffer(out[0].pvBuffer); if (!ok) return false; }
            if (st == SEC_E_INCOMPLETE_MESSAGE) { need = true; first = !ctxOk_; continue; }
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) { size_t extra = in[1].BufferType == SECBUFFER_EXTRA ? in[1].cbBuffer : 0; raw_ = extra ? raw_.substr(raw_.size() - extra) : std::string(); if (st == SEC_E_OK) return QueryContextAttributes(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sz_) == SEC_E_OK; need = raw_.empty(); continue; }
            Log("Web: TLS-Handshake fehlgeschlagen " + Hx((unsigned long)st) + " von " + ip_); return false;
        }
    }
    int Read(void* p, int n) override {
        for (;;) {
            if (!plain_.empty()) { int k = std::min<int>(n, (int)plain_.size()); memcpy(p, plain_.data(), k); plain_.erase(0, k); return k; }
            if (!raw_.empty()) {
                SecBuffer b[4] = {{(ULONG)raw_.size(), SECBUFFER_DATA, &raw_[0]}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}}; SecBufferDesc d = {SECBUFFER_VERSION, 4, b}; SECURITY_STATUS st; { std::lock_guard<std::mutex> l(cm_); st = DecryptMessage(&ctx_, &d, 0, nullptr); }
                if (st == SEC_E_OK) { size_t extra = 0; for (int i = 1; i < 4; i++) { if (b[i].BufferType == SECBUFFER_DATA) plain_.append((char*)b[i].pvBuffer, b[i].cbBuffer); if (b[i].BufferType == SECBUFFER_EXTRA) extra = b[i].cbBuffer; } raw_ = extra ? raw_.substr(raw_.size() - extra) : std::string(); continue; }
                if (st == SEC_I_CONTEXT_EXPIRED) return 0; if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;
            }
            char tmp[16384]; int r = recv(s_, tmp, sizeof tmp, 0); if (r == 0) return 0; if (r < 0) return -1; raw_.append(tmp, r); if (raw_.size() > 262144) return -1;
        }
    }
    bool WriteAll(const void* p, int n) override {
        const char* c = (const char*)p;
        while (n > 0) { int chunk = std::min<int>(n, (int)sz_.cbMaximumMessage); std::string buf(sz_.cbHeader + chunk + sz_.cbTrailer, 0); memcpy(&buf[sz_.cbHeader], c, chunk);
            SecBuffer b[4] = {{sz_.cbHeader, SECBUFFER_STREAM_HEADER, &buf[0]}, {(ULONG)chunk, SECBUFFER_DATA, &buf[sz_.cbHeader]}, {sz_.cbTrailer, SECBUFFER_STREAM_TRAILER, &buf[sz_.cbHeader + chunk]}, {0, SECBUFFER_EMPTY, nullptr}}; SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
            SECURITY_STATUS st; { std::lock_guard<std::mutex> l(cm_); st = EncryptMessage(&ctx_, 0, &d, 0); } if (st != SEC_E_OK) return false; if (!SendRaw(buf.data(), (int)(b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer))) return false; c += chunk; n -= chunk; }
        return true;
    }
    void SetTimeout(int ms) override { DWORD t = ms; setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t); setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t); }
    void Close() override { if (s_ != INVALID_SOCKET) shutdown(s_, SD_BOTH); }
    std::string PeerIp() override { return ip_; }
};

// ---------------------------------------------------------------- Listener
class Host {
    SOCKET ls_ = INVALID_SOCKET; CredHandle cred_{}; bool credOk_ = false; Certs certs_; web::Server* srv_ = nullptr; std::thread acc_; std::atomic<bool> run_{false}; std::atomic<int> live_{0}; int port_ = 0;
public:
    ~Host() { Stop(); }
    const Certs& certs() const { return certs_; } int port() const { return port_; } bool running() const { return run_; }
    bool Start(int port, web::Server* srv, const std::wstring& dir, std::string& err) {
        Stop(); WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); InitRandom(); srv_ = srv;
        if (!EnsureCerts(dir, LocalIPv4(), certs_)) { err = "Zertifikat: " + certs_.err; return false; } srv->SetCaPem(certs_.caPem);
        SCHANNEL_CRED sc = {}; sc.dwVersion = SCHANNEL_CRED_VERSION; sc.cCreds = 1; sc.paCred = &certs_.server; sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER; sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_NO_DEFAULT_CREDS; TimeStamp ts;
        SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_INBOUND, nullptr, &sc, nullptr, nullptr, &cred_, &ts); if (st != SEC_E_OK) { err = "TLS-Zugangsdaten: " + Hx((unsigned long)st); return false; } credOk_ = true;
        ls_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); if (ls_ == INVALID_SOCKET) { err = "Socket: " + std::to_string(WSAGetLastError()); return false; } DWORD off = 0; setsockopt(ls_, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off);
        sockaddr_in6 a = {}; a.sin6_family = AF_INET6; a.sin6_addr = in6addr_any; a.sin6_port = htons((u_short)port); if (bind(ls_, (sockaddr*)&a, sizeof a) != 0 || listen(ls_, 16) != 0) { err = "Port " + std::to_string(port) + " nicht verfuegbar (" + std::to_string(WSAGetLastError()) + ")"; closesocket(ls_); ls_ = INVALID_SOCKET; return false; }
        port_ = port; run_ = true; srv->Start(); acc_ = std::thread([this] { Accept(); }); Log("Web: HTTPS-Server laeuft auf Port " + std::to_string(port)); return true;
    }
    void Stop() {
        if (!run_ && ls_ == INVALID_SOCKET && !credOk_) return; run_ = false; if (ls_ != INVALID_SOCKET) { closesocket(ls_); ls_ = INVALID_SOCKET; } if (acc_.joinable()) acc_.join(); if (srv_) srv_->Stop();
        for (int i = 0; i < 150 && live_ > 0; i++) Sleep(20); if (credOk_) { FreeCredentialsHandle(&cred_); credOk_ = false; } if (certs_.server) { CertFreeCertificateContext(certs_.server); certs_.server = nullptr; }
    }
private:
    void Accept() {
        while (run_) {
            sockaddr_in6 pa; int pl = sizeof pa; SOCKET c = accept(ls_, (sockaddr*)&pa, &pl); if (c == INVALID_SOCKET) { if (!run_) break; Sleep(20); continue; }
            char ip[64] = {}; inet_ntop(AF_INET6, &pa.sin6_addr, ip, sizeof ip); if (!IsPrivateIp(ip) || live_ >= 24) { closesocket(c); continue; }
            BOOL nd = TRUE; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd); live_++;
            std::thread([this, c, s = std::string(ip)] { auto* t = new TlsConn(c, &cred_, s); t->SetTimeout(10000); if (t->Handshake()) srv_->Handle(std::unique_ptr<web::Transport>(t)); else delete t; live_--; }).detach();
        }
    }
};
}  // namespace webwin
