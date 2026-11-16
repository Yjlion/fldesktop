// XDG Trash specification (home trash only)
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace fld {

struct TrashItem {
  std::string name;          // name in Trash/files
  std::string original_path;
  time_t deletion_date = 0;
};

std::string trash_dir();     // ~/.local/share/Trash (created)
// Moves path into the trash. Returns false with errno-like message on failure.
bool trash(const std::string &path, std::string *error = nullptr);
std::vector<TrashItem> trash_list();
bool trash_restore(const TrashItem &item, std::string *error = nullptr);
bool trash_delete(const TrashItem &item);
bool trash_empty();
// Recursive delete (like rm -rf). Returns false if anything could not be removed.
bool remove_recursive(const std::string &path);
// Recursive copy preserving modes and symlinks. `progress` gets bytes copied.
bool copy_recursive(const std::string &src, const std::string &dst, std::string *error = nullptr,
                    void (*progress)(unsigned long long, void *) = nullptr, void *data = nullptr);
// rename(), falling back to copy + delete across filesystems.
bool move_path(const std::string &src, const std::string &dst, std::string *error = nullptr);

} // namespace fld
