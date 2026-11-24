// fl-secretd's keyrings: one file per collection in
// ~/.local/share/fldesktop/keyrings/<name>.keyring
//
//   FLDKEYRING 1
//   {header: label, times, scrypt parameters, item ids with keyed hashes
//    of their attributes (so a locked keyring can still be searched)}
//   <base64: AES-256-GCM of the items (labels, attributes, secrets),
//    the header as additional data>
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "crypto.h"

namespace store {

using Attributes = std::map<std::string, std::string>;

struct Item {
  std::string id;               // the last part of its D-Bus path
  std::string label, content_type = "text/plain";
  Attributes attributes;        // known while unlocked
  crypto::Bytes secret;         // known while unlocked
  std::map<std::string, std::string> hashed; // attribute name -> keyed hash of its value
  uint64_t created = 0, modified = 0;
};

struct Collection {
  std::string name;             // file name and D-Bus path part ("login")
  std::string label;
  std::string file;             // "" for one kept in memory only (the "session" one)
  uint64_t created = 0, modified = 0;
  crypto::Kdf kdf;
  crypto::Bytes hash_salt;
  bool locked = true;
  crypto::Bytes key;            // while unlocked
  std::vector<Item> items;
  uint64_t next_id = 1;

  Item *find(const std::string &id);
  std::string hash(const std::string &name, const std::string &value) const;
  // the item matches all of these attributes (by value, or by hash when locked)
  bool matches(const Item &it, const Attributes &query) const;
  void set_attributes(Item &it, const Attributes &a); // with the hashes
  std::string new_id();
};

uint64_t now();
// a file name from a label ("My Passwords" -> "my_passwords"), unique in dir
std::string name_for(const std::string &label, const std::string &dir);

// a new collection (unlocked) with this password; saved
bool create(Collection &c, const std::string &dir, const std::string &name, const std::string &label,
            const std::string &password, std::string &err);
bool load(Collection &c, const std::string &file, std::string &err); // the header: locked
bool unlock(Collection &c, const std::string &password, std::string &err);
// with the key itself (handed over by the login: see stash.h)
bool unlock_with_key(Collection &c, const crypto::Bytes &key, std::string &err);
// the key a password gives (to hand over)
bool derive_key(const Collection &c, const std::string &password, crypto::Bytes &key);
void lock(Collection &c);     // forgets the key and the secrets
bool save(const Collection &c, std::string &err); // unlocked ones only
bool change_password(Collection &c, const std::string &old_pw, const std::string &new_pw, std::string &err);

// the whole file text
bool serialize(const Collection &c, std::string &out, std::string &err);

} // namespace store
