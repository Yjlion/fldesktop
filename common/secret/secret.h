// The Secret Service (org.freedesktop.secrets: GNOME Keyring,
// KeePassXC, KWallet) over GDBus, called from the FLTK thread. Unlocking and
// deleting may need the service's own password prompt; while it is open the
// calls wait and keep the FLTK windows drawn.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace secret {

struct Item {
  std::string path, label;
  std::vector<std::pair<std::string, std::string>> attributes;
  long long created = 0, modified = 0;
  bool locked = false;
};

struct Collection {
  std::string path, label, alias; // alias: "default" for the login keyring
  bool locked = false;
  std::vector<Item> items;
};

// Connects to the session bus; false (and err) when no service runs.
bool connect(std::string &err);
std::vector<Collection> collections();
bool unlock(const std::string &path, std::string &err); // a collection or an item
bool lock(const std::string &path, std::string &err);
// asks the service to change a collection's password (its own dialog)
bool change_password(const std::string &collection, std::string &err);
// The secret of an item as text (unlock it first).
bool get_secret(const std::string &item, std::string &value, std::string &err);
bool set_secret(const std::string &item, const std::string &value, std::string &err);
bool set_label(const std::string &item, const std::string &label, std::string &err);
bool delete_item(const std::string &item, std::string &err);
// A new password in a collection, with the attributes apps look for.
bool create_item(const std::string &collection, const std::string &label,
                 const std::vector<std::pair<std::string, std::string>> &attributes, const std::string &value,
                 std::string &err);
// the items whose attributes include all of these (locked ones too)
std::vector<Item> search(const std::vector<std::pair<std::string, std::string>> &attributes);
// the collection new passwords go to ("" when there is none)
std::string default_collection();
bool create_collection(const std::string &label, std::string &err);
bool delete_collection(const std::string &path, std::string &err);

} // namespace secret
