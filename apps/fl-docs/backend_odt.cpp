// fl-docs: OpenDocument text (.odt, .ott)
//
// content.xml holds the text, styles.xml the named styles and the page
// layout, meta.xml the title, Pictures/ the images. Paragraphs and headings
// keep their alignment, indents, spacing, page breaks and character styles
// (bold, italic, underline, strike-through, size, super- and subscript,
// monospace); lists get bullets or numbers; tables are drawn as simple grids;
// images keep their size. The text is laid out again by Pango, so lines and
// pages break where they fit here, not where the writing program put them.
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>

#include "archive_reader.h"
#include "backend.h"
#include "flow.h"
#include "fld/util.h"

namespace docs {

namespace {

using flow::Block;

std::string lname(xmlNode *n) {
  std::string s = n->name ? (const char *)n->name : "";
  return s;
}

std::string attr(xmlNode *n, const char *name) {
  for (xmlAttr *a = n->properties; a; a = a->next) {
    if (strcmp((const char *)a->name, name) == 0) {
      xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
      std::string s = v ? (const char *)v : "";
      xmlFree(v);
      return s;
    }
  }
  return "";
}

xmlNode *child(xmlNode *n, const char *name) {
  for (xmlNode *c = n ? n->children : nullptr; c; c = c->next)
    if (c->type == XML_ELEMENT_NODE && lname(c) == name) return c;
  return nullptr;
}

xmlDoc *parse_xml(const std::string &data) {
  return xmlReadMemory(data.data(), (int)data.size(), nullptr, nullptr,
                       XML_PARSE_RECOVER | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_HUGE);
}

struct Style {
  std::string parent, list_style;
  std::optional<bool> bold, italic, underline, strike, mono;
  std::optional<double> size;     // points
  std::optional<double> size_pct; // of the parent's size
  std::optional<int> align;       // Block::Align
  std::optional<double> margin_left, margin_top, margin_bottom, text_indent;
  std::optional<std::string> position; // "super" / "sub"
  bool break_before = false, break_after = false;
};

struct ListLevel {
  bool numbered = false;
  std::string bullet = "•";
  std::string format = "1"; // 1, a, A, i, I
};

std::string roman(int n, bool upper) {
  static const std::pair<int, const char *> t[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"},
                                                   {90, "xc"},  {50, "l"},   {40, "xl"},  {10, "x"},   {9, "ix"},
                                                   {5, "v"},    {4, "iv"},   {1, "i"}};
  std::string s;
  for (auto &p : t)
    while (n >= p.first) {
      s += p.second;
      n -= p.first;
    }
  if (upper)
    for (char &c : s) c = (char)toupper((unsigned char)c);
  return s;
}

std::string number(int n, const std::string &format) {
  if (format == "a" || format == "A") {
    std::string s;
    for (int k = n; k > 0; k = (k - 1) / 26) s.insert(s.begin(), (char)((format == "A" ? 'A' : 'a') + (k - 1) % 26));
    return s;
  }
  if (format == "i" || format == "I") return roman(n, format == "I");
  return std::to_string(n);
}

class Odt {
public:
  const std::map<std::string, std::string> &files;
  std::map<std::string, Style> styles; // "paragraph:Name", "text:Name"
  std::map<std::string, std::vector<ListLevel>> list_styles;
  std::map<std::string, std::string> fonts; // font-face name -> family
  Style defaults;
  flow::PageStyle page;
  std::vector<Block> blocks;

  explicit Odt(const std::map<std::string, std::string> &f) : files(f) {}

  void read_font_faces(xmlNode *root) {
    if (xmlNode *d = child(root, "font-face-decls"))
      for (xmlNode *f = d->children; f; f = f->next)
        if (f->type == XML_ELEMENT_NODE && lname(f) == "font-face") fonts[attr(f, "name")] = attr(f, "font-family");
  }

  bool is_mono(const std::string &font_name) {
    std::string fam = fld::lower(fonts.count(font_name) ? fonts[font_name] : font_name);
    return fam.find("mono") != std::string::npos || fam.find("courier") != std::string::npos ||
           fam.find("consol") != std::string::npos;
  }

