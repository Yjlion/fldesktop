#include "crypto.h"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstring>
#include <memory>

namespace crypto {

Bytes random_bytes(size_t n) {
  Bytes b(n, '\0');
  if (n && RAND_bytes((unsigned char *)&b[0], (int)n) != 1) b.clear();
  return b;
}

static std::string hex(const unsigned char *d, size_t n) {
  static const char *x = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += x[d[i] >> 4];
    s += x[d[i] & 15];
  }
  return s;
}

std::string sha256_hex(const Bytes &data) {
  unsigned char md[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char *)data.data(), data.size(), md);
  return hex(md, sizeof md);
}

std::string hmac_sha256_hex(const Bytes &key, const Bytes &data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), key.data(), (int)key.size(), (const unsigned char *)data.data(), data.size(), md, &len);
  return hex(md, len);
}

void wipe(Bytes &b) {
  if (!b.empty()) OPENSSL_cleanse(&b[0], b.size());
  b.clear();
}

bool scrypt(const Bytes &password, const Kdf &kdf, Bytes &key) {
  key.assign(32, '\0');
  // 256 MiB of memory at most
  return EVP_PBE_scrypt(password.data(), password.size(), (const unsigned char *)kdf.salt.data(), kdf.salt.size(),
                        kdf.n, kdf.r, kdf.p, 256u << 20, (unsigned char *)&key[0], key.size()) == 1;
}

using Ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

bool seal(const Bytes &key, const Bytes &plain, const Bytes &aad, Bytes &out) {
  if (key.size() != 32) return false;
  Bytes nonce = random_bytes(12);
  Ctx c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  int len = 0;
  Bytes ct(plain.size() + 16, '\0');
  if (!c || nonce.size() != 12 ||
      EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, (const unsigned char *)key.data(),
                         (const unsigned char *)nonce.data()) != 1 ||
      (!aad.empty() && EVP_EncryptUpdate(c.get(), nullptr, &len, (const unsigned char *)aad.data(), (int)aad.size()) != 1) ||
      EVP_EncryptUpdate(c.get(), (unsigned char *)&ct[0], &len, (const unsigned char *)plain.data(), (int)plain.size()) != 1)
    return false;
  int total = len;
  if (EVP_EncryptFinal_ex(c.get(), (unsigned char *)&ct[0] + total, &len) != 1) return false;
  total += len;
  ct.resize((size_t)total);
  unsigned char tag[16];
  if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) return false;
  out = nonce + ct + Bytes((const char *)tag, 16);
  return true;
}

bool open(const Bytes &key, const Bytes &sealed, const Bytes &aad, Bytes &plain) {
  if (key.size() != 32 || sealed.size() < 28) return false;
  Bytes nonce = sealed.substr(0, 12), tag = sealed.substr(sealed.size() - 16);
  Bytes ct = sealed.substr(12, sealed.size() - 28);
  Ctx c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  int len = 0;
  plain.assign(ct.size() + 16, '\0');
  if (!c ||
      EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, (const unsigned char *)key.data(),
                         (const unsigned char *)nonce.data()) != 1 ||
      (!aad.empty() && EVP_DecryptUpdate(c.get(), nullptr, &len, (const unsigned char *)aad.data(), (int)aad.size()) != 1) ||
      EVP_DecryptUpdate(c.get(), (unsigned char *)&plain[0], &len, (const unsigned char *)ct.data(), (int)ct.size()) != 1) {
    wipe(plain);
    return false;
  }
  int total = len;
  if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, 16, (void *)tag.data()) != 1 ||
      EVP_DecryptFinal_ex(c.get(), (unsigned char *)&plain[0] + total, &len) != 1) {
    wipe(plain); // wrong key (password) or a damaged file
    return false;
  }
  plain.resize((size_t)(total + len));
  return true;
}

// ---- Diffie-Hellman, RFC 2409 second Oakley group (1024-bit MODP) ----

static const char *kPrime =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF";

static Bytes bn_bytes(const BIGNUM *n, size_t pad) {
  Bytes b(pad, '\0');
  if (BN_bn2binpad(n, (unsigned char *)&b[0], (int)pad) < 0) b.clear();
  return b;
}

bool dh_generate(DhKeys &k) {
  BIGNUM *p = nullptr, *g = BN_new(), *x = BN_new(), *y = BN_new();
  BN_CTX *ctx = BN_CTX_new();
  bool ok = BN_hex2bn(&p, kPrime) && g && x && y && ctx && BN_set_word(g, 2) &&
            BN_rand(x, 1023, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) && BN_mod_exp(y, g, x, p, ctx);
  if (ok) {
    k.priv = bn_bytes(x, 128);
    k.pub = bn_bytes(y, 128);
    ok = k.priv.size() == 128 && k.pub.size() == 128;
  }
  BN_clear_free(x);
  BN_free(y);
  BN_free(g);
  BN_free(p);
  BN_CTX_free(ctx);
  return ok;
}

