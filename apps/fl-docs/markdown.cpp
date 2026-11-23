// fl-docs: Markdown to HTML (see docs_util.h)
//
// CommonMark's blocks (headings, paragraphs, block quotes, lists, code,
// rules, HTML blocks, link reference definitions) and its inlines (emphasis
// by the delimiter rules, code spans, links, images, autolinks, escapes,
// entities, raw tags, hard breaks), plus GitHub's tables, strike-through and
// task lists. Not every corner of the specification: tabs are 4 columns,
// lazy continuation lines only continue a paragraph, and HTML blocks run to
// the next blank line. The HTML is read by libxml's forgiving HTML parser.
#include <cctype>
#include <cstring>
#include <map>
#include <vector>

#include "docs_util.h"

namespace docs {

namespace {

struct Ref {
  std::string url, title;
};

struct Ctx {
  std::map<std::string, Ref> refs;
  std::string h1; // the first level 1 heading, as text
};

bool is_punct(char c) { return c > 0 && c < 0x7f && ispunct((unsigned char)c); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string esc(const std::string &s) {
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

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) a++;
  while (b > a && is_space(s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string rtrim(std::string s) {
  while (!s.empty() && is_space(s.back())) s.pop_back();
  return s;
}

int indent_of(const std::string &l) {
  int n = 0;
  while (n < (int)l.size() && l[n] == ' ') n++;
  return n;
}

bool blank(const std::string &l) { return trim(l).empty(); }

// lower case, runs of space as one: how references are matched
std::string ref_key(const std::string &s) {
  std::string o;
  bool sp = false;
  for (char c : trim(s)) {
    if (is_space(c)) {
      sp = true;
      continue;
    }
    if (sp && !o.empty()) o += ' ';
    sp = false;
    o += (char)tolower((unsigned char)c);
  }
  return o;
}

// &name; &#123; &#x1F;
size_t entity_len(const std::string &s, size_t i) {
  size_t j = i + 1;
  if (j < s.size() && s[j] == '#') {
    j++;
    bool hex = j < s.size() && (s[j] == 'x' || s[j] == 'X');
    if (hex) j++;
    size_t d = j;
    while (j < s.size() && (hex ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j]))) j++;
    if (j == d || j - d > 7) return 0;
  } else {
    size_t d = j;
    while (j < s.size() && isalnum((unsigned char)s[j])) j++;
    if (j == d || j - d > 32) return 0;
  }
  return j < s.size() && s[j] == ';' ? j + 1 - i : 0;
}

// a tag as raw HTML: <a href="x">, </b>, <br/>, <!-- ... -->
size_t raw_tag_len(const std::string &s, size_t i) {
  if (s.compare(i, 4, "<!--") == 0) {
    size_t e = s.find("-->", i + 4);
    return e == std::string::npos ? 0 : e + 3 - i;
  }
  size_t j = i + 1;
  if (j < s.size() && s[j] == '/') j++;
  if (j >= s.size() || !isalpha((unsigned char)s[j])) return 0;
  while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '-')) j++;
  char q = 0;
  for (; j < s.size(); j++) {
    char c = s[j];
    if (q) {
      if (c == q) q = 0;
    } else if (c == '"' || c == '\'') {
      q = c;
    } else if (c == '<') {
      return 0;
    } else if (c == '>') {
      return j + 1 - i;
    }
  }
  return 0;
}

// <https://x>, <mailto:x>, <someone@example.com>
size_t autolink_len(const std::string &s, size_t i, std::string &url) {
  size_t e = s.find('>', i + 1);
  if (e == std::string::npos) return 0;
  std::string in = s.substr(i + 1, e - i - 1);
  if (in.empty()) return 0;
  for (char c : in)
    if (is_space(c) || c == '<') return 0;
  size_t colon = in.find(':');
  if (colon != std::string::npos && colon >= 2 && colon <= 32 && isalpha((unsigned char)in[0])) {
    bool ok = true;
    for (size_t k = 0; k < colon; k++)
      if (!isalnum((unsigned char)in[k]) && in[k] != '+' && in[k] != '.' && in[k] != '-') ok = false;
    if (ok) {
      url = in;
      return e + 1 - i;
    }
  }
  size_t at = in.find('@');
  if (at != std::string::npos && at > 0 && in.find('.', at) != std::string::npos && in.find('@', at + 1) == std::string::npos) {
    url = "mailto:" + in;
    return e + 1 - i;
  }
  return 0;
}

std::string inlines(const std::string &s, Ctx &ctx);

// the run of backticks at i and its closing run; false when none closes it
bool code_span(const std::string &s, size_t i, size_t &end, std::string &html) {
  size_t n = 0;
  while (i + n < s.size() && s[i + n] == '`') n++;
  size_t j = i + n;
  while (j < s.size()) {
    size_t k = s.find('`', j);
    if (k == std::string::npos) return false;
    size_t m = 0;
    while (k + m < s.size() && s[k + m] == '`') m++;
    if (m == n) {
      std::string c = s.substr(i + n, k - i - n);
      for (char &ch : c)
        if (ch == '\n') ch = ' ';
      if (c.size() >= 2 && c.front() == ' ' && c.back() == ' ' && trim(c) != "") c = c.substr(1, c.size() - 2);
      html = "<code>" + esc(c) + "</code>";
      end = k + m;
      return true;
    }
    j = k + m;
  }
  return false;
}

// the "]" closing the "[" at i (nested brackets, code spans and escapes
// skipped); npos when none
size_t close_bracket(const std::string &s, size_t i) {
  int depth = 0;
  for (size_t j = i; j < s.size(); j++) {
    char c = s[j];
    if (c == '\\') {
      j++;
    } else if (c == '`') {
      size_t end;
      std::string h;
      if (code_span(s, j, end, h)) j = end - 1;
      else
        while (j + 1 < s.size() && s[j + 1] == '`') j++;
    } else if (c == '[') {
      depth++;
    } else if (c == ']') {
      if (--depth == 0) return j;
    }
  }
  return std::string::npos;
}

std::string unescape(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size() && is_punct(s[i + 1])) i++;
    o += s[i];
  }
  return o;
}