  void read_props(xmlNode *s, Style &st) {
    for (xmlNode *p = s->children; p; p = p->next) {
      if (p->type != XML_ELEMENT_NODE) continue;
      std::string n = lname(p);
      if (n == "text-properties") {
        std::string v = attr(p, "font-weight");
        if (!v.empty()) st.bold = v == "bold" || atoi(v.c_str()) >= 600;
        v = attr(p, "font-style");
        if (!v.empty()) st.italic = v == "italic" || v == "oblique";
        v = attr(p, "text-underline-style");
        if (!v.empty()) st.underline = v != "none";
        v = attr(p, "text-line-through-style");
        if (!v.empty()) st.strike = v != "none";
        v = attr(p, "font-size");
        if (!v.empty()) {
          if (v.back() == '%') st.size_pct = atof(v.c_str()) / 100.0;
          else st.size = parse_length(v, 0);
        }
        v = attr(p, "font-name");
        if (!v.empty()) st.mono = is_mono(v);
        v = attr(p, "text-position");
        if (!v.empty()) {
          if (v.compare(0, 5, "super") == 0 || (isdigit((unsigned char)v[0]) && atof(v.c_str()) > 0)) st.position = "super";
          else if (v.compare(0, 3, "sub") == 0 || v[0] == '-') st.position = "sub";
        }
      } else if (n == "paragraph-properties") {
        std::string v = attr(p, "text-align");
        if (v == "center") st.align = Block::Center;
        else if (v == "end" || v == "right") st.align = Block::Right;
        else if (v == "justify") st.align = Block::Justify;
        else if (v == "start" || v == "left") st.align = Block::Left;
        v = attr(p, "margin-left");
        if (!v.empty()) st.margin_left = parse_length(v, 0);
        v = attr(p, "margin-top");
        if (!v.empty()) st.margin_top = parse_length(v, 0);
        v = attr(p, "margin-bottom");
        if (!v.empty()) st.margin_bottom = parse_length(v, 0);
        v = attr(p, "text-indent");
        if (!v.empty()) st.text_indent = parse_length(v, 0);
        if (attr(p, "break-before") == "page") st.break_before = true;
        if (attr(p, "break-after") == "page") st.break_after = true;
      }
    }
  }

  void read_styles(xmlNode *container) {
    for (xmlNode *s = container ? container->children : nullptr; s; s = s->next) {
      if (s->type != XML_ELEMENT_NODE) continue;
      std::string n = lname(s);
      if (n == "style") {
        Style st;
        st.parent = attr(s, "parent-style-name");
        st.list_style = attr(s, "list-style-name");
        read_props(s, st);
        styles[attr(s, "family") + ":" + attr(s, "name")] = st;
      } else if (n == "default-style" && attr(s, "family") == "paragraph") {
        read_props(s, defaults);
      } else if (n == "list-style") {
        std::vector<ListLevel> levels(10);
        for (xmlNode *l = s->children; l; l = l->next) {
          if (l->type != XML_ELEMENT_NODE) continue;
          int lv = atoi(attr(l, "level").c_str());
          if (lv < 1 || lv > 10) continue;
          ListLevel &L = levels[(size_t)lv - 1];
          if (lname(l) == "list-level-style-number") {
            L.numbered = true;
            std::string f = attr(l, "num-format");
            if (!f.empty()) L.format = f;
            if (f.empty()) L.numbered = false, L.bullet = "";
          } else if (lname(l) == "list-level-style-bullet") {
            std::string c = attr(l, "bullet-char");
            // symbol-font bullets do not show in other fonts
            L.bullet = c.empty() || (unsigned char)c[0] >= 0xee ? "•" : c;
          }
        }
        list_styles[attr(s, "name")] = levels;
      }
    }
  }

