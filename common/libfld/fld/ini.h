// Minimal INI / desktop-entry style key file
#pragma once
#include <map>
#include <string>
#include <vector>

namespace fld {

class Ini {
public:
  bool load(const std::string &path);
  bool parse(const std::string &text);
  bool save(const std::string &path) const;
  std::string to_string() const;

  bool has(const std::string &group, const std::string &key) const;
  std::string get(const std::string &group, const std::string &key,
                  const std::string &def = "") const;
  int get_int(const std::string &group, const std::string &key, int def = 0) const;
  double get_double(const std::string &group, const std::string &key, double def = 0) const;
  bool get_bool(const std::string &group, const std::string &key, bool def = false) const;
  void set(const std::string &group, const std::string &key, const std::string &value);
  void set_int(const std::string &group, const std::string &key, int v);
  void set_bool(const std::string &group, const std::string &key, bool v);
  void remove(const std::string &group, const std::string &key);

  std::vector<std::string> groups() const;
  std::vector<std::string> keys(const std::string &group) const; // in file order

private:
  struct Group {
    std::string name;
    std::vector<std::pair<std::string, std::string>> entries;
  };
  std::vector<Group> groups_;
  Group *find(const std::string &g);
  const Group *find(const std::string &g) const;
};

} // namespace fld