// "(dest "title")" at i; sets end past ")"
bool inline_dest(const std::string &s, size_t i, size_t &end, std::string &url, std::string &title) {
  if (i >= s.size() || s[i] != '(') return false;
  size_t j = i + 1;
  while (j < s.size() && is_space(s[j])) j++;
  if (j < s.size() && s[j] == '<') {
    size_t e = s.find('>', j);
    if (e == std::string::npos || s.find('\n', j) < e) return false;
    url = s.substr(j + 1, e - j - 1);
    j = e + 1;
  } else {
    int depth = 0;
    size_t a = j;
    while (j < s.size() && !is_space(s[j])) {
      if (s[j] == '\\' && j + 1 < s.size()) {
        j += 2;
        continue;
      }
      if (s[j] == '(') depth++;
      if (s[j] == ')') {
        if (depth == 0) break;
        depth--;
      }
      j++;
    }
    url = s.substr(a, j - a);
  }
  while (j < s.size() && is_space(s[j])) j++;
  if (j < s.size() && (s[j] == '"' || s[j] == '\'' || s[j] == '(')) {
    char close = s[j] == '(' ? ')' : s[j];
    size_t e = j + 1;
    while (e < s.size() && s[e] != close) e += s[e] == '\\' ? 2 : 1;
    if (e >= s.size()) return false;
    title = s.substr(j + 1, e - j - 1);
    j = e + 1;
    while (j < s.size() && is_space(s[j])) j++;
  }
  if (j >= s.size() || s[j] != ')') return false;
  url = unescape(url);
  title = unescape(title);
  end = j + 1;
  return true;
}

// text without its tags, entities of the escaper decoded (titles)
std::string plain(const std::string &html) {
  std::string o;
  bool tag = false;
  for (size_t i = 0; i < html.size(); i++) {
    char c = html[i];
    if (c == '<') tag = true;
    else if (c == '>') tag = false;
    else if (!tag) {
      if (c == '&') {
        static const std::pair<const char *, char> ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}};
        bool done = false;
        for (auto &e : ents)
          if (html.compare(i, strlen(e.first), e.first) == 0) {
            o += e.second;
            i += strlen(e.first) - 1;
            done = true;
            break;
          }
        if (done) continue;
      }
      o += c;
    }
  }
  return trim(o);
}

struct Node {
  std::string html;       // text: its HTML; a delimiter run: tags around it
  char delim = 0;         // '*', '_', '~' or 0 for text
  int count = 0, orig = 0;
  bool open = false, close = false, active = true;
  std::string before, after; // a delimiter run: closing tags, then its characters, then opening tags
};