  void read_page_layout(xmlNode *styles_root) {
    std::string layout_name;
    if (xmlNode *ms = child(styles_root, "master-styles"))
      if (xmlNode *mp = child(ms, "master-page")) layout_name = attr(mp, "page-layout-name");
    xmlNode *auto_styles = child(styles_root, "automatic-styles");
    for (xmlNode *s = auto_styles ? auto_styles->children : nullptr; s; s = s->next) {
      if (s->type != XML_ELEMENT_NODE || lname(s) != "page-layout") continue;
      if (!layout_name.empty() && attr(s, "name") != layout_name) continue;
      if (xmlNode *p = child(s, "page-layout-properties")) {
        page.w = parse_length(attr(p, "page-width"), page.w);
        page.h = parse_length(attr(p, "page-height"), page.h);
        page.left = parse_length(attr(p, "margin-left"), page.left);
        page.right = parse_length(attr(p, "margin-right"), page.right);
        page.top = parse_length(attr(p, "margin-top"), page.top);
        page.bottom = parse_length(attr(p, "margin-bottom"), page.bottom);
      }
      break;
    }
    // something sensible for odd values
    if (page.w < 144 || page.w > 5000) page.w = 595.3;
    if (page.h < 144 || page.h > 5000) page.h = 841.9;
    for (double *m : {&page.left, &page.right})
      if (*m < 0 || *m > page.w / 3) *m = 56.7;
    for (double *m : {&page.top, &page.bottom})
      if (*m < 0 || *m > page.h / 3) *m = 56.7;
  }

  // a style with its parents' settings filled in
  Style resolve(const std::string &family, const std::string &name) {
    Style out;
    std::vector<const Style *> chain;
    std::string n = name;
    for (int depth = 0; !n.empty() && depth < 16; depth++) {
      auto it = styles.find(family + ":" + n);
      if (it == styles.end()) break;
      chain.push_back(&it->second);
      n = it->second.parent;
    }
    chain.push_back(&defaults);
    double size = page.size;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const Style &s = **it;
      if (s.bold) out.bold = s.bold;
      if (s.italic) out.italic = s.italic;
      if (s.underline) out.underline = s.underline;
      if (s.strike) out.strike = s.strike;
      if (s.mono) out.mono = s.mono;
      if (s.size) size = *s.size;
      if (s.size_pct) size *= *s.size_pct;
      if (s.align) out.align = s.align;
      if (s.margin_left) out.margin_left = s.margin_left;
      if (s.margin_top) out.margin_top = s.margin_top;
      if (s.margin_bottom) out.margin_bottom = s.margin_bottom;
      if (s.text_indent) out.text_indent = s.text_indent;
      if (s.position) out.position = s.position;
      if (!s.list_style.empty()) out.list_style = s.list_style;
    }
    out.size = size;
    if (!chain.empty() && chain[0] != &defaults) {
      out.break_before = chain[0]->break_before;
      out.break_after = chain[0]->break_after;
    }
    return out;
  }

  // the Pango span for a text style, relative to the paragraph's
  std::string span_open(const Style &s, const Style &para) {
    std::string a;
    if (s.bold && *s.bold != para.bold.value_or(false)) a += *s.bold ? " weight=\"bold\"" : " weight=\"normal\"";
    if (s.italic && *s.italic != para.italic.value_or(false)) a += *s.italic ? " style=\"italic\"" : " style=\"normal\"";
    if (s.underline && *s.underline) a += " underline=\"single\"";
    if (s.strike && *s.strike) a += " strikethrough=\"true\"";
    if (s.mono && *s.mono) a += " face=\"Monospace\"";
    if (s.size && std::fabs(*s.size - *para.size) > 0.1) a += " size=\"" + std::to_string((int)(*s.size * 1024)) + "\"";
    std::string open = a.empty() ? "<span>" : "<span" + a + ">";
    if (s.position == std::optional<std::string>("super")) open += "<sup>";
    if (s.position == std::optional<std::string>("sub")) open += "<sub>";
    return open;
  }
  static std::string span_close(const Style &s) {
    std::string c;
    if (s.position == std::optional<std::string>("super")) c += "</sup>";
    if (s.position == std::optional<std::string>("sub")) c += "</sub>";
    return c + "</span>";
  }

  struct Para {
    std::string markup;
    bool last_space = true;
    std::vector<Block> after; // images and text boxes anchored in the paragraph
  };

