#include "reader.h"

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <regex>
#include <set>

namespace reader {

static std::string lower(std::string s) {
  for (auto &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static std::string attr(xmlNode *n, const char *name) {
  xmlChar *v = xmlGetProp(n, (const xmlChar *)name);
  std::string s = v ? (const char *)v : "";
  if (v) xmlFree(v);
  return s;
}

static std::string name_of(xmlNode *n) { return n && n->name ? lower((const char *)n->name) : ""; }

static std::string text_of(xmlNode *n) {
  xmlChar *t = xmlNodeGetContent(n);
  std::string s = t ? (const char *)t : "";
  if (t) xmlFree(t);
  return s;
}

static std::string squash(const std::string &s) {
  std::string out;
  bool sp = false;
  for (char c : s) {
    if (std::isspace((unsigned char)c)) sp = !out.empty();
    else {
      if (sp) out += ' ';
      sp = false;
      out += c;
    }
  }
  return out;
}

static int count_words(const std::string &s) {
  int n = 0;
  bool in = false;
  for (unsigned char c : s) {
    bool w = !std::isspace(c);
    if (w && !in) n++;
    in = w;
  }
  return n;
}

std::string resolve(const std::string &base, const std::string &rel) {
  if (rel.empty()) return base;
  static const std::regex scheme("^[a-zA-Z][a-zA-Z0-9+.-]*:");
  if (std::regex_search(rel, scheme)) return rel;
  size_t se = base.find("://");
  if (se == std::string::npos) return rel;
  std::string proto = base.substr(0, se);
  size_t host_end = base.find('/', se + 3);
  std::string origin = host_end == std::string::npos ? base : base.substr(0, host_end);
  if (rel.compare(0, 2, "//") == 0) return proto + ":" + rel;
  if (rel[0] == '/') return origin + rel;
  std::string path = host_end == std::string::npos ? "/" : base.substr(host_end);
  path = path.substr(0, path.find_first_of("?#"));
  if (rel[0] == '#' || rel[0] == '?') return origin + path + rel;
  std::string dir = path.substr(0, path.rfind('/') + 1);
  // remove ./ and ../
  std::vector<std::string> parts;
  std::string all = dir + rel, q;
  size_t qp = all.find_first_of("?#");
  if (qp != std::string::npos) {
    q = all.substr(qp);
    all = all.substr(0, qp);
  }
  size_t p = 0;
  while (p <= all.size()) {
    size_t e = all.find('/', p);
    if (e == std::string::npos) e = all.size();
    std::string seg = all.substr(p, e - p);
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (seg != "." && !(seg.empty() && e < all.size())) parts.push_back(seg);
    p = e + 1;
  }
  std::string out;
  for (auto &s : parts) out += "/" + s;
  if (!all.empty() && all.back() == '/' && (out.empty() || out.back() != '/')) out += "/";
  return origin + (out.empty() ? "/" : out) + q;
}

// ---- cleaning ----------------------------------------------------------------------

static const std::set<std::string> kDrop = {"script", "style", "noscript", "nav", "header", "footer", "aside", "form",
                                            "iframe", "svg", "button", "input", "select", "textarea", "template",
                                            "object", "embed", "canvas", "dialog", "menu", "link", "meta"};

static bool unlikely(xmlNode *n) {
  static const std::regex bad("comment|footer|sidebar|side-bar|navbar|\\bnav\\b|menu|share|social|related|advert|"
                              "\\bads?\\b|ad-|promo|cookie|banner|popup|modal|newsletter|subscribe|breadcrumb|"
                              "pagination|disqus|outbrain|sponsor|skip",
                              std::regex::icase);
  static const std::regex good("article|content|main|post|body|text|entry|story", std::regex::icase);
  std::string ci = attr(n, "class") + " " + attr(n, "id") + " " + attr(n, "role");
  if (attr(n, "aria-hidden") == "true" || attr(n, "hidden") != "") return true;
  std::string role = lower(attr(n, "role"));
  if (role == "navigation" || role == "complementary" || role == "banner" || role == "contentinfo") return true;
  if (ci.size() <= 2) return false;
  return std::regex_search(ci, bad) && !std::regex_search(ci, good);
}

static void clean(xmlNode *n) {
  for (xmlNode *c = n->children; c;) {
    xmlNode *next = c->next;
    if (c->type == XML_COMMENT_NODE) {
      xmlUnlinkNode(c);
      xmlFreeNode(c);
    } else if (c->type == XML_ELEMENT_NODE) {
      std::string nm = name_of(c);
      if (kDrop.count(nm) || (nm != "body" && nm != "html" && nm != "article" && nm != "main" && unlikely(c))) {
        xmlUnlinkNode(c);
        xmlFreeNode(c);
      } else
        clean(c);
    }
    c = next;
  }
}

// ---- scoring -----------------------------------------------------------------------

static int link_text(xmlNode *n) {
  int len = 0;
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    if (name_of(c) == "a") len += (int)squash(text_of(c)).size();
    else len += link_text(c);
  }
  return len;
}

static void collect(xmlNode *n, std::vector<xmlNode *> &out, const std::set<std::string> &names) {
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    if (names.count(name_of(c))) out.push_back(c);
    collect(c, out, names);
  }
}

static xmlNode *best_node(xmlNode *body) {
  std::map<xmlNode *, double> score;
  std::vector<xmlNode *> paras;
  collect(body, paras, {"p", "pre", "td", "blockquote", "li"});
  for (xmlNode *p : paras) {
    std::string t = squash(text_of(p));
    if (t.size() < 25) continue;
    double s = 1 + (double)std::count(t.begin(), t.end(), ',') + std::min(3.0, t.size() / 100.0);
    xmlNode *parent = p->parent, *grand = parent ? parent->parent : nullptr;
    if (parent && parent->type == XML_ELEMENT_NODE) score[parent] += s;
    if (grand && grand->type == XML_ELEMENT_NODE) score[grand] += s / 2;
  }
  xmlNode *best = nullptr;
  double best_s = 0;
  for (auto &e : score) {
    std::string nm = name_of(e.first);
    double s = e.second;
    if (nm == "article" || nm == "main") s *= 1.3;
    int tl = (int)squash(text_of(e.first)).size();
    if (tl > 0) s *= 1.0 - std::min(0.9, (double)link_text(e.first) / tl);
    if (s > best_s) {
      best_s = s;
      best = e.first;
    }
  }
  return best;
}

// ---- output ------------------------------------------------------------------------

static void esc(const std::string &s, std::string &out, bool attr_value = false) {
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += attr_value ? "&quot;" : "\""; break;
      default: out += c;
    }
  }
}

