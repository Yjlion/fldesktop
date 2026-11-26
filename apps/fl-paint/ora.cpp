#include "ora.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <zlib.h>

namespace ora {

uint32_t crc32(const std::string &d) {
  return (uint32_t)::crc32(0L, (const Bytef *)d.data(), (uInt)d.size());
}

static void le16(std::string &o, uint32_t v) {
  o += (char)(v & 255);
  o += (char)((v >> 8) & 255);
}
static void le32(std::string &o, uint32_t v) {
  le16(o, v & 0xffff);
  le16(o, v >> 16);
}
static uint32_t rd16(const std::string &s, size_t p) {
  return p + 2 <= s.size() ? (uint32_t)(unsigned char)s[p] | (uint32_t)(unsigned char)s[p + 1] << 8 : 0;
}
static uint32_t rd32(const std::string &s, size_t p) { return rd16(s, p) | rd16(s, p + 2) << 16; }

std::string zip_write(const std::vector<ZipEntry> &entries) {
  std::string out, central;
  for (auto &e : entries) {
    uint32_t crc = crc32(e.data), off = (uint32_t)out.size();
    // local file header: stored, no data descriptor
    le32(out, 0x04034b50);
    le16(out, 20);
    le16(out, 0);
    le16(out, 0); // stored
    le16(out, 0);
    le16(out, 0x21); // 1980-01-01
    le32(out, crc);
    le32(out, (uint32_t)e.data.size());
    le32(out, (uint32_t)e.data.size());
    le16(out, (uint32_t)e.name.size());
    le16(out, 0);
    out += e.name;
    out += e.data;
    le32(central, 0x02014b50);
    le16(central, 20);
    le16(central, 20);
    le16(central, 0);
    le16(central, 0);
    le16(central, 0);
    le16(central, 0x21);
    le32(central, crc);
    le32(central, (uint32_t)e.data.size());
    le32(central, (uint32_t)e.data.size());
    le16(central, (uint32_t)e.name.size());
    le16(central, 0);
    le16(central, 0);
    le16(central, 0);
    le16(central, 0);
    le32(central, 0);
    le32(central, off);
    central += e.name;
  }
  uint32_t cd_off = (uint32_t)out.size();
  out += central;
  le32(out, 0x06054b50);
  le16(out, 0);
  le16(out, 0);
  le16(out, (uint32_t)entries.size());
  le16(out, (uint32_t)entries.size());
  le32(out, (uint32_t)central.size());
  le32(out, cd_off);
  le16(out, 0);
  return out;
}

static bool inflate_raw(const std::string &in, size_t usize, std::string &out) {
  out.assign(usize, '\0');
  z_stream z;
  memset(&z, 0, sizeof z);
  if (inflateInit2(&z, -15) != Z_OK) return false;
  z.next_in = (Bytef *)in.data();
  z.avail_in = (uInt)in.size();
  z.next_out = (Bytef *)&out[0];
  z.avail_out = (uInt)usize;
  int r = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  return r == Z_STREAM_END && z.total_out == usize;
}

bool zip_read(const std::string &zip, std::vector<ZipEntry> &out, size_t max_total) {
  out.clear();
  // the end of central directory record: within the last 64 KB + 22 bytes
  if (zip.size() < 22) return false;
  size_t eocd = std::string::npos;
  for (size_t p = zip.size() - 22;; p--) {
    if (rd32(zip, p) == 0x06054b50) {
      eocd = p;
      break;
    }
    if (p == 0 || zip.size() - p > 65557) break;
  }
  if (eocd == std::string::npos) return false;
  uint32_t n = rd16(zip, eocd + 10), cd = rd32(zip, eocd + 16);
  size_t p = cd, total = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (p + 46 > zip.size() || rd32(zip, p) != 0x02014b50) return false;
    uint32_t method = rd16(zip, p + 10), csize = rd32(zip, p + 20), usize = rd32(zip, p + 24);
    uint32_t nl = rd16(zip, p + 28), xl = rd16(zip, p + 30), cl = rd16(zip, p + 32), lh = rd32(zip, p + 42);
    if (p + 46 + nl > zip.size()) return false;
    std::string name = zip.substr(p + 46, nl);
    p += 46 + nl + xl + cl;
    if (lh + 30 > zip.size() || rd32(zip, lh) != 0x04034b50) return false;
    size_t data = lh + 30 + rd16(zip, lh + 26) + rd16(zip, lh + 28);
    if (data + csize > zip.size()) return false;
    total += usize;
    if (total > max_total) return false;
    ZipEntry e{name, ""};
    std::string comp = zip.substr(data, csize);
    if (method == 0) e.data = comp;
    else if (method == 8) {
      if (!inflate_raw(comp, usize, e.data)) return false;
    } else {
      continue; // another method: left out
    }
    out.push_back(std::move(e));
  }
  return true;
}