  void text(Para &p, const std::string &t) {
    std::string o;
    for (char c : t) {
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
        if (!p.last_space) o += ' ';
        p.last_space = true;
      } else {
        o += c;
        p.last_space = false;
      }
    }
    p.markup += flow::escape(o);
  }

  void frame(xmlNode *f, Para &p) {
    double w = parse_length(attr(f, "width"), 0), h = parse_length(attr(f, "height"), 0);
    for (xmlNode *c = f->children; c; c = c->next) {
      if (c->type != XML_ELEMENT_NODE) continue;
      if (lname(c) == "image") {
        auto it = files.find(resolve_member("", attr(c, "href")));
        if (it == files.end()) continue;
        Block b;
        b.kind = Block::Image;
        b.image = it->second;
        b.image_w = w;
        b.image_h = h;
        p.after.push_back(b);
        return; // one picture per frame (others are replacements)
      }
      if (lname(c) == "text-box") {
        Odt sub(files);
        sub.styles = styles;
        sub.list_styles = list_styles;
        sub.fonts = fonts;
        sub.defaults = defaults;
        sub.page = page;
        sub.body(c, 0, "");
        p.after.insert(p.after.end(), sub.blocks.begin(), sub.blocks.end());
        return;
      }
    }
  }

  void inline_children(xmlNode *n, Para &p, const Style &para) {
    for (xmlNode *c = n->children; c; c = c->next) {
      if (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE) {
        if (c->content) text(p, (const char *)c->content);
        continue;
      }
      if (c->type != XML_ELEMENT_NODE) continue;
      std::string nm = lname(c);
      if (nm == "s") {
        int k = std::max(1, atoi(attr(c, "c").c_str()));
        p.markup += std::string((size_t)std::min(k, 200), ' ');
        p.last_space = true;
      } else if (nm == "tab") {
        p.markup += "\t";
        p.last_space = true;
      } else if (nm == "line-break") {
        p.markup += "\n";
        p.last_space = true;
      } else if (nm == "span") {
        Style s = resolve("text", attr(c, "style-name"));
        p.markup += span_open(s, para);
        inline_children(c, p, para);
        p.markup += span_close(s);
      } else if (nm == "note") {
        // the citation (a footnote's number); the note's text is left out
        if (xmlNode *cit = child(c, "note-citation")) {
          p.markup += "<sup>";
          inline_children(cit, p, para);
          p.markup += "</sup>";
        }
      } else if (nm == "frame") {
        frame(c, p);
      } else if (nm == "annotation" || nm == "annotation-end" || nm == "tracked-changes" || nm == "change" ||
                 nm == "soft-page-break" || nm == "bookmark" || nm == "bookmark-start" || nm == "bookmark-end") {
        // not shown
      } else {
        // a, span-like fields (dates, page numbers, ...), ruby, meta
        inline_children(c, p, para);
      }
    }
  }

  void paragraph(xmlNode *n, double indent, const std::string &bullet, bool heading) {
    Style s = resolve("paragraph", attr(n, "style-name"));
    if (s.break_before) page_break();
    int level = heading ? std::max(1, std::min(6, atoi(attr(n, "outline-level").c_str()))) : 0;
    if (heading && !styles.count("paragraph:" + attr(n, "style-name"))) {
      // headings without a style of their own
      static const double scale[] = {1.6, 1.4, 1.25, 1.1, 1.0, 1.0};
      s.size = page.size * scale[level - 1];
      s.bold = true;
    }
    Para p;
    inline_children(n, p, s);
    Block b;
    b.markup = fld::trim(p.markup);
    if (!b.markup.empty() || !bullet.empty()) {
      std::string open, close;
      if (s.bold.value_or(false)) open += "<b>", close = "</b>" + close;
      if (s.italic.value_or(false)) open += "<i>", close = "</i>" + close;
      if (s.underline.value_or(false)) open += "<u>", close = "</u>" + close;
      if (s.mono.value_or(false)) b.family = "Monospace";
      b.markup = open + b.markup + close;
      b.size = *s.size;
      b.align = (Block::Align)s.align.value_or(Block::Left);
      b.indent = std::max(0.0, indent + s.margin_left.value_or(0));
      b.first_indent = s.text_indent.value_or(0);
      b.space_before = s.margin_top.value_or(heading ? 10 : 0);
      b.space_after = s.margin_bottom.value_or(heading ? 4 : 2);
      b.bullet = bullet;
      if (b.markup.empty()) b.markup = " ";
      blocks.push_back(b);
    } else {
      // an empty paragraph is an empty line
      Block e;
      e.markup = " ";
      e.size = *s.size;
      blocks.push_back(e);
    }
    blocks.insert(blocks.end(), p.after.begin(), p.after.end());
    if (s.break_after) page_break();
  }

