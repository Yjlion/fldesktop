// fl-secretd: keyring files (password, tampering, search while locked) and
// the transport encryption of Secret Service sessions
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "crypto.h"
#include "fld/util.h"
#include "store.h"
#include "test.h"

static void test_crypto() {
  // both ends of a dh-ietf1024-sha256-aes128-cbc-pkcs7 session get the same key
  crypto::DhKeys a, b;
  CHECK(crypto::dh_generate(a));
  CHECK(crypto::dh_generate(b));
  CHECK_EQ(a.pub.size(), (size_t)128);
  crypto::Bytes ka, kb;
  CHECK(crypto::dh_session_key(a, b.pub, ka));
  CHECK(crypto::dh_session_key(b, a.pub, kb));
  CHECK(ka == kb);
  CHECK_EQ(ka.size(), (size_t)16);
  // a public key of 1 is refused
  CHECK(!crypto::dh_session_key(a, std::string(127, '\0') + "\x01", ka));
  crypto::Bytes iv, ct, pt;
  CHECK(crypto::aes128_cbc_encrypt(kb, "hunter2", iv, ct));
  CHECK_EQ(ct.size(), (size_t)16); // PKCS#7: one block
  CHECK(crypto::aes128_cbc_decrypt(kb, iv, ct, pt));
  CHECK_EQ(pt, std::string("hunter2"));
  // AES-GCM: wrong key or changed data fail
  crypto::Bytes key = crypto::random_bytes(32), sealed, open;
  CHECK(crypto::seal(key, "secret data", "header", sealed));
  CHECK(crypto::open(key, sealed, "header", open) && open == "secret data");
  CHECK(!crypto::open(key, sealed, "other header", open));
  sealed[15] ^= 1;
  CHECK(!crypto::open(key, sealed, "header", open));
  crypto::Bytes b64;
  CHECK(crypto::unbase64(crypto::base64(std::string("\x00\xff\x10ab", 5)), b64) && b64 == std::string("\x00\xff\x10ab", 5));
}

static void test_store(const std::string &dir) {
  store::Collection c;
  std::string err;
  CHECK(store::create(c, dir, "login", "Login", "pw1", err));
  CHECK(!c.locked);
  store::Item it;
  it.id = c.new_id();
  it.label = "Mail";
  it.secret = "s3cret\n\x01";
  c.set_attributes(it, {{"service", "imap"}, {"user", "leo"}});
  c.items.push_back(it);
  CHECK(store::save(c, err));
  std::string text;
  CHECK(fld::read_file(dir + "/login.keyring", text));
  CHECK(text.find("s3cret") == std::string::npos);
  CHECK(text.find("imap") == std::string::npos);
  CHECK(text.find("Mail") == std::string::npos);

  // locked: found by the attribute hashes, nothing else known
  store::Collection l;
  CHECK(store::load(l, dir + "/login.keyring", err));
  CHECK(l.locked);
  CHECK_EQ(l.items.size(), (size_t)1);
  CHECK(l.matches(l.items[0], {{"service", "imap"}}));
  CHECK(!l.matches(l.items[0], {{"service", "pop"}}));
  CHECK(l.items[0].secret.empty());
  CHECK(!store::unlock(l, "wrong", err));
  CHECK_EQ(err, std::string("wrong password"));
  CHECK(store::unlock(l, "pw1", err));
  CHECK_EQ(l.items[0].secret, std::string("s3cret\n\x01"));
  CHECK_EQ(l.items[0].label, std::string("Mail"));
  CHECK_EQ(l.items[0].attributes["user"], std::string("leo"));
  CHECK(l.matches(l.items[0], {{"service", "imap"}, {"user", "leo"}}));

  // a changed header (hashes) is noticed: it is authenticated with the body
  std::string bad = text;
  size_t p = bad.find("\"label\":\"Login\"");
  CHECK(p != std::string::npos);
  if (p != std::string::npos) bad.replace(p, 15, "\"label\":\"Logon\"");
  fld::write_file(dir + "/bad.keyring", bad);
  store::Collection b;
  CHECK(store::load(b, dir + "/bad.keyring", err));
  CHECK(!store::unlock(b, "pw1", err));

  // a new password
  CHECK(store::change_password(l, "pw1", "pw2", err));
  store::Collection n;
  CHECK(store::load(n, dir + "/login.keyring", err));
  CHECK(!store::unlock(n, "pw1", err));
  CHECK(store::unlock(n, "pw2", err));
  // the key alone opens it (the login hands it over)
  crypto::Bytes key;
  store::Collection k;
  CHECK(store::load(k, dir + "/login.keyring", err) && store::derive_key(k, "pw2", key));
  CHECK(store::unlock_with_key(k, key, err) && k.items.size() == 1);
  store::lock(n);
  CHECK(n.locked && n.items[0].secret.empty());
  CHECK_EQ(store::name_for("My Passwords!", dir), std::string("my_passwords"));
  CHECK_EQ(store::name_for("Login", dir), std::string("login2"));
}

int main() {
  char tmpl[] = "/tmp/fld-secretd-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  test_crypto();
  test_store(dir);
  std::string out;
  fld::run_capture({"rm", "-rf", dir}, out);
  return test_result("secretd");
}
