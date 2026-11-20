// fl-docs: reading members of zip (and rar, 7z, tar) files with libarchive
#include "archive_reader.h"

#include <archive.h>
#include <archive_entry.h>

#include <cctype>
#include <memory>

namespace docs {

namespace {

struct Reader {
  archive *a = archive_read_new();
  ~Reader() { archive_read_free(a); }
};

bool open(Reader &r, const std::string &path, std::string &err) {
  archive_read_support_filter_all(r.a);
  archive_read_support_format_all(r.a);
  if (archive_read_open_filename(r.a, path.c_str(), 64 * 1024) != ARCHIVE_OK) {
    err = archive_error_string(r.a) ? archive_error_string(r.a) : "Cannot read the file.";
    return false;
  }
  return true;
}

std::string url_decode(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

} // namespace

std::string clean_member(const std::string &name) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= name.size()) {
    size_t p = name.find('/', start);
    if (p == std::string::npos) p = name.size();
    std::string part = name.substr(start, p - start);
    start = p + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (parts.empty()) return "";
      parts.pop_back();
      continue;
    }
    parts.push_back(part);
  }
  std::string out;
  for (auto &p : parts) out += (out.empty() ? "" : "/") + p;
  return out;
}

std::string resolve_member(const std::string &base, const std::string &href) {
  std::string h = href.substr(0, href.find('#'));
  h = url_decode(h.substr(0, h.find('?')));
  if (h.empty()) return "";
  if (h[0] == '/') return clean_member(h);
  size_t slash = base.rfind('/');
  return clean_member((slash == std::string::npos ? "" : base.substr(0, slash + 1)) + h);
}

bool list_members(const std::string &path, std::vector<std::string> &names, std::string &err) {
  Reader r;
  if (!open(r, path, err)) return false;
  archive_entry *e;
  int rc;
  while ((rc = archive_read_next_header(r.a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
    if (archive_entry_filetype(e) == AE_IFREG) {
      const char *n = archive_entry_pathname_utf8(e);
      if (!n) n = archive_entry_pathname(e);
      std::string c = n ? clean_member(n) : "";
      if (!c.empty()) names.push_back(c);
    }
    archive_read_data_skip(r.a);
  }
  if (rc != ARCHIVE_EOF) {
    err = archive_error_string(r.a) ? archive_error_string(r.a) : "The file is damaged.";
    return !names.empty();
  }
  return true;
}

bool read_members(const std::string &path, const std::function<bool(const std::string &)> &want,
                  std::map<std::string, std::string> &out, std::string &err, size_t max_total, size_t max_member,
                  size_t prefix) {
  Reader r;
  if (!open(r, path, err)) return false;
  archive_entry *e;
  int rc;
  size_t total = 0;
  while ((rc = archive_read_next_header(r.a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
    const char *n = archive_entry_pathname_utf8(e);
    if (!n) n = archive_entry_pathname(e);
    std::string c = n ? clean_member(n) : "";
    if (archive_entry_filetype(e) != AE_IFREG || c.empty() || !want(c)) {
      archive_read_data_skip(r.a);
      continue;
    }
    if (archive_entry_is_encrypted(e)) {
      err = "The file is protected by a password.";
      return false;
    }
    std::string data;
    char buf[64 * 1024];
    la_ssize_t got;
    while ((got = archive_read_data(r.a, buf, sizeof buf)) > 0) {
      data.append(buf, (size_t)got);
      if (prefix && data.size() >= prefix) break;
      if (data.size() > max_member || total + data.size() > max_total) {
        err = "The file is too large.";
        return false;
      }
    }
    if (got < 0) {
      err = archive_error_string(r.a) ? archive_error_string(r.a) : "The file is damaged.";
      return false;
    }
    total += data.size();
    out[c] = std::move(data);
  }
  if (rc != ARCHIVE_EOF) {
    err = archive_error_string(r.a) ? archive_error_string(r.a) : "The file is damaged.";
    return false;
  }
  return true;
}

bool read_member(const std::string &path, const std::string &name, std::string &out, std::string &err) {
  std::map<std::string, std::string> m;
  bool found = false;
  // stop scanning once the member is read: later members are not needed
  if (!read_members(path, [&](const std::string &n) { return !found && (found = n == name); }, m, err)) return false;
  auto it = m.find(name);
  if (it == m.end()) {
    err = "Missing " + name + ".";
    return false;
  }
  out = std::move(it->second);
  return true;
}

} // namespace docs
