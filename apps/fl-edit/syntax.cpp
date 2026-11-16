#include "syntax.h"

#include <cctype>
#include <cstring>
#include <set>

namespace syntax {

static std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

Language detect(const std::string &filename, const std::string &first_line) {
  std::string base = filename.substr(filename.rfind('/') == std::string::npos ? 0 : filename.rfind('/') + 1);
  std::string lb = lower(base);
  size_t dot = lb.rfind('.');
  std::string ext = dot == std::string::npos ? "" : lb.substr(dot + 1);
  if (ext == "c" || ext == "h" || ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "hpp" || ext == "hh" ||
      ext == "hxx" || ext == "ino" || ext == "m")
    return C_CPP;
  if (ext == "py" || ext == "pyw") return PYTHON;
  if (ext == "sh" || ext == "bash" || ext == "zsh" || lb == ".bashrc" || lb == ".profile" || lb == "pkgbuild" ||
      lb == ".zshrc")
    return SHELL;
  if (ext == "js" || ext == "mjs" || ext == "ts" || ext == "jsx" || ext == "tsx") return JAVASCRIPT;
  if (ext == "rs") return RUST;
  if (ext == "go") return GO;
  if (ext == "java" || ext == "kt" || ext == "cs") return JAVA;
  if (ext == "ini" || ext == "conf" || ext == "desktop" || ext == "cfg" || ext == "toml" || ext == "service")
    return INI;
  if (ext == "md" || ext == "markdown") return MARKDOWN;
  if (lb == "cmakelists.txt" || ext == "cmake") return CMAKE;
  if (ext == "json") return JSON;
  if (ext == "xml" || ext == "html" || ext == "htm" || ext == "svg" || ext == "ui" || ext == "xhtml") return XML;
  if (ext == "css" || ext == "scss") return CSS;
  if (lb == "makefile" || ext == "mk") return SHELL;
  if (first_line.rfind("#!", 0) == 0) {
    if (first_line.find("python") != std::string::npos) return PYTHON;
    if (first_line.find("sh") != std::string::npos) return SHELL;
    if (first_line.find("node") != std::string::npos) return JAVASCRIPT;
  }
  return NONE;
}

const char *language_name(Language l) {
  switch (l) {
    case C_CPP: return "C/C++";
    case PYTHON: return "Python";
    case SHELL: return "Shell";
    case JAVASCRIPT: return "JavaScript";
    case RUST: return "Rust";
    case GO: return "Go";
    case JAVA: return "Java";
    case INI: return "INI";
    case MARKDOWN: return "Markdown";
    case CMAKE: return "CMake";
    case JSON: return "JSON";
    case XML: return "XML/HTML";
    case CSS: return "CSS";
    default: return "Plain Text";
  }
}

namespace {

struct Lang {
  const char *line_comment;  // e.g. "//" or "#"
  const char *block_open;    // e.g. "/*"
  const char *block_close;
  bool preproc;              // '#' lines are preprocessor
  bool triple_quotes;        // python
  bool single_quote_strings;
  bool backtick_strings;
  std::set<std::string> keywords, types;
};

std::set<std::string> words(const char *s) {
  std::set<std::string> r;
  std::string cur;
  for (const char *p = s;; p++) {
    if (!*p || *p == ' ') {
      if (!cur.empty()) r.insert(cur);
      cur.clear();
      if (!*p) break;
    } else cur += *p;
  }
  return r;
}

const Lang &lang_def(Language l) {
  static Lang c{"//", "/*", "*/", true, false, true, false,
                words("if else for while do switch case default break continue return goto sizeof typedef struct "
                      "union enum class public private protected virtual override final static const constexpr "
                      "extern inline volatile register template typename namespace using new delete this "
                      "operator friend try catch throw noexcept nullptr true false auto decltype explicit mutable "
                      "static_cast dynamic_cast reinterpret_cast const_cast co_await co_return co_yield"),
                words("void bool char short int long float double signed unsigned size_t ssize_t uint8_t uint16_t "
                      "uint32_t uint64_t int8_t int16_t int32_t int64_t wchar_t std string vector map FILE")};
  static Lang py{"#", nullptr, nullptr, false, true, true, false,
                 words("and as assert async await break class continue def del elif else except finally for from "
                       "global if import in is lambda nonlocal not or pass raise return try while with yield True "
                       "False None self match case"),
                 words("int float str bool list dict set tuple bytes object type print len range open")};
  static Lang sh{"#", nullptr, nullptr, false, false, true, true,
                 words("if then else elif fi for while until do done case esac in function return exit local "
                       "export readonly set unset shift break continue source alias declare"),
                 words("echo printf cd test read eval exec trap true false")};
  static Lang js{"//", "/*", "*/", false, false, true, true,
                 words("var let const function return if else for while do switch case default break continue new "
                       "delete typeof instanceof in of class extends super this import export from as async await "
                       "try catch finally throw yield null undefined true false interface type enum implements"),
                 words("string number boolean any void never unknown Object Array Promise")};
  static Lang rs{"//", "/*", "*/", false, false, false, false,
                 words("as async await break const continue crate dyn else enum extern false fn for if impl in let "
                       "loop match mod move mut pub ref return self Self static struct super trait true type unsafe "
                       "use where while"),
                 words("i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec "
                       "Option Result Box")};
  static Lang go{"//", "/*", "*/", false, false, false, true,
                 words("break case chan const continue default defer else fallthrough for func go goto if import "
                       "interface map package range return select struct switch type var nil true false"),
                 words("bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string "
                       "uint uint8 uint16 uint32 uint64 uintptr")};
  static Lang java{"//", "/*", "*/", false, false, true, false,
                   words("abstract assert break case catch class const continue default do else enum extends final "
                         "finally for goto if implements import instanceof interface native new package private "
                         "protected public return static strictfp super switch synchronized this throw throws "
                         "transient try volatile while true false null var val fun override namespace using"),
                   words("boolean byte char double float int long short void String Object")};
  static Lang cmake{"#", nullptr, nullptr, false, false, false, false,
                    words("if elseif else endif foreach endforeach while endwhile function endfunction macro "
                          "endmacro return set unset option project add_executable add_library add_subdirectory "
                          "target_link_libraries target_include_directories target_compile_definitions install "
                          "find_package include message cmake_minimum_required list string file configure_file"),
                    words("PRIVATE PUBLIC INTERFACE REQUIRED STATUS FATAL_ERROR WARNING ON OFF TRUE FALSE")};
  static Lang css{nullptr, "/*", "*/", false, false, true, false, words(""), words("")};
  static Lang json{nullptr, nullptr, nullptr, false, false, false, false, words("true false null"), words("")};
  static Lang ini{"#", nullptr, nullptr, false, false, false, false, words("true false yes no on off"), words("")};
  static Lang none{nullptr, nullptr, nullptr, false, false, false, false, words(""), words("")};
  switch (l) {
    case C_CPP: return c;
    case PYTHON: return py;
    case SHELL: return sh;
    case JAVASCRIPT: return js;
    case RUST: return rs;
    case GO: return go;
    case JAVA: return java;
    case CMAKE: return cmake;
    case CSS: return css;
    case JSON: return json;
    case INI: return ini;
    default: return none;
  }
}

bool starts(const std::string &t, size_t i, const char *s) {
  return s && t.compare(i, strlen(s), s) == 0;
}

void highlight_markdown(const std::string &t, std::string &st) {
  size_t i = 0, n = t.size();
  bool in_code = false;
  while (i < n) {
    size_t eol = t.find('\n', i);
    if (eol == std::string::npos) eol = n;
    if (t.compare(i, 3, "```") == 0) {
      in_code = !in_code;
      for (size_t k = i; k < eol; k++) st[k] = PREPROC;
    } else if (in_code) {
      for (size_t k = i; k < eol; k++) st[k] = STRING;
    } else if (t[i] == '#') {
      for (size_t k = i; k < eol; k++) st[k] = HEADING;
    } else if (t[i] == '>') {
      for (size_t k = i; k < eol; k++) st[k] = COMMENT;
    } else {
      // inline code and emphasis markers
      bool code = false;
      for (size_t k = i; k < eol; k++) {
        if (t[k] == '`') { code = !code; st[k] = STRING; continue; }
        if (code) st[k] = STRING;
        else if (t[k] == '*' || t[k] == '_') st[k] = KEYWORD;
      }
      if (i < eol && (t[i] == '-' || t[i] == '*') && i + 1 < eol && t[i + 1] == ' ') st[i] = KEYWORD;
    }
    i = eol + 1;
  }
}

void highlight_xml(const std::string &t, std::string &st) {
  size_t i = 0, n = t.size();
  while (i < n) {
    if (starts(t, i, "<!--")) {
      size_t e = t.find("-->", i + 4);
      e = e == std::string::npos ? n : e + 3;
      for (size_t k = i; k < e; k++) st[k] = COMMENT;
      i = e;
    } else if (t[i] == '<') {
      size_t e = i + 1;
      // tag name
      st[i] = KEYWORD;
      while (e < n && t[e] != '>' && !isspace((unsigned char)t[e])) st[e++] = KEYWORD;
      while (e < n && t[e] != '>') {
        if (t[e] == '"' || t[e] == '\'') {
          char q = t[e];
          size_t s = e++;
          while (e < n && t[e] != q) e++;
          for (size_t k = s; k <= e && k < n; k++) st[k] = STRING;
          e++;
        } else if (isalpha((unsigned char)t[e])) {
          while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '-' || t[e] == ':')) st[e++] = TYPE;
        } else e++;
      }
      if (e < n) st[e++] = KEYWORD;
      i = e;
    } else if (t[i] == '&') {
      size_t e = t.find(';', i);
      if (e != std::string::npos && e - i < 10) {
        for (size_t k = i; k <= e; k++) st[k] = NUMBER;
        i = e + 1;
      } else i++;
    } else i++;
  }
}

} // namespace