static const std::set<std::string> kKeep = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "blockquote",
                                            "pre", "code", "em", "strong", "b", "i", "u", "s", "sub", "sup", "a",
                                            "img", "figure", "figcaption", "table", "thead", "tbody", "tr", "th",
                                            "td", "br", "hr", "dl", "dt", "dd", "small", "mark", "q", "cite",
                                            "abbr", "kbd", "samp", "var", "del", "ins", "picture", "caption"};
static const std::set<std::string> kVoid = {"img", "br", "hr"};

static void emit(xmlNode *n, const std::string &url, std::string &out, bool in_pre) {
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE) {
      std::string t = c->content ? (const char *)c->content : "";
      if (!in_pre) {
        // runs of white space become one space
        std::string s;
        bool sp = false;
        for (char ch : t) {
          if (std::isspace((unsigned char)ch)) {
            if (!sp) s += ' ';
            sp = true;
          } else {
            s += ch;
            sp = false;
          }
        }
        t = s;
      }
      esc(t, out);
      continue;
    }
    if (c->type != XML_ELEMENT_NODE) continue;
    std::string nm = name_of(c);
    if (nm == "picture") { // the img inside is enough
      emit(c, url, out, in_pre);
      continue;
    }
    if (!kKeep.count(nm)) { // div, span, section...: their content
      bool block = nm == "div" || nm == "section" || nm == "article" || nm == "main";
      if (block) out += "\n";
      emit(c, url, out, in_pre);
      if (block) out += "\n";
      continue;
    }
    if (nm == "img") {
      std::string src = attr(c, "src");
      if (src.empty() || src.compare(0, 5, "data:") == 0) src = attr(c, "data-src");
      if (src.empty()) continue;
      out += "<img src=\"";
      esc(resolve(url, src), out, true);
      out += "\" alt=\"";
      esc(attr(c, "alt"), out, true);
      out += "\">";
      continue;
    }
    out += "<" + nm;
    if (nm == "a") {
      std::string href = attr(c, "href");
      if (!href.empty() && lower(href).compare(0, 11, "javascript:") != 0) {
        out += " href=\"";
        esc(resolve(url, href), out, true);
        out += "\"";
      }
    }
    out += ">";
    if (kVoid.count(nm)) continue;
    emit(c, url, out, in_pre || nm == "pre");
    out += "</" + nm + ">";
  }
}

