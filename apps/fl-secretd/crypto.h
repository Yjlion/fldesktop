// fl-secretd's cryptography, on OpenSSL:
//  - keyring files: scrypt (from the password) and AES-256-GCM
//  - the Secret Service's "dh-ietf1024-sha256-aes128-cbc-pkcs7" sessions:
//    Diffie-Hellman in the 1024-bit MODP group (RFC 2409 group 2), HKDF-SHA256
//    to a 128-bit key, AES-128-CBC with PKCS#7 padding
#pragma once
#include <cstdint>
#include <string>

namespace crypto {

using Bytes = std::string; // binary

Bytes random_bytes(size_t n);
std::string sha256_hex(const Bytes &data);
// a keyed hash for the attributes a locked keyring can still be searched by
std::string hmac_sha256_hex(const Bytes &key, const Bytes &data);
// overwrites (not optimized away)
void wipe(Bytes &b);

struct Kdf {
  Bytes salt;
  uint64_t n = 1 << 16; // scrypt cost: about 64 MiB, ~0.2 s
  uint32_t r = 8, p = 1;
};
bool scrypt(const Bytes &password, const Kdf &kdf, Bytes &key32);

// AES-256-GCM: nonce (12) + ciphertext + tag (16); aad is authenticated too
bool seal(const Bytes &key32, const Bytes &plain, const Bytes &aad, Bytes &out);
bool open(const Bytes &key32, const Bytes &sealed, const Bytes &aad, Bytes &plain);

// ---- Secret Service transport encryption ----
struct DhKeys {
  Bytes priv, pub; // big-endian
};
bool dh_generate(DhKeys &k);
// the session key from the peer's public key (16 bytes)
bool dh_session_key(const DhKeys &mine, const Bytes &peer_pub, Bytes &key16);
bool aes128_cbc_encrypt(const Bytes &key16, const Bytes &plain, Bytes &iv, Bytes &cipher); // makes the IV
bool aes128_cbc_decrypt(const Bytes &key16, const Bytes &iv, const Bytes &cipher, Bytes &plain);

std::string base64(const Bytes &b);
bool unbase64(const std::string &s, Bytes &out);

} // namespace crypto
