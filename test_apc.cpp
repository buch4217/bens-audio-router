#include "ap_crypto.h"
#include "ap_key.h"
#include <cstdio>
static std::vector<uint8_t> hx(const char* s) { std::vector<uint8_t> o; for (size_t i = 0; s[i] && s[i+1]; i += 2) { unsigned v; sscanf(s + i, "%2x", &v); o.push_back((uint8_t)v); } return o; }
static void ph(const char* t, const uint8_t* d, size_t n) { printf("%s ", t); for (size_t i = 0; i < n; i++) printf("%02x", d[i]); printf("\n"); }
int main(int argc, char** argv) {
    // argv: ct_hex(512) iv key cipher_hex data_hex
    apc::Rsa2048 rsa; rsa.init(AP_RSA_N_HEX, AP_RSA_D_HEX);
    uint8_t h[20]; const char* msg = "abc"; apc::sha1((const uint8_t*)msg, 3, h); ph("sha1", h, 20);
    auto ct = hx(argv[1]); std::vector<uint8_t> pt; bool ok = rsa.oaepDecrypt(ct.data(), ct.size(), pt); printf("oaep %d ", ok); for (auto b : pt) printf("%02x", b); printf("\n");
    auto key = hx(argv[2]), iv = hx(argv[3]), cipher = hx(argv[4]);
    apc::Aes128 aes; aes.setKey(key.data()); std::vector<uint8_t> out(cipher.size()); aes.cbcDecrypt(iv.data(), cipher.data(), out.data(), cipher.size()); ph("aes", out.data(), out.size());
    auto data = hx(argv[5]); uint8_t sig[256]; rsa.signRaw(data.data(), data.size(), sig); ph("sig", sig, 256);
    std::string b = apc::b64enc(data.data(), data.size()); printf("b64 %s\n", b.c_str());
    return 0;
}