void emphasis(std::vector<Node> &nodes) {
  for (size_t c = 0; c < nodes.size(); c++) {
    Node &cl = nodes[c];
    if (!cl.delim || !cl.close || !cl.active) continue;
    while (cl.count > 0) {
      // the nearest opener of the same kind
      long found = -1;
      for (long o = (long)c - 1; o >= 0; o--) {
        Node &op = nodes[o];
        if (op.delim != cl.delim || !op.open || !op.active || op.count == 0) continue;
        if (cl.delim == '~') {
          if (op.count != cl.count) continue;
        } else if ((op.close || cl.open) && (op.orig + cl.orig) % 3 == 0 && !(op.orig % 3 == 0 && cl.orig % 3 == 0)) {
          continue;
        }
        found = o;
        break;
      }
      if (found < 0) break;
      Node &op = nodes[found];
      int n;
      std::string tag;
      if (cl.delim == '~') {
        n = cl.count;
        tag = "s";
      } else {
        n = op.count >= 2 && cl.count >= 2 ? 2 : 1;
        tag = n == 2 ? "b" : "i";
      }
      op.count -= n;
      cl.count -= n;
      op.after = "<" + tag + ">" + op.after;
      cl.before += "</" + tag + ">";
      // the delimiters between can no longer match
      for (size_t k = found + 1; k < c; k++) nodes[k].active = false;
    }
  }
}

std::string inlines(const std::string &s, Ctx &ctx) {
  std::vector<Node> nodes;
  std::string text;
  auto push_text = [&]() {
    if (text.empty()) return;
    Node n;
    n.html = text;
    nodes.push_back(n);
    text.clear();
  };
  auto link = [&](size_t i, bool image, size_t &next) -> bool {
    size_t lb = image ? i + 1 : i;
    size_t rb = close_bracket(s, lb);
    if (rb == std::string::npos) return false;
    std::string label = s.substr(lb + 1, rb - lb - 1);
    std::string url, title;
    size_t end;
    bool ok = false;
    if (inline_dest(s, rb + 1, end, url, title)) {
      ok = true;
    } else {
      // [text][ref], [ref][], [ref]
      std::string key = label;
      end = rb + 1;
      if (rb + 1 < s.size() && s[rb + 1] == '[') {
        size_t r2 = s.find(']', rb + 2);
        if (r2 != std::string::npos) {
          std::string k2 = s.substr(rb + 2, r2 - rb - 2);
          if (!trim(k2).empty()) key = k2;
          end = r2 + 1;
        }
      }
      auto it = ctx.refs.find(ref_key(key));
      if (it != ctx.refs.end()) {
        url = it->second.url;
        title = it->second.title;
        ok = true;
      } else if (end != rb + 1) {
        // [text][missing]: not a link; try [text] alone
        it = ctx.refs.find(ref_key(label));
        if (it != ctx.refs.end()) {
          url = it->second.url;
          title = it->second.title;
          end = rb + 1;
          ok = true;
        }
      }
    }
    if (!ok) return false;
    push_text();
    Node n;
    std::string inner = inlines(label, ctx);
    if (image)
      n.html = "<img src=\"" + esc(url) + "\" alt=\"" + esc(plain(inner)) + "\"" +
               (title.empty() ? "" : " title=\"" + esc(title) + "\"") + "/>";
    else
      n.html = "<a href=\"" + esc(url) + "\">" + inner + "</a>";
    nodes.push_back(n);
    next = end;
    return true;
  };

  for (size_t i = 0; i < s.size();) {
    char c = s[i];
    if (c == '\\') {
      if (i + 1 < s.size() && s[i + 1] == '\n') {
        text += "<br/>\n";
        i += 2;
        while (i < s.size() && s[i] == ' ') i++;
      } else if (i + 1 < s.size() && is_punct(s[i + 1])) {
        text += esc(std::string(1, s[i + 1]));
        i += 2;
      } else {
        text += '\\';
        i++;
      }
    } else if (c == '`') {
      size_t end;
      std::string h;
      if (code_span(s, i, end, h)) {
        text += h;
        i = end;
      } else {
        while (i < s.size() && s[i] == '`') text += s[i++];
      }
    } else if (c == '!' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t next;
      if (link(i, true, next)) i = next;
      else text += s[i++];
    } else if (c == '[') {
      size_t next;
      if (link(i, false, next)) i = next;
      else text += s[i++];
    } else if (c == '<') {
      std::string url;
      size_t n;
      if ((n = autolink_len(s, i, url))) {
        text += "<a href=\"" + esc(url) + "\">" + esc(s.substr(i + 1, n - 2)) + "</a>";
        i += n;
      } else if ((n = raw_tag_len(s, i))) {
        text += s.substr(i, n);
        i += n;
      } else {
        text += "&lt;";
        i++;
      }
    } else if (c == '&') {
      size_t n = entity_len(s, i);
      text += n ? s.substr(i, n) : "&amp;";
      i += n ? n : 1;
    } else if (c == '\n') {
      // two spaces at the end of a line: a hard break
      size_t sp = 0;
      while (!text.empty() && text.back() == ' ') {
        text.pop_back();
        sp++;
      }
      text += sp >= 2 ? "<br/>\n" : "\n";
      i++;
      while (i < s.size() && s[i] == ' ') i++;
    } else if (c == '*' || c == '_' || c == '~') {
      size_t n = 0;
      while (i + n < s.size() && s[i + n] == c) n++;
      char prev = i == 0 ? ' ' : s[i - 1];
      char next = i + n < s.size() ? s[i + n] : ' ';
      bool left = !is_space(next) && (!is_punct(next) || is_space(prev) || is_punct(prev));
      bool right = !is_space(prev) && (!is_punct(prev) || is_space(next) || is_punct(next));
      // GitHub: "~" works in runs of one or two only
      if (c == '~' && n > 2) left = right = false;
      push_text();
      Node d;
      d.delim = c;
      d.count = d.orig = (int)n;
      if (c == '_') {
        d.open = left && (!right || is_punct(prev));
        d.close = right && (!left || is_punct(next));
      } else {
        d.open = left;
        d.close = right;
      }
      nodes.push_back(d);
      i += n;
    } else if (c == '>') {
      text += "&gt;";
      i++;
    } else if (c == '"') {
      text += "&quot;";
      i++;
    } else {
      text += c;
      i++;
    }
  }
  push_text();
  emphasis(nodes);
  std::string o;
  for (auto &n : nodes) {
    if (!n.delim) o += n.html;
    else o += n.before + std::string(n.count, n.delim) + n.after;
  }
  // a hard break at the very end is not one
  while (o.size() >= 6 && o.compare(o.size() - 6, 6, "<br/>\n") == 0) o.erase(o.size() - 6);
  return rtrim(o);
}