static std::string esc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static std::string unesc(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') {
      o += s[i];
      continue;
    }
    static const char *ents[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    bool done = false;
    for (auto &e : ents)
      if (s.compare(i, strlen(e[0]), e[0]) == 0) {
        o += e[1];
        i += strlen(e[0]) - 1;
        done = true;
        break;
      }
    if (!done) o += '&';
  }
  return o;
}

std::string stack_xml(int w, int h, const std::vector<LayerInfo> &top_first) {
  char buf[128];
  snprintf(buf, sizeof buf, "<image version=\"0.0.3\" w=\"%d\" h=\"%d\">\n <stack>\n", w, h);
  std::string x = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") + buf;
  for (auto &l : top_first) {
    snprintf(buf, sizeof buf, "\" x=\"%d\" y=\"%d\" opacity=\"%.3f\" visibility=\"%s\"", l.x, l.y, l.opacity,
             l.visible ? "visible" : "hidden");
    x += "  <layer name=\"" + esc(l.name) + "\" src=\"" + esc(l.src) + buf + "/>\n";
  }
  return x + " </stack>\n</image>\n";
}

static std::string attr(const std::string &tag, const char *name) {
  std::string key = std::string(" ") + name + "=\"";
  size_t p = tag.find(key);
  if (p == std::string::npos) {
    key = std::string(" ") + name + "='";
    p = tag.find(key);
    if (p == std::string::npos) return "";
  }
  char q = key.back();
  size_t a = p + key.size(), b = tag.find(q, a);
  return b == std::string::npos ? "" : unesc(tag.substr(a, b - a));
}

bool parse_stack(const std::string &xml, int &w, int &h, std::vector<LayerInfo> &top_first) {
  top_first.clear();
  size_t im = xml.find("<image");
  if (im == std::string::npos) return false;
  std::string itag = xml.substr(im, xml.find('>', im) - im);
  w = atoi(attr(itag, "w").c_str());
  h = atoi(attr(itag, "h").c_str());
  if (w <= 0 || h <= 0 || w > 32768 || h > 32768) return false;
  // layers anywhere in the stack (nested stacks are flattened in order)
  for (size_t p = xml.find("<layer"); p != std::string::npos; p = xml.find("<layer", p + 6)) {
    size_t e = xml.find('>', p);
    if (e == std::string::npos) break;
    std::string tag = xml.substr(p, e - p);
    LayerInfo l;
    l.name = attr(tag, "name");
    l.src = attr(tag, "src");
    l.x = atoi(attr(tag, "x").c_str());
    l.y = atoi(attr(tag, "y").c_str());
    std::string op = attr(tag, "opacity");
    l.opacity = op.empty() ? 1.0 : atof(op.c_str());
    if (!(l.opacity >= 0 && l.opacity <= 1)) l.opacity = 1.0;
    l.visible = attr(tag, "visibility") != "hidden";
    if (!l.src.empty()) top_first.push_back(l);
  }
  return true;
}

} // namespace ora