static std::string meta(xmlNode *n, const char *key, const char *value) {
  for (xmlNode *c = n; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    if (name_of(c) == "meta" && (lower(attr(c, key)) == value)) return attr(c, "content");
    std::string r = meta(c->children, key, value);
    if (!r.empty()) return r;
  }
  return "";
}

static xmlNode *find_first(xmlNode *n, const char *name) {
  for (xmlNode *c = n; c; c = c->next) {
    if (c->type == XML_ELEMENT_NODE && name_of(c) == name) return c;
    if (c->type == XML_ELEMENT_NODE)
      if (xmlNode *r = find_first(c->children, name)) return r;
  }
  return nullptr;
}

Article extract(const std::string &html, const std::string &url) {
  Article a;
  htmlDocPtr doc = htmlReadMemory(html.data(), (int)html.size(), url.c_str(), "UTF-8",
                                  HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
  if (!doc) return a;
  xmlNode *root = xmlDocGetRootElement(doc);
  if (!root) {
    xmlFreeDoc(doc);
    return a;
  }
  a.title = squash(meta(root, "property", "og:title"));
  if (a.title.empty())
    if (xmlNode *t = find_first(root, "title")) a.title = squash(text_of(t));
  a.site = squash(meta(root, "property", "og:site_name"));
  a.byline = squash(meta(root, "name", "author"));
  xmlNode *body = find_first(root, "body");
  if (!body) body = root;
  if (a.title.empty())
    if (xmlNode *h = find_first(body, "h1")) a.title = squash(text_of(h));
  clean(body);
  xmlNode *best = best_node(body);
  if (best) {
    std::string out;
    emit(best, url, out, false);
    // the article's own heading at the start repeats the title
    size_t h = out.find("<h1>");
    if (h != std::string::npos && out.find_first_not_of(" \n") == h) {
      size_t e = out.find("</h1>", h);
      if (e != std::string::npos &&
          squash(std::regex_replace(out.substr(h + 4, e - h - 4), std::regex("<[^>]*>"), "")) == a.title)
        out.erase(h, e + 5 - h);
    }
    a.html = out;
    a.words = count_words(squash(text_of(best)));
  }
  xmlFreeDoc(doc);
  return a;
}

std::string page(const Article &a, bool dark, int font_px) {
  std::string bg = dark ? "#1e1f22" : "#fbf8f1", fg = dark ? "#dcdfe4" : "#2b2a28",
              dim = dark ? "#9aa0a6" : "#6f6a62", link = dark ? "#8ab4f8" : "#1c62b9";
  std::string out = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>";
  esc(a.title, out);
  out += "</title><style>"
         "body{background:" + bg + ";color:" + fg + ";margin:0;padding:2.5em 1.25em;"
         "font-family:'Noto Serif','DejaVu Serif',Georgia,serif;font-size:" + std::to_string(font_px) + "px;"
         "line-height:1.6}"
         "main{max-width:38em;margin:0 auto}"
         "h1{font-size:1.8em;line-height:1.25;margin:0 0 .3em}"
         ".meta{color:" + dim + ";font-family:sans-serif;font-size:.85em;margin-bottom:2em}"
         "a{color:" + link + "}img{max-width:100%;height:auto;display:block;margin:1em auto}"
         "pre{overflow:auto;padding:.8em;background:" + std::string(dark ? "#2b2d31" : "#efe9dd") + ";font-size:.85em}"
         "code{font-family:'DejaVu Sans Mono',monospace;font-size:.9em}"
         "blockquote{margin:1em 0;padding-left:1em;border-left:3px solid " + dim + ";color:" + dim + "}"
         "figcaption{color:" + dim + ";font-size:.85em;text-align:center}"
         "table{border-collapse:collapse}td,th{border:1px solid " + dim + ";padding:.3em .6em}"
         "</style></head><body><main><h1>";
  esc(a.title, out);
  out += "</h1><div class=\"meta\">";
  std::string m;
  if (!a.site.empty()) m += a.site;
  if (!a.byline.empty()) m += (m.empty() ? "" : " · ") + a.byline;
  if (a.words > 0) m += (m.empty() ? "" : " · ") + std::to_string(std::max(1, a.words / 230)) + " min read";
  esc(m, out);
  out += "</div>" + a.html + "</main></body></html>";
  return out;
}

} // namespace reader