// ---- blocks ----

std::string expand_tabs(const std::string &l) {
  std::string o;
  for (char c : l) {
    if (c == '\t') o.append(4 - o.size() % 4, ' ');
    else if (c != '\r') o += c;
  }
  return o;
}

bool atx(const std::string &l, int &level, std::string &text) {
  int in = indent_of(l);
  if (in > 3) return false;
  size_t i = in;
  int n = 0;
  while (i < l.size() && l[i] == '#') i++, n++;
  if (n < 1 || n > 6 || (i < l.size() && l[i] != ' ')) return false;
  std::string t = trim(l.substr(i));
  // closing #s (after a space)
  size_t e = t.size();
  while (e > 0 && t[e - 1] == '#') e--;
  if (e == 0) t.clear();
  else if (e < t.size() && t[e - 1] == ' ') t = rtrim(t.substr(0, e));
  level = n;
  text = t;
  return true;
}

bool rule(const std::string &l) {
  if (indent_of(l) > 3) return false;
  char m = 0;
  int n = 0;
  for (char c : l) {
    if (c == ' ') continue;
    if (c != '*' && c != '-' && c != '_') return false;
    if (m && c != m) return false;
    m = c;
    n++;
  }
  return n >= 3;
}

// ``` or ~~~ (with its indent)
bool fence(const std::string &l, char &ch, size_t &len, int &in) {
  in = indent_of(l);
  if (in > 3 || in >= (int)l.size()) return false;
  char c = l[in];
  if (c != '`' && c != '~') return false;
  size_t n = 0;
  while (in + n < l.size() && l[in + n] == c) n++;
  if (n < 3) return false;
  if (c == '`' && l.find('`', in + n) != std::string::npos) return false;
  ch = c;
  len = n;
  return true;
}