  void page_break() {
    Block b;
    b.kind = Block::PageBreak;
    blocks.push_back(b);
  }

  void list(xmlNode *n, int level, std::string style, double indent) {
    std::string own = attr(n, "style-name");
    if (!own.empty()) style = own;
    std::vector<ListLevel> levels = list_styles.count(style) ? list_styles[style] : std::vector<ListLevel>(10);
    const ListLevel &L = levels[(size_t)std::min(level, 9)];
    int counter = 0;
    double ind = indent + 22;
    for (xmlNode *item = n->children; item; item = item->next) {
      if (item->type != XML_ELEMENT_NODE) continue;
      std::string in = lname(item);
      if (in != "list-item" && in != "list-header") continue;
      counter++;
      std::string sv = attr(item, "start-value");
      if (!sv.empty()) counter = atoi(sv.c_str());
      std::string bullet = in == "list-header" ? "" : L.numbered ? number(counter, L.format) + "." : L.bullet;
      bool first = true;
      for (xmlNode *c = item->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        std::string cn = lname(c);
        if (cn == "p" || cn == "h") {
          paragraph(c, ind, first ? bullet : "", cn == "h");
          first = false;
        } else if (cn == "list") {
          list(c, level + 1, style, ind);
        } else {
          body_element(c, ind, style);
        }
      }
    }
  }

  void table(xmlNode *n) {
    Block t;
    t.kind = Block::Table;
    std::function<void(xmlNode *)> rows = [&](xmlNode *p) {
      for (xmlNode *r = p->children; r; r = r->next) {
        if (r->type != XML_ELEMENT_NODE) continue;
        std::string rn = lname(r);
        if (rn == "table-header-rows" || rn == "table-rows" || rn == "table-row-group") {
          rows(r);
          continue;
        }
        if (rn != "table-row") continue;
        std::vector<std::vector<Block>> cells;
        for (xmlNode *c = r->children; c; c = c->next) {
          if (c->type != XML_ELEMENT_NODE || lname(c) != "table-cell") continue;
          Odt sub(files);
          sub.styles = styles;
          sub.list_styles = list_styles;
          sub.fonts = fonts;
          sub.defaults = defaults;
          sub.page = page;
          sub.body(c, 0, "");
          int span = std::max(1, std::min(64, atoi(attr(c, "number-columns-repeated").c_str())));
          for (int k = 0; k < span; k++) cells.push_back(k ? std::vector<Block>{} : sub.blocks);
        }
        int rep = std::max(1, std::min(64, atoi(attr(r, "number-rows-repeated").c_str())));
        for (int k = 0; k < rep; k++) t.rows.push_back(cells);
      }
    };
    rows(n);
    // a sheet's endless empty repeats are dropped
    while (!t.rows.empty()) {
      bool empty = true;
      for (auto &c : t.rows.back())
        if (!c.empty()) empty = false;
      if (!empty) break;
      t.rows.pop_back();
    }
    if (!t.rows.empty()) blocks.push_back(t);
  }

  void body_element(xmlNode *c, double indent, const std::string &list_style) {
    std::string nm = lname(c);
    if (nm == "p" || nm == "h") paragraph(c, indent, "", nm == "h");
    else if (nm == "list") list(c, 0, list_style, indent);
    else if (nm == "table") table(c);
    else if (nm == "section" || nm == "index-body" || nm == "index-title" || nm == "table-of-content" ||
             nm == "alphabetical-index" || nm == "illustration-index" || nm == "user-index" || nm == "bibliography" ||
             nm == "deletion")
      body(c, indent, list_style);
    else if (nm == "frame") {
      Para p;
      frame(c, p);
      blocks.insert(blocks.end(), p.after.begin(), p.after.end());
    }
    // sequence-decls, forms, tracked-changes, variable-decls, ...: nothing
  }

