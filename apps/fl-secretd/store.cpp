#include "store.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/json.h"
#include "fld/util.h"

namespace store {

static const char *kMagic = "FLDKEYRING 1\n";

uint64_t now() { return (uint64_t)time(nullptr); }

Item *Collection::find(const std::string &id) {
  for (auto &i : items)
    if (i.id == id) return &i;
  return nullptr;
}

std::string Collection::hash(const std::string &n, const std::string &v) const {
  return crypto::hmac_sha256_hex(hash_salt, n + std::string(1, '\0') + v);
}

bool Collection::matches(const Item &it, const Attributes &q) const {
  for (auto &kv : q) {
    if (!locked) {
      auto a = it.attributes.find(kv.first);
      if (a == it.attributes.end() || a->second != kv.second) return false;
    } else {
      auto h = it.hashed.find(kv.first);
      if (h == it.hashed.end() || h->second != hash(kv.first, kv.second)) return false;
    }
  }
  return true;
}

void Collection::set_attributes(Item &it, const Attributes &a) {
  it.attributes = a;
  it.hashed.clear();
  for (auto &kv : a) it.hashed[kv.first] = hash(kv.first, kv.second);
}

std::string Collection::new_id() {
  for (;;) {
    std::string id = std::to_string(next_id++);
    if (!find(id)) return id;
  }
}

std::string name_for(const std::string &label, const std::string &dir) {
  std::string base;
  for (char c : label) {
    if (isalnum((unsigned char)c)) base += (char)tolower((unsigned char)c);
    else if (!base.empty() && base.back() != '_') base += '_';
  }
  while (!base.empty() && base.back() == '_') base.pop_back();
  if (base.empty()) base = "keyring";
  std::string n = base;
  for (int i = 2; !dir.empty() && fld::file_exists(dir + "/" + n + ".keyring"); i++) n = base + std::to_string(i);
  return n;
}

static std::string header_text(const Collection &c) {
  fld::Json h = fld::Json::object();
  h.set("label", c.label);
  h.set("created", (double)c.created);
  h.set("modified", (double)c.modified);
  h.set("kdf", "scrypt");
  h.set("n", (double)c.kdf.n);
  h.set("r", (double)c.kdf.r);
  h.set("p", (double)c.kdf.p);
  h.set("salt", crypto::base64(c.kdf.salt));
  h.set("hash_salt", crypto::base64(c.hash_salt));
  h.set("next_id", (double)c.next_id);
  fld::Json items = fld::Json::array();
  for (auto &it : c.items) {
    fld::Json i = fld::Json::object();
    i.set("id", it.id);
    i.set("created", (double)it.created);
    i.set("modified", (double)it.modified);
    fld::Json hs = fld::Json::object();
    for (auto &kv : it.hashed) hs.set(kv.first, kv.second);
    i.set("hashed", hs);
    items.push(i);
  }
  h.set("items", items);
  return h.dump();
}

bool serialize(const Collection &c, std::string &out, std::string &err) {
  if (c.locked || c.key.size() != 32) {
    err = "the keyring is locked";
    return false;
  }
  std::string header = header_text(c);
  fld::Json body = fld::Json::object();
  fld::Json items = fld::Json::array();
  for (auto &it : c.items) {
    fld::Json i = fld::Json::object();
    i.set("id", it.id);
    i.set("label", it.label);
    i.set("content_type", it.content_type);
    i.set("secret", crypto::base64(it.secret));
    fld::Json a = fld::Json::object();
    for (auto &kv : it.attributes) a.set(kv.first, kv.second);
    i.set("attributes", a);
    items.push(i);
  }
  body.set("items", items);
  std::string plain = body.dump(), sealed;
  bool ok = crypto::seal(c.key, plain, header, sealed);
  crypto::wipe(plain);
  if (!ok) {
    err = "encryption failed";
    return false;
  }
  out = kMagic + header + "\n" + crypto::base64(sealed) + "\n";
  return true;
}

// the header part (everything a locked keyring knows)
static bool parse_header(Collection &c, const std::string &text, std::string &header, std::string &body,
                         std::string &err) {
  size_t m = strlen(kMagic);
  if (text.compare(0, m, kMagic) != 0) {
    err = "not a keyring file";
    return false;
  }
  size_t nl = text.find('\n', m);
  if (nl == std::string::npos) {
    err = "the keyring file is cut short";
    return false;
  }
  header = text.substr(m, nl - m);
  body = fld::trim(text.substr(nl + 1));
  fld::Json h;
  if (!fld::Json::parse(header, h) || !h.is_object() || h["kdf"].as_string() != "scrypt") {
    err = "the keyring file is damaged";
    return false;
  }
  c.label = h["label"].as_string();
  c.created = (uint64_t)h["created"].as_number();
  c.modified = (uint64_t)h["modified"].as_number();
  c.kdf.n = (uint64_t)h["n"].as_number();
  c.kdf.r = (uint32_t)h["r"].as_number();
  c.kdf.p = (uint32_t)h["p"].as_number();
  c.next_id = (uint64_t)h["next_id"].as_number();
  if (c.next_id == 0) c.next_id = 1;
  if (!crypto::unbase64(h["salt"].as_string(), c.kdf.salt) ||
      !crypto::unbase64(h["hash_salt"].as_string(), c.hash_salt)) {
    err = "the keyring file is damaged";
    return false;
  }
  c.items.clear();
  for (auto &i : h["items"].items()) {
    Item it;
    it.id = i["id"].as_string();
    it.created = (uint64_t)i["created"].as_number();
    it.modified = (uint64_t)i["modified"].as_number();
    for (auto &kv : i["hashed"].members()) it.hashed[kv.first] = kv.second.as_string();
    c.items.push_back(it);
  }
  c.locked = true;
  return true;
}

static bool open_body(Collection &c, const std::string &text, const crypto::Bytes &key, std::string &err) {
  std::string header, body;
  Collection tmp = c;
  if (!parse_header(tmp, text, header, body, err)) return false;
  crypto::Bytes sealed, plain;
  if (!crypto::unbase64(body, sealed)) {
    err = "the keyring file is damaged";
    return false;
  }
  if (!crypto::open(key, sealed, header, plain)) {
    err = "wrong password";
    return false;
  }
  fld::Json b;
  bool ok = fld::Json::parse(plain, b) && b.is_object();
  crypto::wipe(plain);
  if (!ok) {
    err = "the keyring file is damaged";
    return false;
  }
  std::map<std::string, Item> by_id;
  for (auto &it : tmp.items) by_id[it.id] = it;
  tmp.items.clear();
  for (auto &i : b["items"].items()) {
    Item it = by_id[i["id"].as_string()];
    it.id = i["id"].as_string();
    it.label = i["label"].as_string();
    it.content_type = i["content_type"].as_string();
    crypto::unbase64(i["secret"].as_string(), it.secret);
    Attributes a;
    for (auto &kv : i["attributes"].members()) a[kv.first] = kv.second.as_string();
    tmp.set_attributes(it, a);
    tmp.items.push_back(it);
  }
  tmp.name = c.name;
  tmp.file = c.file;
  tmp.key = key;
  tmp.locked = false;
  c = tmp;
  return true;
}

bool load(Collection &c, const std::string &file, std::string &err) {
  std::string text;
  if (!fld::read_file(file, text)) {
    err = "cannot read " + file;
    return false;
  }
  c.file = file;
  std::string n = fld::basename(file);
  c.name = n.substr(0, n.rfind('.'));
  std::string header, body;
  return parse_header(c, text, header, body, err);
}

bool unlock(Collection &c, const std::string &password, std::string &err) {
  std::string text;
  if (c.file.empty()) { // the session keyring is never locked
    err = "this keyring is not kept on disk";
    return false;
  }
  if (!fld::read_file(c.file, text)) {
    err = "cannot read " + c.file;
    return false;
  }
  crypto::Bytes key;
  if (!crypto::scrypt(password, c.kdf, key)) {
    err = "key derivation failed";
    return false;
  }
  bool ok = open_body(c, text, key, err);
  crypto::wipe(key);
  return ok;
}

bool unlock_with_key(Collection &c, const crypto::Bytes &key, std::string &err) {
  std::string text;
  if (c.file.empty() || !fld::read_file(c.file, text)) {
    err = "cannot read the keyring";
    return false;
  }
  return open_body(c, text, key, err);
}

bool derive_key(const Collection &c, const std::string &password, crypto::Bytes &key) {
  return crypto::scrypt(password, c.kdf, key);
}

void lock(Collection &c) {
  crypto::wipe(c.key);
  for (auto &it : c.items) {
    crypto::wipe(it.secret);
    it.attributes.clear();
    it.label.clear();
  }
  c.locked = true;
}

bool save(const Collection &c, std::string &err) {
  if (c.file.empty()) return true; // kept in memory only
  std::string text;
  if (!serialize(c, text, err)) return false;
  fld::mkdir_p(fld::dirname(c.file), 0700);
  std::string tmp = c.file + ".new";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    err = "cannot write " + tmp;
    return false;
  }
  size_t off = 0;
  while (off < text.size()) {
    ssize_t n = write(fd, text.data() + off, text.size() - off);
    if (n <= 0) break;
    off += (size_t)n;
  }
  bool ok = off == text.size() && fsync(fd) == 0;
  close(fd);
  if (!ok || rename(tmp.c_str(), c.file.c_str()) != 0) {
    unlink(tmp.c_str());
    err = "cannot write " + c.file;
    return false;
  }
  return true;
}

bool create(Collection &c, const std::string &dir, const std::string &name, const std::string &label,
            const std::string &password, std::string &err) {
  c = Collection();
  c.name = name;
  c.label = label;
  c.file = dir.empty() ? "" : dir + "/" + name + ".keyring";
  c.created = c.modified = now();
  c.kdf.salt = crypto::random_bytes(16);
  c.hash_salt = crypto::random_bytes(16);
  if (!crypto::scrypt(password, c.kdf, c.key)) {
    err = "key derivation failed";
    return false;
  }
  c.locked = false;
  return save(c, err);
}

bool change_password(Collection &c, const std::string &old_pw, const std::string &new_pw, std::string &err) {
  Collection probe = c;
  if (!unlock(probe, old_pw, err)) return false;
  probe.kdf.salt = crypto::random_bytes(16);
  crypto::wipe(probe.key);
  if (!crypto::scrypt(new_pw, probe.kdf, probe.key)) {
    err = "key derivation failed";
    return false;
  }
  probe.modified = now();
  if (!save(probe, err)) return false;
  c = probe;
  return true;
}

} // namespace store
