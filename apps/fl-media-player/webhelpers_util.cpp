#include "webhelpers_util.h"

#include <cctype>
#include <sstream>

#include "fld/json.h"

namespace webhelpers {

const char *const kYtdlpBase = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";
const char *const kQjsApi = "https://api.github.com/repos/quickjs-ng/quickjs/releases/latest";
const char *const kQjsDownload = "https://github.com/quickjs-ng/quickjs/releases/download/";

std::string qjs_asset(const std::string &m) {
  std::string arch;
  if (m == "x86_64" || m == "amd64") arch = "x86_64";
  else if (m == "aarch64" || m == "arm64") arch = "aarch64";
  else if (m == "armv7l" || m == "armv8l") arch = "armv7";
  else if (m.size() == 4 && m[0] == 'i' && m[1] >= '3' && m[1] <= '6' && m.substr(2) == "86") arch = "x86";
  else if (m == "riscv64") arch = "riscv64";
  return arch.empty() ? "" : "qjs-linux-" + arch;
}

bool is_sha256(const std::string &s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!isxdigit((unsigned char)c) || isupper((unsigned char)c)) return false;
  return true;
}

std::string sha256_in_sums(const std::string &sums, const std::string &file) {
  std::istringstream in(sums);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string sum, name, extra;
    if (!(l >> sum >> name) || (l >> extra)) continue;
    if (!name.empty() && name[0] == '*') name = name.substr(1);
    for (char &c : sum) c = (char)tolower((unsigned char)c);
    if (name == file && is_sha256(sum)) return sum;
  }
  return "";
}

bool qjs_release_asset(const std::string &json, const std::string &asset, std::string &tag, std::string &sha256,
                       std::string &url) {
  fld::Json j;
  if (!fld::Json::parse(json, j) || !j.is_object()) return false;
  tag = j["tag_name"].as_string();
  if (tag.empty()) return false;
  for (const fld::Json &a : j["assets"].items()) {
    if (a["name"].as_string() != asset) continue;
    std::string d = a["digest"].as_string();
    if (d.compare(0, 7, "sha256:") != 0) return false;
    sha256 = d.substr(7);
    for (char &c : sha256) c = (char)tolower((unsigned char)c);
    url = a["browser_download_url"].as_string();
    std::string prefix = kQjsDownload, tail = "/" + asset;
    return is_sha256(sha256) && url.compare(0, prefix.size(), prefix) == 0 && url.size() > tail.size() &&
           url.compare(url.size() - tail.size(), tail.size(), tail) == 0;
  }
  return false;
}

bool version_at_least(const std::string &version, const std::string &min) {
  return !version.empty() && version >= min;
}

std::string js_runtime(bool have_deno, const std::string &managed_qjs, bool system_qjs,
                       const std::string &ytdlp_version) {
  if (have_deno) return ""; // yt-dlp's default
  std::string r;
  if (!managed_qjs.empty()) r = "quickjs:" + managed_qjs;
  else if (system_qjs) r = "quickjs";
  else return "";
  // --js-runtimes came with yt-dlp 2025.11; older ones refuse to run with it
  return version_at_least(ytdlp_version, "2025.11") ? r : "";
}

std::vector<std::string> resolve_cmd(const std::string &ytdlp, const std::string &js, const std::string &page) {
  std::vector<std::string> c = {ytdlp, "--no-playlist", "-f", "b", "--get-url", "--get-title"};
  if (!js.empty()) {
    c.push_back("--js-runtimes");
    c.push_back(js);
  }
  c.push_back("--");
  c.push_back(page);
  return c;
}

} // namespace webhelpers