struct Marker {
  bool ordered = false;
  char ch = 0; // '-', '+', '*' or '.', ')'
  int start = 1;
  int content = 0; // the column the item's content starts at
  bool empty = false;
};

bool list_marker(const std::string &l, Marker &m) {
  int in = indent_of(l);
  if (in > 3) return false;
  size_t i = in;
  if (i >= l.size()) return false;
  if (l[i] == '-' || l[i] == '+' || l[i] == '*') {
    m.ordered = false;
    m.ch = l[i];
    i++;
  } else if (isdigit((unsigned char)l[i])) {
    size_t d = i;
    while (i < l.size() && isdigit((unsigned char)l[i])) i++;
    if (i - d > 9 || i >= l.size() || (l[i] != '.' && l[i] != ')')) return false;
    m.ordered = true;
    m.start = atoi(l.substr(d, i - d).c_str());
    m.ch = l[i];
    i++;
  } else {
    return false;
  }
  if (i < l.size() && l[i] != ' ') return false;
  size_t sp = 0;
  while (i + sp < l.size() && l[i + sp] == ' ') sp++;
  m.empty = i + sp >= l.size();
  m.content = (int)i + (m.empty || sp > 4 ? 1 : (int)sp);
  return true;
}

bool quote_line(const std::string &l, std::string &rest) {
  int in = indent_of(l);
  if (in > 3 || in >= (int)l.size() || l[in] != '>') return false;
  size_t i = in + 1;
  if (i < l.size() && l[i] == ' ') i++;
  rest = l.substr(i);
  return true;
}

bool html_block(const std::string &l) {
  int in = indent_of(l);
  if (in > 3 || in >= (int)l.size() || l[in] != '<') return false;
  if (l.compare(in, 4, "<!--") == 0 || l.compare(in, 2, "<?") == 0 || l.compare(in, 2, "<!") == 0) return true;
  size_t i = in + 1;
  if (i < l.size() && l[i] == '/') i++;
  size_t a = i;
  while (i < l.size() && isalnum((unsigned char)l[i])) i++;
  std::string tag = l.substr(a, i - a);
  for (char &c : tag) c = (char)tolower((unsigned char)c);
  if (tag.empty() || (i < l.size() && l[i] != ' ' && l[i] != '>' && l[i] != '/')) return false;
  static const char *blocks[] = {
      "address", "article", "aside", "blockquote", "center", "details", "dialog", "dd", "div", "dl", "dt",
      "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "li",
      "main", "nav", "ol", "p", "pre", "section", "summary", "table", "tbody", "td", "tfoot", "th", "thead",
      "tr", "ul", "script", "style", "img", "picture", "video", "audio", "iframe"};
  for (const char *b : blocks)
    if (tag == b) return true;
  return false;
}

// [label]: url "title"
bool ref_def(const std::string &l, std::string &key, Ref &ref) {
  int in = indent_of(l);
  if (in > 3 || in >= (int)l.size() || l[in] != '[') return false;
  size_t rb = l.find("]:", in);
  if (rb == std::string::npos || rb == (size_t)in + 1) return false;
  std::string label = l.substr(in + 1, rb - in - 1);
  if (label.find('[') != std::string::npos || label.find(']') != std::string::npos) return false;
  std::string rest = trim(l.substr(rb + 2));
  if (rest.empty()) return false;
  size_t e;
  std::string url;
  if (rest[0] == '<') {
    e = rest.find('>');
    if (e == std::string::npos) return false;
    url = rest.substr(1, e - 1);
    e++;
  } else {
    e = 0;
    while (e < rest.size() && !is_space(rest[e])) e++;
    url = rest.substr(0, e);
  }
  std::string t = trim(rest.substr(e));
  std::string title;
  if (!t.empty()) {
    char o = t[0], c = o == '(' ? ')' : o;
    if ((o != '"' && o != '\'' && o != '(') || t.size() < 2 || t.back() != c) return false;
    title = t.substr(1, t.size() - 2);
  }
  key = ref_key(label);
  ref.url = unescape(url);
  ref.title = unescape(title);
  return !key.empty();
}

