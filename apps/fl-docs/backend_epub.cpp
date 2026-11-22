// fl-docs: EPUB e-books
//
// META-INF/container.xml names the package (OPF): its title, manifest and
// spine (reading order). Each spine document (XHTML) becomes blocks:
// headings, paragraphs, lists, quotes, preformatted text, images, rules and
// simple tables, with bold, italic and the like inside paragraphs. Style
// sheets are not applied, apart from text-align and bold or italic in style
// attributes. Every spine document starts on a new page.
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>

#include "archive_reader.h"
#include "backend.h"
#include "flow.h"
#include "fld/util.h"

namespace docs {

namespace {

using flow::Block;

std::string lname(xmlNode *n) {
  std::string s = n->name ? (const char *)n->name : "";
  size_t c = s.find(':');
  if (c != std::string::npos) s = s.substr(c + 1);
  for (char &ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}

// an attribute by its local name, whatever its namespace
std::string attr(xmlNode *n, const char *name) {
  for (xmlAttr *a = n->properties; a; a = a->next) {
    std::string an = (const char *)a->name;
    size_t c = an.find(':');
    if (c != std::string::npos) an = an.substr(c + 1);
    if (strcasecmp(an.c_str(), name) == 0) {
      xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
      std::string s = v ? (const char *)v : "";
      xmlFree(v);
      return s;
    }
  }
  return "";
}

std::string text_of(xmlNode *n) {
  xmlChar *v = xmlNodeGetContent(n);
  std::string s = v ? (const char *)v : "";
  xmlFree(v);
  return fld::trim(s);
}

xmlDoc *parse_xml(const std::string &data) {
  return xmlReadMemory(data.data(), (int)data.size(), nullptr, nullptr,
                       XML_PARSE_RECOVER | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
}

// XHTML through the HTML parser: it knows &nbsp; and the other entities
// e-books use without declaring them, and forgives broken markup
xmlDoc *parse_html(const std::string &data) {
  return htmlReadMemory(data.data(), (int)data.size(), nullptr, "UTF-8",
                        HTML_PARSE_RECOVER | HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
}

xmlNode *find(xmlNode *n, const char *name) {
  for (; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    if (lname(n) == name) return n;
    if (xmlNode *f = find(n->children, name)) return f;
  }
  return nullptr;
}

std::string style_prop(const std::string &style, const char *prop) {
  std::string s = fld::lower(style);
  size_t p = s.find(prop);
  if (p == std::string::npos) return "";
  p = s.find(':', p);
  if (p == std::string::npos) return "";
  size_t e = s.find(';', p);
  return fld::trim(s.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1));
}

const double kHeading[] = {1.9, 1.55, 1.3, 1.15, 1.05, 1.0};

// One spine document to blocks.
class Walker {
public:
  const flow::ImageFn &load; // an <img src>'s bytes
  std::vector<Block> blocks;

  explicit Walker(const flow::ImageFn &l) : load(l) {}

  void walk_children(xmlNode *n) {
    for (xmlNode *c = n->children; c; c = c->next) walk(c);
  }

  void flush() {
    std::string trimmed = fld::trim(cur.markup);
    bool has_text = false;
    // only tags and spaces: nothing to show
    bool tag = false;
    for (char c : trimmed) {
      if (c == '<') tag = true;
      else if (c == '>') tag = false;
      else if (!tag && !isspace((unsigned char)c)) { has_text = true; break; }
    }
    if (has_text) {
      Block b = cur;
      // close the inline tags still open; they are opened again after
      std::string close;
      for (auto it = open.rbegin(); it != open.rend(); ++it) close += "</" + *it + ">";
      std::string body = pre ? cur.markup : trimmed;
      if (!pre)
        while (!body.empty() && body.back() == '\n') body.pop_back();
      b.markup = body + close;
      blocks.push_back(b);
      cur.bullet.clear();
    }
    // without text a list bullet waits for the first paragraph of the item
    cur.markup.clear();
    for (auto &t : open) cur.markup += "<" + t + ">";
    last_space = true;
  }

  // starts a block-level element with these settings (restored by end())
  struct Saved {
    Block style;
    bool pre;
  };
  Saved begin_block() {
    flush();
    return {cur, pre};
  }
  void end_block(const Saved &s) {
    flush();
    std::string markup = cur.markup;
    cur = s.style;
    cur.markup = markup;
    cur.bullet.clear();
    pre = s.pre;
  }

  void inline_tag(xmlNode *n, const std::string &tag) {
    cur.markup += "<" + tag + ">";
    open.push_back(tag);
    walk_children(n);
    // a block inside may have flushed and reopened it
    if (!open.empty() && open.back() == tag) open.pop_back();
    cur.markup += "</" + tag + ">";
  }

  void text(const std::string &t) {
    if (pre) {
      cur.markup += flow::escape(t);
      return;
    }
    std::string o;
    for (char c : t) {
      if (c == '\n' || c == '\t' || c == '\r' || c == ' ') {
        if (!last_space) o += ' ';
        last_space = true;
      } else {
        o += c;
        last_space = false;
      }
    }
    cur.markup += flow::escape(o);
  }

  void image(const std::string &href) {
    std::string data;
    if (href.empty() || !load(href, data) || data.empty()) return;
    flush();
    Block b;
    b.kind = Block::Image;
    b.image = std::move(data);
    blocks.push_back(b);
  }

  void apply_style(xmlNode *n) {
    std::string a = fld::lower(attr(n, "align"));
    std::string ta = style_prop(attr(n, "style"), "text-align");
    if (!ta.empty()) a = ta;
    if (a == "center") cur.align = Block::Center;
    else if (a == "right" || a == "end") cur.align = Block::Right;
    else if (a == "justify") cur.align = Block::Justify;
    else if (a == "left" || a == "start") cur.align = Block::Left;
  }

  void table(xmlNode *n) {
    flush();
    Block t;
    t.kind = Block::Table;
    std::vector<xmlNode *> rows;
    std::function<void(xmlNode *)> collect = [&](xmlNode *p) {
      for (xmlNode *c = p->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        std::string nm = lname(c);
        if (nm == "tr") rows.push_back(c);
        else if (nm == "thead" || nm == "tbody" || nm == "tfoot") collect(c);
      }
    };
    collect(n);
    for (xmlNode *r : rows) {
      std::vector<std::vector<Block>> cells;
      for (xmlNode *c = r->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        std::string nm = lname(c);
        if (nm != "td" && nm != "th") continue;
        Walker w(load);
        w.base_size = base_size;
        if (nm == "th") w.cur.markup = "<b>", w.open.push_back("b");
        w.walk_children(c);
        w.flush();
        cells.push_back(w.blocks);
      }
      if (!cells.empty()) t.rows.push_back(cells);
    }
    if (!t.rows.empty()) blocks.push_back(t);
  }

  void walk(xmlNode *n) {
    if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
      if (n->content) text((const char *)n->content);
      return;
    }
    if (n->type != XML_ELEMENT_NODE) return;
    std::string nm = lname(n);
    if (nm == "head" || nm == "script" || nm == "style" || nm == "title" || nm == "noscript") return;
    std::string st = attr(n, "style");
    if (style_prop(st, "display") == "none") return;

    // inline
    if (nm == "b" || nm == "strong") return inline_tag(n, "b");
    if (nm == "i" || nm == "em" || nm == "cite" || nm == "var" || nm == "dfn") return inline_tag(n, "i");
    if (nm == "u" || nm == "ins") return inline_tag(n, "u");
    if (nm == "s" || nm == "strike" || nm == "del") return inline_tag(n, "s");
    if (nm == "sup") return inline_tag(n, "sup");
    if (nm == "sub") return inline_tag(n, "sub");
    if (nm == "small") return inline_tag(n, "small");
    if (nm == "big") return inline_tag(n, "big");
    if (nm == "code" || nm == "tt" || nm == "kbd" || nm == "samp") return inline_tag(n, "tt");
    if (nm == "br") {
      cur.markup += "\n";
      last_space = true;
      return;
    }
    if (nm == "img") return image(attr(n, "src"));
    if (nm == "image") return image(attr(n, "href")); // inside <svg>
    if (nm == "span" || nm == "a" || nm == "font" || nm == "abbr" || nm == "q" || nm == "mark" || nm == "label") {
      std::string fw = style_prop(st, "font-weight"), fs = style_prop(st, "font-style");
      if (fw == "bold" || fw == "700" || fw == "800" || fw == "900") return inline_tag(n, "b");
      if (fs == "italic" || fs == "oblique") return inline_tag(n, "i");
      return walk_children(n);
    }
    if (nm == "svg") {
      // only the pictures inside
      std::function<void(xmlNode *)> imgs = [&](xmlNode *p) {
        for (xmlNode *c = p->children; c; c = c->next)
          if (c->type == XML_ELEMENT_NODE) {
            if (lname(c) == "image") image(attr(c, "href"));
            else imgs(c);
          }
      };
      imgs(n);
      return;
    }
    if (nm == "table") return table(n);
    if (nm == "hr") {
      flush();
      Block b;
      b.kind = Block::Rule;
      blocks.push_back(b);
      return;
    }

    // block level
    Saved s = begin_block();
    apply_style(n);
    if (nm.size() == 2 && nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6') {
      int lv = nm[1] - '1';
      cur.size = base_size * kHeading[lv];
      cur.space_before = 14 - lv;
      cur.space_after = 8;
      cur.markup += "<b>";
      open.push_back("b");
      walk_children(n);
      if (!open.empty() && open.back() == "b") open.pop_back();
      cur.markup += "</b>";
    } else if (nm == "ul" || nm == "ol" || nm == "dl") {
      lists.push_back({nm == "ol", nm == "ol" && !attr(n, "start").empty() ? atoi(attr(n, "start").c_str()) : 1});
      cur.indent += 22;
      walk_children(n);
      lists.pop_back();
    } else if (nm == "li") {
      std::string bullet = "•";
      if (!lists.empty() && lists.back().ordered) bullet = std::to_string(lists.back().next++) + ".";
      cur.bullet = bullet;
      cur.space_after = 2;
      walk_children(n);
    } else if (nm == "dd") {
      cur.indent += 22;
      walk_children(n);
    } else if (nm == "blockquote") {
      cur.indent += 24;
      cur.space_after = 8;
      cur.markup += "<i>";
      open.push_back("i");
      walk_children(n);
      if (!open.empty() && open.back() == "i") open.pop_back();
      cur.markup += "</i>";
    } else if (nm == "pre") {
      pre = true;
      cur.family = "Monospace";
      cur.space_before = 4;
      cur.space_after = 8;
      cur.size = base_size * 0.85;
      walk_children(n);
    } else {
      // p, div, section, figure, figcaption, center, ...
      if (nm == "p") cur.space_after = 6;
      if (nm == "center") cur.align = Block::Center;
      if (nm == "figcaption" || nm == "caption") {
        cur.align = Block::Center;
        cur.size = base_size * 0.9;
      }
      walk_children(n);
    }
    end_block(s);
  }

  double base_size = 12;

private:
  struct List {
    bool ordered;
    int next;
  };
  std::vector<List> lists;
  Block cur;
  std::vector<std::string> open; // inline tags open in cur.markup
  bool pre = false;
  bool last_space = true;
};

} // namespace

std::vector<Block> flow::html_blocks(const std::string &html, const ImageFn &image, double size) {
  xmlDoc *h = parse_html(html);
  if (!h) return {};
  Walker w(image);
  w.base_size = size;
  xmlNode *body = find(xmlDocGetRootElement(h), "body");
  if (body) w.walk_children(body);
  w.flush();
  xmlFreeDoc(h);
  return std::move(w.blocks);
}

std::unique_ptr<Backend> open_epub(const std::string &path, std::string &err) {
  std::map<std::string, std::string> files;
  // everything but fonts, audio and video
  auto want = [](const std::string &n) {
    std::string l = fld::lower(n);
    for (const char *skip : {".ttf", ".otf", ".woff", ".woff2", ".mp3", ".mp4", ".m4a", ".ogg", ".webm"})
      if (l.size() > strlen(skip) && l.compare(l.size() - strlen(skip), strlen(skip), skip) == 0) return false;
    return true;
  };
  if (!read_members(path, want, files, err)) return nullptr;

  auto container = files.find("META-INF/container.xml");
  if (container == files.end()) {
    err = "The e-book has no META-INF/container.xml.";
    return nullptr;
  }
  std::string opf_name;
  if (xmlDoc *c = parse_xml(container->second)) {
    if (xmlNode *r = find(xmlDocGetRootElement(c), "rootfile")) opf_name = clean_member(attr(r, "full-path"));
    xmlFreeDoc(c);
  }
  auto opf = files.find(opf_name);
  if (opf == files.end()) {
    err = "The e-book's package file is missing.";
    return nullptr;
  }

  // DRM: encrypted content documents (fonts may be obfuscated, that is fine)
  auto enc = files.find("META-INF/encryption.xml");
  if (enc != files.end()) {
    if (xmlDoc *e = parse_xml(enc->second)) {
      bool drm = false;
      std::function<void(xmlNode *)> scan = [&](xmlNode *n) {
        for (; n; n = n->next) {
          if (n->type != XML_ELEMENT_NODE) continue;
          if (lname(n) == "cipherreference") {
            std::string u = fld::lower(attr(n, "URI"));
            for (const char *x : {".html", ".xhtml", ".htm", ".xml"})
              if (u.size() > strlen(x) && u.compare(u.size() - strlen(x), strlen(x), x) == 0) drm = true;
          }
          scan(n->children);
        }
      };
      scan(xmlDocGetRootElement(e));
      xmlFreeDoc(e);
      if (drm) {
        err = "The e-book is protected (DRM) and cannot be shown.";
        return nullptr;
      }
    }
  }

  auto b = std::make_unique<flow::FlowBackend>();
  std::map<std::string, std::string> manifest; // id -> member
  std::vector<std::string> spine;
  xmlDoc *o = parse_xml(opf->second);
  if (!o) {
    err = "The e-book's package file cannot be read.";
    return nullptr;
  }
  xmlNode *root = xmlDocGetRootElement(o);
  if (xmlNode *t = find(root, "title")) b->title_ = text_of(t);
  if (xmlNode *m = find(root, "manifest"))
    for (xmlNode *i = m->children; i; i = i->next)
      if (i->type == XML_ELEMENT_NODE && lname(i) == "item") manifest[attr(i, "id")] = resolve_member(opf_name, attr(i, "href"));
  if (xmlNode *s = find(root, "spine"))
    for (xmlNode *i = s->children; i; i = i->next)
      if (i->type == XML_ELEMENT_NODE && lname(i) == "itemref") {
        auto it = manifest.find(attr(i, "idref"));
        if (it != manifest.end()) spine.push_back(it->second);
      }
  xmlFreeDoc(o);
  if (spine.empty()) {
    err = "The e-book has no pages.";
    return nullptr;
  }

  flow::PageStyle style;
  std::vector<Block> blocks;
  for (const std::string &doc : spine) {
    auto it = files.find(doc);
    if (it == files.end()) continue;
    flow::ImageFn image = [&](const std::string &href, std::string &data) {
      auto f = files.find(resolve_member(doc, href));
      if (f == files.end()) return false;
      data = f->second;
      return true;
    };
    std::vector<Block> doc_blocks = flow::html_blocks(it->second, image, style.size);
    if (doc_blocks.empty()) continue;
    if (!blocks.empty()) {
      Block br;
      br.kind = Block::PageBreak;
      blocks.push_back(br);
    }
    blocks.insert(blocks.end(), doc_blocks.begin(), doc_blocks.end());
  }
  if (blocks.empty()) {
    err = "The e-book has no text.";
    return nullptr;
  }
  // the first pages now, the rest as the viewer asks (layout_step)
  b->doc.begin(std::move(blocks), style);
  b->layout_step(0.1);
  return b;
}

} // namespace docs