  void body(xmlNode *n, double indent, const std::string &list_style) {
    for (xmlNode *c = n->children; c; c = c->next)
      if (c->type == XML_ELEMENT_NODE) body_element(c, indent, list_style);
  }
};

} // namespace

std::unique_ptr<Backend> open_odt(const std::string &path, std::string &err) {
  std::map<std::string, std::string> files;
  auto want = [](const std::string &n) {
    return n == "content.xml" || n == "styles.xml" || n == "meta.xml" || n.compare(0, 9, "Pictures/") == 0 ||
           n.compare(0, 6, "media/") == 0;
  };
  if (!read_members(path, want, files, err)) return nullptr;
  auto content = files.find("content.xml");
  if (content == files.end()) {
    err = "The document has no content.xml.";
    return nullptr;
  }
  Odt odt(files);
  odt.page = flow::PageStyle{595.3, 841.9, 56.7, 56.7, 56.7, 56.7, "Serif", 12};
  xmlDoc *styles = nullptr;
  if (files.count("styles.xml")) styles = parse_xml(files["styles.xml"]);
  xmlDoc *c = parse_xml(content->second);
  if (!c) {
    if (styles) xmlFreeDoc(styles);
    err = "The document's text cannot be read.";
    return nullptr;
  }
  xmlNode *croot = xmlDocGetRootElement(c);
  if (styles) {
    xmlNode *sroot = xmlDocGetRootElement(styles);
    odt.read_font_faces(sroot);
    odt.read_styles(child(sroot, "styles"));
    odt.read_styles(child(sroot, "automatic-styles"));
    odt.read_page_layout(sroot);
  }
  odt.read_font_faces(croot);
  odt.read_styles(child(croot, "automatic-styles"));
  // the body font: sans or serif, and its size
  if (odt.defaults.size) odt.page.size = *odt.defaults.size;
  if (styles) {
    xmlNode *sroot = xmlDocGetRootElement(styles);
    if (xmlNode *st = child(sroot, "styles"))
      for (xmlNode *s = st->children; s; s = s->next)
        if (s->type == XML_ELEMENT_NODE && lname(s) == "default-style" && attr(s, "family") == "paragraph")
          if (xmlNode *tp = child(s, "text-properties")) {
            std::string fam = fld::lower(odt.fonts.count(attr(tp, "font-name")) ? odt.fonts[attr(tp, "font-name")]
                                                                                    : attr(tp, "font-name"));
            for (const char *sans : {"sans", "arial", "helvetica", "calibri", "carlito", "verdana", "tahoma"})
              if (fam.find(sans) != std::string::npos) odt.page.family = "Sans";
          }
    xmlFreeDoc(styles);
  }
  odt.defaults.size = odt.page.size;
  if (xmlNode *body = child(croot, "body"))
    if (xmlNode *text = child(body, "text")) odt.body(text, 0, "");
  xmlFreeDoc(c);

  auto b = std::make_unique<flow::FlowBackend>();
  auto meta = files.find("meta.xml");
  if (meta != files.end())
    if (xmlDoc *m = parse_xml(meta->second)) {
      std::function<xmlNode *(xmlNode *)> find = [&](xmlNode *n) -> xmlNode * {
        for (; n; n = n->next) {
          if (n->type == XML_ELEMENT_NODE && lname(n) == "title") return n;
          if (xmlNode *f = find(n->children)) return f;
        }
        return nullptr;
      };
      if (xmlNode *t = find(xmlDocGetRootElement(m))) {
        xmlChar *v = xmlNodeGetContent(t);
        b->title_ = fld::trim(v ? (const char *)v : "");
        xmlFree(v);
      }
      xmlFreeDoc(m);
    }
  if (odt.blocks.empty()) {
    Block e;
    e.markup = " ";
    odt.blocks.push_back(e);
  }
  b->doc.begin(std::move(odt.blocks), odt.page);
  b->layout_step(0.1);
  return b;
}

} // namespace docs