// a table's delimiter row: | :--- | ---: |
bool table_delim(const std::string &l, size_t &cols) {
  std::string t = trim(l);
  if (t.find('-') == std::string::npos) return false;
  if (!t.empty() && t[0] == '|') t.erase(0, 1);
  if (!t.empty() && t.back() == '|') t.pop_back();
  cols = 0;
  size_t start = 0;
  while (true) {
    size_t p = t.find('|', start);
    std::string c = trim(t.substr(start, p == std::string::npos ? std::string::npos : p - start));
    if (c.empty()) return false;
    size_t a = c[0] == ':' ? 1 : 0, b = c.back() == ':' && c.size() > 1 ? c.size() - 1 : c.size();
    if (a >= b) return false;
    for (size_t k = a; k < b; k++)
      if (c[k] != '-') return false;
    cols++;
    if (p == std::string::npos) break;
    start = p + 1;
  }
  return cols > 0;
}

std::vector<std::string> table_cells(const std::string &l) {
  std::string t = trim(l);
  if (!t.empty() && t[0] == '|') t.erase(0, 1);
  if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
  std::vector<std::string> cells;
  std::string cur;
  bool code = false;
  for (size_t i = 0; i < t.size(); i++) {
    char c = t[i];
    if (c == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
      cur += '|';
      i++;
    } else if (c == '`') {
      code = !code;
      cur += c;
    } else if (c == '|' && !code) {
      cells.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  cells.push_back(trim(cur));
  return cells;
}

std::vector<std::string> table_aligns(const std::string &l) {
  std::vector<std::string> a;
  for (const std::string &c : table_cells(l)) {
    bool left = !c.empty() && c[0] == ':', right = !c.empty() && c.back() == ':';
    a.push_back(left && right ? "center" : right ? "right" : left ? "left" : "");
  }
  return a;
}

// a line that ends a paragraph without a blank line before it
bool interrupts(const std::string &l) {
  int lv;
  std::string t;
  char ch;
  size_t len;
  int in;
  Marker m;
  std::string rest;
  if (atx(l, lv, t) || rule(l) || fence(l, ch, len, in) || quote_line(l, rest) || html_block(l)) return true;
  if (list_marker(l, m) && !m.empty && (!m.ordered || m.start == 1)) return true;
  return false;
}

std::string blocks(const std::vector<std::string> &lines, Ctx &ctx, bool tight);

std::string list(const std::vector<std::string> &lines, size_t &i, Ctx &ctx) {
  Marker first;
  list_marker(lines[i], first);
  std::vector<std::vector<std::string>> items;
  bool loose = false;
  size_t n = lines.size();
  while (i < n) {
    Marker m;
    if (!list_marker(lines[i], m) || m.ordered != first.ordered || m.ch != first.ch || rule(lines[i])) break;
    std::vector<std::string> item;
    const std::string &l = lines[i];
    item.push_back((int)l.size() > m.content ? l.substr(m.content) : "");
    i++;
    // an item that starts blank holds at most one blank line
    if (m.empty && i < n && blank(lines[i])) {
      items.push_back(item);
      continue;
    }
    while (i < n) {
      const std::string &x = lines[i];
      if (blank(x)) {
        item.push_back("");
        i++;
        continue;
      }
      if (indent_of(x) >= m.content) {
        item.push_back(x.substr(m.content));
        i++;
        continue;
      }
      // lazy continuation of the item's last paragraph
      if (!blank(item.back()) && !interrupts(x) && !rule(x)) {
        Marker other;
        if (!list_marker(x, other)) {
          item.push_back(trim(x));
          i++;
          continue;
        }
      }
      break;
    }
    // blank lines at the end belong between the items
    size_t trailing = 0;
    while (!item.empty() && blank(item.back())) {
      item.pop_back();
      trailing++;
    }
    for (size_t k = 1; k < item.size(); k++)
      if (blank(item[k]) && k + 1 < item.size()) {
        // a blank line between two of the item's own blocks
        Marker nested;
        if (!(list_marker(item[k + 1], nested) && k > 0 && list_marker(item[k - 1], nested))) loose = true;
      }
    items.push_back(item);
    if (trailing > 0) {
      Marker next;
      if (i < n && list_marker(lines[i], next) && next.ordered == first.ordered && next.ch == first.ch && !rule(lines[i]))
        loose = true;
      else
        break;
    }
  }
  std::string o = first.ordered ? (first.start != 1 ? "<ol start=\"" + std::to_string(first.start) + "\">\n" : "<ol>\n")
                                : "<ul>\n";
  for (auto &item : items) {
    // GitHub task lists
    if (!item.empty()) {
      std::string &f = item[0];
      if (f.size() >= 3 && f[0] == '[' && f[2] == ']' && (f.size() == 3 || f[3] == ' ')) {
        if (f[1] == ' ') f = "\xe2\x98\x90" + f.substr(3);           // ☐
        else if (f[1] == 'x' || f[1] == 'X') f = "\xe2\x98\x91" + f.substr(3); // ☑
      }
    }
    o += "<li>" + rtrim(blocks(item, ctx, !loose)) + "</li>\n";
  }
  o += first.ordered ? "</ol>\n" : "</ul>\n";
  return o;
}

std::string blocks(const std::vector<std::string> &lines, Ctx &ctx, bool tight) {
  std::string o;
  size_t n = lines.size();
  size_t i = 0;
  while (i < n) {
    const std::string &l = lines[i];
    int level;
    std::string text, rest;
    char fch;
    size_t flen;
    int fin;
    Marker m;
    size_t cols;
    if (blank(l)) {
      i++;
    } else if (fence(l, fch, flen, fin)) {
      std::string info = trim(l.substr(fin + flen));
      std::vector<std::string> code;
      i++;
      while (i < n) {
        char c2;
        size_t len2;
        int in2;
        if (fence(lines[i], c2, len2, in2) && c2 == fch && len2 >= flen && trim(lines[i].substr(in2 + len2)).empty()) {
          i++;
          break;
        }
        std::string x = lines[i];
        int strip = std::min(fin, indent_of(x));
        code.push_back(x.substr(strip));
        i++;
      }
      std::string body;
      for (size_t k = 0; k < code.size(); k++) body += (k ? "\n" : "") + code[k];
      o += "<pre><code>" + esc(body) + "</code></pre>\n";
    } else if (indent_of(l) >= 4) {
      std::vector<std::string> code;
      while (i < n && (blank(lines[i]) || indent_of(lines[i]) >= 4)) {
        code.push_back(blank(lines[i]) ? "" : lines[i].substr(4));
        i++;
      }
      while (!code.empty() && code.back().empty()) code.pop_back();
      std::string body;
      for (size_t k = 0; k < code.size(); k++) body += (k ? "\n" : "") + code[k];
      o += "<pre><code>" + esc(body) + "</code></pre>\n";
    } else if (atx(l, level, text)) {
      std::string h = inlines(text, ctx);
      if (level == 1 && ctx.h1.empty()) ctx.h1 = plain(h);
      std::string t = "h" + std::to_string(level);
      o += "<" + t + ">" + h + "</" + t + ">\n";
      i++;
    } else if (rule(l)) {
      o += "<hr/>\n";
      i++;
    } else if (quote_line(l, rest)) {
      std::vector<std::string> inner;
      bool para = false;
      while (i < n) {
        std::string r;
        if (quote_line(lines[i], r)) {
          inner.push_back(r);
          para = !blank(r);
          i++;
        } else if (para && !blank(lines[i]) && !interrupts(lines[i])) {
          inner.push_back(lines[i]);
          i++;
        } else {
          break;
        }
      }
      o += "<blockquote>\n" + blocks(inner, ctx, false) + "</blockquote>\n";
    } else if (list_marker(l, m)) {
      o += list(lines, i, ctx);
    } else if (html_block(l)) {
      while (i < n && !blank(lines[i])) o += lines[i++] + "\n";
    } else if (l.find('|') != std::string::npos && i + 1 < n && table_delim(lines[i + 1], cols) &&
               table_cells(l).size() == cols) {
      std::vector<std::string> aligns = table_aligns(lines[i + 1]);
      auto row = [&](const std::string &line, const char *tag) {
        std::vector<std::string> cells = table_cells(line);
        std::string r = "<tr>";
        for (size_t k = 0; k < cols; k++) {
          std::string a = k < aligns.size() && !aligns[k].empty() ? " style=\"text-align:" + aligns[k] + "\"" : "";
          r += std::string("<") + tag + a + ">" + (k < cells.size() ? inlines(cells[k], ctx) : "") + "</" + tag + ">";
        }
        return r + "</tr>\n";
      };
      o += "<table>\n<thead>\n" + row(l, "th") + "</thead>\n<tbody>\n";
      i += 2;
      while (i < n && !blank(lines[i]) && !interrupts(lines[i])) o += row(lines[i++], "td");
      o += "</tbody>\n</table>\n";
    } else {
      // link reference definitions, then a paragraph
      std::string key;
      Ref ref;
      if (ref_def(l, key, ref)) {
        i++;
        continue;
      }
      std::vector<std::string> para;
      para.push_back(l.substr(indent_of(l)));
      i++;
      int setext = 0;
      while (i < n) {
        const std::string &x = lines[i];
        if (blank(x)) break;
        std::string t = trim(x);
        if (indent_of(x) <= 3 && !t.empty() && t.find_first_not_of('=') == std::string::npos) {
          setext = 1;
          i++;
          break;
        }
        if (indent_of(x) <= 3 && !t.empty() && t.find_first_not_of('-') == std::string::npos) {
          setext = 2;
          i++;
          break;
        }
        if (interrupts(x)) break;
        // keep trailing spaces: two make a hard break
        std::string kept = x.substr(indent_of(x));
        para.push_back(kept);
        i++;
      }
      std::string joined;
      for (size_t k = 0; k < para.size(); k++) {
        std::string p = para[k];
        if (k + 1 == para.size()) p = rtrim(p);
        joined += (k ? "\n" : "") + p;
      }
      std::string h = inlines(joined, ctx);
      if (setext) {
        if (setext == 1 && ctx.h1.empty()) ctx.h1 = plain(h);
        o += "<h" + std::to_string(setext) + ">" + h + "</h" + std::to_string(setext) + ">\n";
      } else if (tight) {
        o += h + "\n";
      } else {
        o += "<p>" + h + "</p>\n";
      }
    }
  }
  return o;
}

// every reference definition, wherever it is (they may come after their use)
void collect_refs(const std::vector<std::string> &lines, Ctx &ctx) {
  bool in_code = false;
  char fch = 0;
  size_t flen = 0;
  for (const std::string &raw : lines) {
    std::string l = raw;
    // inside quotes and list items too
    for (int guard = 0; guard < 16; guard++) {
      std::string r;
      Marker m;
      if (quote_line(l, r)) l = r;
      else if (list_marker(l, m) && !m.empty) l = l.substr(m.content);
      else break;
    }
    char c;
    size_t len;
    int in;
    if (fence(l, c, len, in)) {
      if (!in_code) {
        in_code = true;
        fch = c;
        flen = len;
      } else if (c == fch && len >= flen) {
        in_code = false;
      }
      continue;
    }
    if (in_code) continue;
    std::string key;
    Ref ref;
    if (ref_def(l, key, ref) && !ctx.refs.count(key)) ctx.refs[key] = ref;
  }
}

} // namespace

std::string markdown_to_html(const std::string &md, std::string *title) {
  std::vector<std::string> lines;
  size_t start = 0;
  // a byte order mark
  if (md.compare(0, 3, "\xef\xbb\xbf") == 0) start = 3;
  while (start <= md.size()) {
    size_t e = md.find('\n', start);
    if (e == std::string::npos) e = md.size();
    lines.push_back(expand_tabs(md.substr(start, e - start)));
    start = e + 1;
  }
  std::string fm_title;
  // YAML front matter
  if (!lines.empty() && rtrim(lines[0]) == "---") {
    for (size_t k = 1; k < lines.size(); k++) {
      std::string t = rtrim(lines[k]);
      if (t == "---" || t == "...") {
        for (size_t j = 1; j < k; j++) {
          const std::string &f = lines[j];
          if (f.compare(0, 6, "title:") == 0) {
            std::string v = trim(f.substr(6));
            if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
            fm_title = v;
          }
        }
        lines.erase(lines.begin(), lines.begin() + k + 1);
        break;
      }
      // not front matter after all: a rule and text
      if (k > 200) break;
    }
  }
  Ctx ctx;
  collect_refs(lines, ctx);
  std::string body = blocks(lines, ctx, false);
  if (title) *title = !fm_title.empty() ? fm_title : ctx.h1;
  return body;
}

bool is_markdown_name(const std::string &path) {
  size_t slash = path.find_last_of('/');
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
  std::string e = path.substr(dot + 1);
  for (char &c : e) c = (char)tolower((unsigned char)c);
  for (const char *x : {"md", "markdown", "mdown", "mkd", "mkdn", "mdwn", "mdtext", "mdtxt"})
    if (e == x) return true;
  return false;
}

} // namespace docs