bool dh_session_key(const DhKeys &mine, const Bytes &peer_pub, Bytes &key16) {
  BIGNUM *p = nullptr, *x = BN_bin2bn((const unsigned char *)mine.priv.data(), (int)mine.priv.size(), nullptr);
  BIGNUM *peer = BN_bin2bn((const unsigned char *)peer_pub.data(), (int)peer_pub.size(), nullptr);
  BIGNUM *s = BN_new(), *one = BN_new(), *pm1 = BN_new();
  BN_CTX *ctx = BN_CTX_new();
  bool ok = BN_hex2bn(&p, kPrime) && x && peer && s && one && pm1 && ctx && BN_one(one) && BN_sub(pm1, p, one) &&
            // the peer's key must be in 2 .. p-2
            BN_cmp(peer, one) > 0 && BN_cmp(peer, pm1) < 0 && BN_mod_exp(s, peer, x, p, ctx);
  Bytes shared;
  if (ok) shared = bn_bytes(s, 128); // padded to the prime's size, as libsecret does
  BN_clear_free(x);
  BN_clear_free(s);
  BN_free(peer);
  BN_free(p);
  BN_free(one);
  BN_free(pm1);
  BN_CTX_free(ctx);
  if (!ok || shared.size() != 128) return false;
  // HKDF-SHA256, no salt, no info, 16 bytes
  EVP_PKEY_CTX *pc = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  key16.assign(16, '\0');
  size_t len = 16;
  ok = pc && EVP_PKEY_derive_init(pc) > 0 && EVP_PKEY_CTX_set_hkdf_md(pc, EVP_sha256()) > 0 &&
       EVP_PKEY_CTX_set1_hkdf_key(pc, (const unsigned char *)shared.data(), (int)shared.size()) > 0 &&
       EVP_PKEY_derive(pc, (unsigned char *)&key16[0], &len) > 0 && len == 16;
  EVP_PKEY_CTX_free(pc);
  wipe(shared);
  return ok;
}

bool aes128_cbc_encrypt(const Bytes &key, const Bytes &plain, Bytes &iv, Bytes &cipher) {
  if (key.size() != 16) return false;
  iv = random_bytes(16);
  Ctx c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  cipher.assign(plain.size() + 16, '\0');
  int len = 0, total = 0;
  if (!c || iv.size() != 16 ||
      EVP_EncryptInit_ex(c.get(), EVP_aes_128_cbc(), nullptr, (const unsigned char *)key.data(),
                         (const unsigned char *)iv.data()) != 1 ||
      EVP_EncryptUpdate(c.get(), (unsigned char *)&cipher[0], &len, (const unsigned char *)plain.data(),
                        (int)plain.size()) != 1)
    return false;
  total = len;
  if (EVP_EncryptFinal_ex(c.get(), (unsigned char *)&cipher[0] + total, &len) != 1) return false;
  cipher.resize((size_t)(total + len));
  return true;
}

bool aes128_cbc_decrypt(const Bytes &key, const Bytes &iv, const Bytes &cipher, Bytes &plain) {
  if (key.size() != 16 || iv.size() != 16 || cipher.empty() || cipher.size() % 16) return false;
  Ctx c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  plain.assign(cipher.size() + 16, '\0');
  int len = 0, total = 0;
  if (!c ||
      EVP_DecryptInit_ex(c.get(), EVP_aes_128_cbc(), nullptr, (const unsigned char *)key.data(),
                         (const unsigned char *)iv.data()) != 1 ||
      EVP_DecryptUpdate(c.get(), (unsigned char *)&plain[0], &len, (const unsigned char *)cipher.data(),
                        (int)cipher.size()) != 1) {
    wipe(plain);
    return false;
  }
  total = len;
  if (EVP_DecryptFinal_ex(c.get(), (unsigned char *)&plain[0] + total, &len) != 1) {
    wipe(plain);
    return false;
  }
  plain.resize((size_t)(total + len));
  return true;
}

std::string base64(const Bytes &b) {
  std::string out(4 * ((b.size() + 2) / 3) + 1, '\0');
  int n = EVP_EncodeBlock((unsigned char *)&out[0], (const unsigned char *)b.data(), (int)b.size());
  out.resize((size_t)n);
  return out;
}

bool unbase64(const std::string &s, Bytes &out) {
  if (s.size() % 4) return false;
  out.assign(s.size() / 4 * 3 + 1, '\0');
  int n = EVP_DecodeBlock((unsigned char *)&out[0], (const unsigned char *)s.data(), (int)s.size());
  if (n < 0) return false;
  size_t pad = 0;
  if (!s.empty() && s.back() == '=') pad++;
  if (s.size() > 1 && s[s.size() - 2] == '=') pad++;
  out.resize((size_t)n - pad);
  return true;
}

} // namespace crypto
