// fl-keyring: parsing the output of ssh-keygen and gpg (unit tested)
#pragma once
#include <string>
#include <vector>

namespace keyring {

struct SshKey {
  std::string path;        // the private key (~/.ssh/id_ed25519); "" if only .pub
  std::string pub_path;    // ~/.ssh/id_ed25519.pub
  std::string type;        // ED25519, RSA, ECDSA
  int bits = 0;
  std::string fingerprint; // SHA256:...
  std::string comment;
};

// "256 SHA256:abc... user@host (ED25519)" (ssh-keygen -l -f)
bool parse_ssh_fingerprint(const std::string &line, SshKey &k);
// The key files in a folder (~/.ssh): every *.pub with its private key next to
// it if there is one. Fingerprints are not filled in.
std::vector<SshKey> find_ssh_keys(const std::string &dir);

struct GpgKey {
  bool secret = false;     // there is a secret key
  std::string fingerprint; // 40 hex digits
  std::string key_id;      // the last 16
  std::string algorithm;   // "RSA 3072", "EdDSA", ...
  long long created = 0, expires = 0; // epoch seconds, 0 = never
  bool revoked = false, expired = false;
  std::vector<std::string> uids; // "Name <mail>"
};

// gpg --list-keys --with-colons (and --list-secret-keys): pub/sec, fpr, uid
std::vector<GpgKey> parse_gpg_colons(const std::string &out);
// merges the secret keys' list into the public one (secret = true)
void mark_secret(std::vector<GpgKey> &keys, const std::vector<GpgKey> &secret);
// gpg's \x3a style escapes in uids
std::string gpg_unescape(const std::string &s);

// A valid name for a new key file in ~/.ssh: letters, digits, . _ -
bool valid_key_name(const std::string &name);

// Secret Service attributes shown as one line: "service: x, username: y"
std::string attributes_text(const std::vector<std::pair<std::string, std::string>> &attrs);

} // namespace keyring