void highlight(Language lang, const std::string &t, std::string &st) {
  size_t n = t.size();
  st.assign(n, PLAIN);
  if (lang == NONE) return;
  if (lang == MARKDOWN) { highlight_markdown(t, st); return; }
  if (lang == XML) { highlight_xml(t, st); return; }
  const Lang &L = lang_def(lang);
  bool line_start = true;
  size_t i = 0;
  while (i < n) {
    char c = t[i];
    if (c == '\n') { line_start = true; i++; continue; }
    // INI: [section] lines and keys
    if (lang == INI && line_start) {
      size_t j = i;
      while (j < n && (t[j] == ' ' || t[j] == '\t')) j++;
      if (j < n && t[j] == '[') {
        size_t e = t.find('\n', j);
        if (e == std::string::npos) e = n;
        for (size_t k = j; k < e; k++) st[k] = HEADING;
        i = e;
        continue;
      }
      if (j < n && (t[j] == '#' || t[j] == ';')) {
        size_t e = t.find('\n', j);
        if (e == std::string::npos) e = n;
        for (size_t k = j; k < e; k++) st[k] = COMMENT;
        i = e;
        continue;
      }
      size_t eq = t.find('=', j), eol = t.find('\n', j);
      if (eol == std::string::npos) eol = n;
      if (eq != std::string::npos && eq < eol) {
        for (size_t k = j; k < eq; k++) st[k] = TYPE;
        i = eq + 1;
        line_start = false;
        continue;
      }
    }
    // preprocessor
    if (L.preproc && line_start) {
      size_t j = i;
      while (j < n && (t[j] == ' ' || t[j] == '\t')) j++;
      if (j < n && t[j] == '#') {
        size_t e = j;
        // continue over backslash-newlines
        while (e < n && t[e] != '\n') {
          if (t[e] == '\\' && e + 1 < n && t[e + 1] == '\n') e++;
          if (starts(t, e, "//") || starts(t, e, "/*")) break;
          e++;
        }
        for (size_t k = j; k < e; k++) st[k] = PREPROC;
        // strings inside #include "x" stay preproc colored
        i = e;
        line_start = false;
        continue;
      }
    }
    if (c != ' ' && c != '\t') line_start = false;
    // comments
    if (L.line_comment && starts(t, i, L.line_comment) &&
        !(lang == SHELL && i > 0 && t[i - 1] == '$')) { // $# in shell
      size_t e = t.find('\n', i);
      if (e == std::string::npos) e = n;
      for (size_t k = i; k < e; k++) st[k] = COMMENT;
      i = e;
      continue;
    }
    if (L.block_open && starts(t, i, L.block_open)) {
      size_t e = t.find(L.block_close, i + strlen(L.block_open));
      e = e == std::string::npos ? n : e + strlen(L.block_close);
      for (size_t k = i; k < e; k++) st[k] = COMMENT;
      i = e;
      continue;
    }
    // strings
    if (L.triple_quotes && (starts(t, i, "\"\"\"") || starts(t, i, "'''"))) {
      std::string q = t.substr(i, 3);
      size_t e = t.find(q, i + 3);
      e = e == std::string::npos ? n : e + 3;
      for (size_t k = i; k < e; k++) st[k] = STRING;
      i = e;
      continue;
    }
    if (c == '"' || (c == '\'' && (L.single_quote_strings || lang == C_CPP || lang == RUST || lang == GO)) ||
        (c == '`' && L.backtick_strings)) {
      // rust lifetimes: 'a without closing quote nearby
      if (c == '\'' && lang == RUST && !(i + 2 < n && (t[i + 2] == '\'' || (t[i + 1] == '\\')))) {
        i++;
        continue;
      }
      size_t e = i + 1;
      while (e < n && t[e] != c) {
        if (t[e] == '\\' && c != '`') e++;
        else if (t[e] == '\n' && c != '`' && !(lang == SHELL)) break;
        e++;
      }
      if (e < n && t[e] == c) e++;
      for (size_t k = i; k < e && k < n; k++) st[k] = STRING;
      i = e;
      continue;
    }
    // numbers
    if (isdigit((unsigned char)c) && (i == 0 || !(isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_'))) {
      size_t e = i;
      while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '.' || t[e] == '_' ||
                       ((t[e] == '+' || t[e] == '-') && (t[e - 1] == 'e' || t[e - 1] == 'E') &&
                        !(t[i] == '0' && i + 1 < n && (t[i + 1] == 'x' || t[i + 1] == 'X')))))
        e++;
      for (size_t k = i; k < e; k++) st[k] = NUMBER;
      i = e;
      continue;
    }
    // shell variables
    if (lang == SHELL && c == '$') {
      size_t e = i + 1;
      if (e < n && t[e] == '{') {
        while (e < n && t[e] != '}' && t[e] != '\n') e++;
        if (e < n && t[e] == '}') e++;
      } else {
        while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '_' || strchr("@#?*!$-", t[e]))) {
          e++;
          if (!isalnum((unsigned char)t[e - 1]) && t[e - 1] != '_') break;
        }
      }
      for (size_t k = i; k < e; k++) st[k] = TYPE;
      i = e;
      continue;
    }
    // identifiers
    if (isalpha((unsigned char)c) || c == '_') {
      size_t e = i;
      while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '_')) e++;
      std::string w = t.substr(i, e - i);
      char s = PLAIN;
      std::string lw = lang == CMAKE ? lower(w) : w;
      if (L.keywords.count(w) || (lang == CMAKE && L.keywords.count(lw))) s = KEYWORD;
      else if (L.types.count(w)) s = TYPE;
      else {
        size_t k = e;
        while (k < n && t[k] == ' ') k++;
        if (k < n && t[k] == '(' && lang != INI && lang != JSON) s = FUNCTION;
      }
      if (lang == JSON) s = PLAIN;
      if (s != PLAIN) for (size_t k = i; k < e; k++) st[k] = s;
      i = e;
      continue;
    }
    i++;
  }
  if (lang == JSON) {
    // keys: strings followed by ':'
    for (size_t k = 0; k < n; k++) {
      if (st[k] == STRING && (k + 1 >= n || st[k + 1] != STRING)) {
        size_t j = k + 1;
        while (j < n && isspace((unsigned char)t[j])) j++;
        if (j < n && t[j] == ':') {
          size_t s = k;
          while (s > 0 && st[s - 1] == STRING) s--;
          for (size_t q = s; q <= k; q++) st[q] = TYPE;
        }
      }
    }
  }
}

} // namespace syntax
