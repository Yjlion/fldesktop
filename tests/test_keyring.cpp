// fl-keyring: ssh-keygen and gpg output, key files
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

#include "fld/util.h"
#include "keyring_util.h"
#include "test.h"

using namespace keyring;

int main() {
  SshKey k;
  CHECK(parse_ssh_fingerprint("256 SHA256:Ab+cd/EF user@host (ED25519)\n", k));
  CHECK_EQ(k.bits, 256);
  CHECK_EQ(k.fingerprint, std::string("SHA256:Ab+cd/EF"));
  CHECK_EQ(k.comment, std::string("user@host"));
  CHECK_EQ(k.type, std::string("ED25519"));
  SshKey k2;
  CHECK(parse_ssh_fingerprint("3072 SHA256:xyz no comment (RSA)", k2));
  CHECK_EQ(k2.comment, std::string(""));
  CHECK_EQ(k2.type, std::string("RSA"));
  CHECK(!parse_ssh_fingerprint("ssh-keygen: not a key", k2));

  char tmpl[] = "/tmp/fld-keyring-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  fld::write_file(dir + "/id_ed25519.pub", "ssh-ed25519 AAAAC3Nz me@laptop\n");
  fld::write_file(dir + "/id_ed25519", "PRIVATE");
  fld::write_file(dir + "/work_rsa.pub", "ssh-rsa AAAAB3 work key\n");
  fld::write_file(dir + "/known_hosts", "x");
  auto keys = find_ssh_keys(dir);
  CHECK_EQ(keys.size(), (size_t)2);
  CHECK_EQ(keys[0].path, dir + "/id_ed25519");
  CHECK_EQ(keys[0].type, std::string("ED25519"));
  CHECK_EQ(keys[0].comment, std::string("me@laptop"));
  CHECK_EQ(keys[1].path, std::string("")); // public only
  CHECK_EQ(keys[1].comment, std::string("work key"));
  CHECK_EQ(keys[1].type, std::string("RSA"));
  std::string out;
  fld::run_capture({"rm", "-r", dir}, out);

  const char *colons =
      "tru::1:1700000000:0:3:1:5\n"
      "pub:u:255:22:1234567890ABCDEF:1700000000:1794608000::u:::scESC:::::ed25519:::0:\n"
      "fpr:::::::::0123456789ABCDEF01231234567890ABCDEF:\n"
      "uid:u::::1700000000::HASH::Jane Doe <jane@example.org>::::::::::0:\n"
      "uid:r::::1600000000::HASH::Old Name\\x3a revoked <old@example.org>::::::::::0:\n"
      "sub:u:255:18:FEDCBA0987654321:1700000000::::::e:::::cv25519::\n"
      "fpr:::::::::FFFFFFFFFFFFFFFFFFFFFEDCBA0987654321:\n"
      "pub:e:3072:1:AAAABBBBCCCCDDDD:1500000000:1600000000::-:::sc::::::::0:\n"
      "fpr:::::::::9999999999999999AAAABBBBCCCCDDDD:\n"
      "uid:e::::1500000000::HASH::Bob \\x3a) <bob@example.org>::::::::::0:\n";
  auto g = parse_gpg_colons(colons);
  CHECK_EQ(g.size(), (size_t)2);
  CHECK_EQ(g[0].fingerprint, std::string("0123456789ABCDEF01231234567890ABCDEF"));
  CHECK_EQ(g[0].key_id, std::string("1234567890ABCDEF"));
  CHECK_EQ(g[0].algorithm, std::string("EdDSA (ed25519)"));
  CHECK_EQ(g[0].uids.size(), (size_t)1); // the revoked uid is left out
  CHECK_EQ(g[0].uids[0], std::string("Jane Doe <jane@example.org>"));
  CHECK_EQ(g[0].expires, 1794608000LL);
  CHECK(!g[0].secret);
  CHECK(g[1].expired);
  CHECK_EQ(g[1].algorithm, std::string("RSA 3072"));
  CHECK_EQ(g[1].uids[0], std::string("Bob :) <bob@example.org>"));
  auto sec = parse_gpg_colons("sec:u:255:22:1234567890ABCDEF:1700000000::::::::::\n"
                              "fpr:::::::::0123456789ABCDEF01231234567890ABCDEF:\n");
  CHECK(sec[0].secret);
  mark_secret(g, sec);
  CHECK(g[0].secret);
  CHECK(!g[1].secret);

  CHECK(valid_key_name("id_ed25519"));
  CHECK(valid_key_name("work-2.key"));
  CHECK(!valid_key_name("../x"));
  CHECK(!valid_key_name(".hidden"));
  CHECK(!valid_key_name("a b"));
  CHECK(!valid_key_name("x.pub"));
  CHECK(!valid_key_name(""));
  CHECK_EQ(attributes_text({{"xdg:schema", "org.gnome.keyring.Network"}, {"server", "mail.example.org"},
                            {"user", "jane"}}),
           std::string("server: mail.example.org, user: jane"));
  return test_result("keyring");
}
