#include "syntax.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <cstring>
#include <set>
#include <strings.h>

namespace syntax {

static std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

static bool any_of(const std::string &v, std::initializer_list<const char *> l) {
  for (auto *x : l)
    if (v == x) return true;
  return false;
}

Language detect(const std::string &filename, const std::string &first_line) {
  std::string base = filename.substr(filename.rfind('/') == std::string::npos ? 0 : filename.rfind('/') + 1);
  std::string lb = lower(base);
  size_t dot = lb.rfind('.');
  std::string ext = dot == std::string::npos ? "" : lb.substr(dot + 1);
  if (any_of(ext, {"c", "h", "cc", "cpp", "cxx", "hpp", "hh", "hxx", "ino", "m", "mm"})) return C_CPP;
  if (any_of(ext, {"py", "pyw", "pyi"}) || lb == "sconstruct") return PYTHON;
  if (any_of(ext, {"sh", "bash", "zsh", "ksh"}) ||
      any_of(lb, {".bashrc", ".profile", "pkgbuild", ".zshrc", ".bash_profile", ".bash_aliases", "configure"}))
    return SHELL;
  if (any_of(ext, {"js", "mjs", "cjs", "jsx"})) return JAVASCRIPT;
  if (any_of(ext, {"ts", "tsx", "mts", "cts"})) return TYPESCRIPT;
  if (ext == "rs") return RUST;
  if (ext == "go") return GO;
  if (ext == "java") return JAVA;
  if (any_of(ext, {"kt", "kts"})) return KOTLIN;
  if (ext == "cs") return CSHARP;
  if (any_of(ext, {"php", "phtml", "php3", "php4", "php5"})) return PHP;
  if (any_of(ext, {"rb", "rake", "gemspec"}) || any_of(lb, {"gemfile", "rakefile", "vagrantfile"})) return RUBY;
  if (ext == "lua") return LUA;
  if (any_of(ext, {"pl", "pm", "t"})) return PERL;
  if (ext == "sql") return SQL;
  if (any_of(ext, {"yaml", "yml"}) || lb == ".clang-format") return YAML;
  if (ext == "toml" || lb == "cargo.lock") return TOML;
  if (any_of(ext, {"ini", "conf", "desktop", "cfg", "service", "socket", "timer", "target", "mount", "theme",
                   "portal", "properties"}) ||
      lb == ".gitconfig" || lb == ".editorconfig")
    return INI;
  if (any_of(ext, {"md", "markdown"})) return MARKDOWN;
  if (lb == "cmakelists.txt" || ext == "cmake") return CMAKE;
  if (any_of(lb, {"meson.build", "meson_options.txt", "meson.options"})) return MESON;
  if (any_of(ext, {"json", "jsonc", "geojson"}) || lb == ".clangd") return JSON;
  if (any_of(ext, {"html", "htm", "xhtml", "vue", "svelte"})) return HTML;
  if (any_of(ext, {"xml", "svg", "ui", "xsl", "xslt", "plist", "metainfo", "policy", "glade"})) return XML;
  if (any_of(ext, {"css", "scss", "less"})) return CSS;
  if (any_of(lb, {"makefile", "gnumakefile", "kbuild"}) || ext == "mk" || ext == "mak") return MAKEFILE;
  if (any_of(ext, {"diff", "patch", "rej"})) return DIFF;
  if (first_line.rfind("#!", 0) == 0) {
    if (first_line.find("python") != std::string::npos) return PYTHON;
    if (first_line.find("node") != std::string::npos) return JAVASCRIPT;
    if (first_line.find("ruby") != std::string::npos) return RUBY;
    if (first_line.find("perl") != std::string::npos) return PERL;
    if (first_line.find("lua") != std::string::npos) return LUA;
    if (first_line.find("php") != std::string::npos) return PHP;
    if (first_line.find("make") != std::string::npos) return MAKEFILE;
    if (first_line.find("sh") != std::string::npos) return SHELL;
  }
  if (first_line.rfind("<?php", 0) == 0) return PHP;
  if (first_line.rfind("<?xml", 0) == 0) return XML;
  if (lower(first_line).rfind("<!doctype html", 0) == 0) return HTML;
  if (first_line.rfind("diff --git ", 0) == 0 || first_line.rfind("--- ", 0) == 0) return DIFF;
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
    case XML: return "XML";
    case CSS: return "CSS";
    case TYPESCRIPT: return "TypeScript";
    case PHP: return "PHP";
    case RUBY: return "Ruby";
    case LUA: return "Lua";
    case PERL: return "Perl";
    case SQL: return "SQL";
    case YAML: return "YAML";
    case TOML: return "TOML";
    case MAKEFILE: return "Makefile";
    case DIFF: return "Diff";
    case HTML: return "HTML";
    case KOTLIN: return "Kotlin";
    case CSHARP: return "C#";
    case MESON: return "Meson";
    default: return "Plain Text";
  }
}

std::vector<Language> all_languages() {
  std::vector<Language> v;
  for (int i = 1; i < LANGUAGE_COUNT; i++) v.push_back((Language)i);
  std::sort(v.begin(), v.end(), [](Language a, Language b) { return lower(language_name(a)) < lower(language_name(b)); });
  return v;
}

namespace {

struct Lang {
  const char *line_comment = nullptr;  // e.g. "//" or "#"
  const char *line_comment2 = nullptr; // a second one (PHP "#", SQL "#")
  const char *block_open = nullptr;    // e.g. "/*"
  const char *block_close = nullptr;
  bool preproc = false;                // '#' lines are preprocessor
  bool triple_quotes = false;          // """ and '''
  bool single_quote_strings = false;
  bool backtick_strings = false;
  bool nocase = false;                 // case-insensitive keywords (SQL, CMake)
  const char *sigils = nullptr;        // variable prefixes, e.g. "$" or "$@%"
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

Lang make(const char *lc, const char *bo, const char *bc, const char *kw, const char *ty) {
  Lang l;
  l.line_comment = lc;
  l.block_open = bo;
  l.block_close = bc;
  l.keywords = words(kw);
  l.types = words(ty);
  return l;
}

const char *kJsKeywords =
    "var let const function return if else for while do switch case default break continue new "
    "delete typeof instanceof in of class extends super this import export from as async await "
    "try catch finally throw yield null undefined true false static get set void";
const char *kTsExtra = " interface type enum implements namespace declare abstract readonly private protected public "
                       "keyof infer is satisfies";

const Lang &lang_def(Language l) {
  static const Lang none;
  static Lang c, py, sh, js, ts, rs, go, java, kt, cs, cmake, css, json, ini, toml, php, rb, lua, perl, sql, make_,
      meson;
  static bool init = false;
  if (!init) {
    init = true;
    c = make("//", "/*", "*/",
             "if else for while do switch case default break continue return goto sizeof typedef struct "
             "union enum class public private protected virtual override final static const constexpr "
             "extern inline volatile register template typename namespace using new delete this "
             "operator friend try catch throw noexcept nullptr true false auto decltype explicit mutable "
             "static_cast dynamic_cast reinterpret_cast const_cast co_await co_return co_yield consteval "
             "constinit concept requires alignas alignof static_assert thread_local NULL",
             "void bool char short int long float double signed unsigned size_t ssize_t uint8_t uint16_t "
             "uint32_t uint64_t int8_t int16_t int32_t int64_t wchar_t char8_t char16_t char32_t std string "
             "vector map FILE");
    c.preproc = c.single_quote_strings = true;
    py = make("#", nullptr, nullptr,
              "and as assert async await break class continue def del elif else except finally for from "
              "global if import in is lambda nonlocal not or pass raise return try while with yield True "
              "False None self match case",
              "int float str bool list dict set tuple bytes object type print len range open");
    py.triple_quotes = py.single_quote_strings = true;
    sh = make("#", nullptr, nullptr,
              "if then else elif fi for while until do done case esac in function return exit local "
              "export readonly set unset shift break continue source alias declare select",
              "echo printf cd test read eval exec trap true false");
    sh.single_quote_strings = sh.backtick_strings = true;
    js = make("//", "/*", "*/", kJsKeywords, "Object Array Promise Map Set JSON Math console window document");
    js.single_quote_strings = js.backtick_strings = true;
    ts = make("//", "/*", "*/", (std::string(kJsKeywords) + kTsExtra).c_str(),
              "string number boolean any void never unknown object bigint symbol Object Array Promise Map Set "
              "Record Partial Readonly");
    ts.single_quote_strings = ts.backtick_strings = true;
    rs = make("//", "/*", "*/",
              "as async await break const continue crate dyn else enum extern false fn for if impl in let "
              "loop match mod move mut pub ref return self Self static struct super trait true type unsafe "
              "use where while",
              "i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec "
              "Option Result Box Some None Ok Err");
    go = make("//", "/*", "*/",
              "break case chan const continue default defer else fallthrough for func go goto if import "
              "interface map package range return select struct switch type var nil true false",
              "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string "
              "uint uint8 uint16 uint32 uint64 uintptr any");
    go.backtick_strings = true;
    java = make("//", "/*", "*/",
                "abstract assert break case catch class const continue default do else enum extends final "
                "finally for goto if implements import instanceof interface native new package private "
                "protected public return static strictfp super switch synchronized this throw throws "
                "transient try volatile while true false null var record sealed permits yield",
                "boolean byte char double float int long short void String Object Integer List Map");
    java.single_quote_strings = true;
    kt = make("//", "/*", "*/",
              "as break class continue do else false for fun if in interface is null object package return "
              "super this throw true try typealias typeof val var when while by catch constructor "
              "delegate dynamic field file finally get import init param property receiver set setparam "
              "where abstract annotation companion const crossinline data enum expect external final infix "
              "inline inner internal lateinit noinline open operator out override private protected public "
              "reified sealed suspend tailrec vararg",
              "Any Unit Nothing Int Long Short Byte Double Float Boolean Char String Array List Map Set "
              "MutableList MutableMap");
    kt.single_quote_strings = kt.triple_quotes = true;
    cs = make("//", "/*", "*/",
              "abstract as base break case catch checked class const continue default delegate do else enum "
              "event explicit extern false finally fixed for foreach goto if implicit in interface internal "
              "is lock namespace new null operator out override params private protected public readonly "
              "ref return sealed sizeof stackalloc static struct switch this throw true try typeof unchecked "
              "unsafe using virtual volatile while async await var get set value yield record init where "
              "partial dynamic nameof",
              "bool byte char decimal double float int long object sbyte short string uint ulong ushort void "
              "String Object List Dictionary Task");
    cs.preproc = cs.single_quote_strings = true;
    cmake = make("#", nullptr, nullptr,
                 "if elseif else endif foreach endforeach while endwhile function endfunction macro "
                 "endmacro return set unset option project add_executable add_library add_subdirectory "
                 "target_link_libraries target_include_directories target_compile_definitions "
                 "target_compile_options target_sources install find_package find_program find_library "
                 "include message cmake_minimum_required list string file configure_file add_test "
                 "add_custom_command add_custom_target pkg_check_modules set_target_properties break continue",
                 "PRIVATE PUBLIC INTERFACE REQUIRED STATUS FATAL_ERROR WARNING ON OFF TRUE FALSE");
    cmake.nocase = true;
    css = make(nullptr, "/*", "*/", "important inherit initial unset none auto", "");
    css.single_quote_strings = true;
    json = make(nullptr, nullptr, nullptr, "true false null", "");
    ini = make("#", nullptr, nullptr, "true false yes no on off", "");
    toml = make("#", nullptr, nullptr, "true false inf nan", "");
    toml.single_quote_strings = toml.triple_quotes = true;
    php = make("//", "/*", "*/",
               "abstract and array as break callable case catch class clone const continue declare default "
               "do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum extends "
               "final finally fn for foreach function global goto if implements include include_once "
               "instanceof insteadof interface isset list match namespace new or print private protected "
               "public readonly require require_once return static switch throw trait try unset use var "
               "while xor yield true false null TRUE FALSE NULL self parent",
               "int float bool string void mixed never object iterable");
    php.line_comment2 = "#";
    php.single_quote_strings = true;
    php.sigils = "$";
    rb = make("#", "=begin", "=end",
              "BEGIN END alias and begin break case class def defined? do else elsif end ensure false for if "
              "in module next nil not or redo rescue retry return self super then true undef unless until "
              "when while yield require require_relative include extend attr_accessor attr_reader "
              "attr_writer private protected public raise lambda proc puts",
              "Array Hash String Integer Float Symbol Object Kernel");
    rb.single_quote_strings = rb.backtick_strings = true;
    rb.sigils = "@$";
    lua = make("--", "--[[", "]]",
               "and break do else elseif end false for function goto if in local nil not or repeat return "
               "then true until while self",
               "print pairs ipairs require type tostring tonumber setmetatable getmetatable string table "
               "math io os coroutine error pcall assert");
    lua.single_quote_strings = true;
    perl = make("#", "\n=pod", "\n=cut",
                "my our local sub if elsif else unless while until for foreach do last next redo return "
                "package use no require BEGIN END eq ne lt gt le ge cmp and or not xor qw undef defined "
                "print printf die warn shift push pop chomp split join keys values exists delete open close",
                "");
    perl.single_quote_strings = perl.backtick_strings = true;
    perl.sigils = "$@%";
    sql = make("--", "/*", "*/",
               "select from where and or not insert into values update set delete create alter drop table "
               "index view trigger procedure function begin end commit rollback transaction join inner left "
               "right outer full cross on as group by order having limit offset union all distinct case when "
               "then else is null like in between exists primary key foreign references default unique "
               "check constraint if replace returning with recursive asc desc database schema grant revoke "
               "true false",
               "int integer bigint smallint tinyint serial bigserial real double float decimal numeric "
               "char varchar text blob boolean bool date time timestamp timestamptz interval json jsonb uuid "
               "bytea");
    sql.line_comment2 = "#";
    sql.single_quote_strings = sql.nocase = true;
    make_ = make("#", nullptr, nullptr,
                 "ifeq ifneq ifdef ifndef else endif include -include sinclude define endef export unexport "
                 "override vpath",
                 ".PHONY .SUFFIXES .DEFAULT .PRECIOUS .INTERMEDIATE .SECONDARY .DELETE_ON_ERROR");
    make_.single_quote_strings = true;
    meson = make("#", nullptr, nullptr,
                 "if elif else endif foreach endforeach and or not in true false continue break",
                 "project executable library shared_library static_library dependency declare_dependency "
                 "subdir install_data install_headers configure_file custom_target run_command "
                 "find_program import files include_directories test get_option message error warning "
                 "summary meson");
    meson.single_quote_strings = meson.triple_quotes = true;
  }
  switch (l) {
    case C_CPP: return c;
    case PYTHON: return py;
    case SHELL: return sh;
    case JAVASCRIPT: return js;
    case TYPESCRIPT: return ts;
    case RUST: return rs;
    case GO: return go;
    case JAVA: return java;
    case KOTLIN: return kt;
    case CSHARP: return cs;
    case CMAKE: return cmake;
    case CSS: return css;
    case JSON: return json;
    case INI: return ini;
    case TOML: return toml;
    case PHP: return php;
    case RUBY: return rb;
    case LUA: return lua;
    case PERL: return perl;
    case SQL: return sql;
    case MAKEFILE: return make_;
    case MESON: return meson;
    default: return none;
  }
}

bool starts(const std::string &t, size_t i, const char *s) {
  return s && t.compare(i, strlen(s), s) == 0;
}

std::string trim_ws(const std::string &s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

size_t line_end(const std::string &t, size_t i) {
  size_t e = t.find('\n', i);
  return e == std::string::npos ? t.size() : e;
}

void fill(std::string &st, size_t a, size_t b, char s) {
  for (size_t k = a; k < b && k < st.size(); k++) st[k] = s;
}

void highlight_markdown(const std::string &t, std::string &st) {
  size_t i = 0, n = t.size();
  bool in_code = false;
  while (i < n) {
    size_t eol = line_end(t, i);
    if (t.compare(i, 3, "```") == 0) {
      in_code = !in_code;
      fill(st, i, eol, PREPROC);
    } else if (in_code) {
      fill(st, i, eol, STRING);
    } else if (t[i] == '#') {
      fill(st, i, eol, HEADING);
    } else if (t[i] == '>') {
      fill(st, i, eol, COMMENT);
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

void highlight_diff(const std::string &t, std::string &st) {
  size_t i = 0, n = t.size();
  while (i < n) {
    size_t eol = line_end(t, i);
    char s = PLAIN;
    if (starts(t, i, "+++") || starts(t, i, "---") || starts(t, i, "diff ") || starts(t, i, "index ") ||
        starts(t, i, "new file") || starts(t, i, "deleted file") || starts(t, i, "similarity") ||
        starts(t, i, "rename "))
      s = HEADING;
    else if (starts(t, i, "@@")) s = PREPROC;
    else if (t[i] == '+' || t[i] == '>') s = ADDED;
    else if (t[i] == '-' || t[i] == '<') s = REMOVED;
    else if (t[i] == '\\') s = COMMENT;
    fill(st, i, eol, s);
    i = eol + 1;
  }
}

void highlight_yaml(const std::string &t, std::string &st) {
  size_t i = 0, n = t.size();
  static const std::set<std::string> kw = words("true false yes no on off null True False Yes No Null TRUE FALSE NULL ~");
  while (i < n) {
    size_t eol = line_end(t, i);
    size_t j = i;
    while (j < eol && (t[j] == ' ' || t[j] == '\t')) j++;
    if (starts(t, i, "---") || starts(t, i, "...")) {
      fill(st, i, eol, HEADING);
      i = eol + 1;
      continue;
    }
    if (j < eol && t[j] == '#') {
      fill(st, j, eol, COMMENT);
      i = eol + 1;
      continue;
    }
    if (j < eol && t[j] == '-' && j + 1 < eol && t[j + 1] == ' ') {
      st[j] = KEYWORD;
      j += 2;
      while (j < eol && t[j] == ' ') j++;
    }
    // key:
    size_t k = j;
    if (k < eol && (t[k] == '"' || t[k] == '\'')) {
      char q = t[k];
      size_t e = t.find(q, k + 1);
      if (e != std::string::npos && e < eol) k = e + 1;
    } else {
      while (k < eol && t[k] != ':' && t[k] != '#' && t[k] != ' ') k++;
      while (k < eol && t[k] == ' ' && k + 1 < eol && t[k + 1] != ':' && t[k + 1] != '#' && t[k + 1] != ' ') {
        k++; // keys with spaces
        while (k < eol && t[k] != ':' && t[k] != '#' && t[k] != ' ') k++;
      }
    }
    size_t v = j;
    if (k < eol && t[k] == ':' && (k + 1 == eol || t[k + 1] == ' ' || t[k + 1] == '\t')) {
      fill(st, j, k, TYPE);
      v = k + 1;
    }
    // value
    while (v < eol) {
      char c = t[v];
      if (c == '#' && (v == 0 || t[v - 1] == ' ')) {
        fill(st, v, eol, COMMENT);
        break;
      }
      if (c == '"' || c == '\'') {
        size_t e = v + 1;
        while (e < eol && t[e] != c) e += (t[e] == '\\' && c == '"') ? 2 : 1;
        e = std::min(e + 1, eol);
        fill(st, v, e, STRING);
        v = e;
        continue;
      }
      if (c == '&' || c == '*' || c == '!') {
        size_t e = v + 1;
        while (e < eol && !isspace((unsigned char)t[e])) e++;
        fill(st, v, e, PREPROC);
        v = e;
        continue;
      }
      if (c == '|' || c == '>') { st[v++] = KEYWORD; continue; }
      if (!isspace((unsigned char)c)) {
        size_t e = v;
        while (e < eol && !(t[e] == ' ' && e + 1 < eol && t[e + 1] == '#')) e++;
        std::string word = t.substr(v, e - v);
        while (!word.empty() && isspace((unsigned char)word.back())) word.pop_back();
        bool num = !word.empty() && (isdigit((unsigned char)word[0]) || ((word[0] == '-' || word[0] == '.') &&
                                     word.size() > 1 && isdigit((unsigned char)word[1])));
        if (num) {
          for (char ch : word)
            if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '+' || ch == '_' || ch == ':')) num = false;
        }
        if (kw.count(word)) fill(st, v, v + word.size(), KEYWORD);
        else if (num) fill(st, v, v + word.size(), NUMBER);
        v = e;
        continue;
      }
      v++;
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
      fill(st, i, e, COMMENT);
      i = e;
    } else if (starts(t, i, "<![CDATA[")) {
      size_t e = t.find("]]>", i);
      e = e == std::string::npos ? n : e + 3;
      fill(st, i, e, STRING);
      i = e;
    } else if (t[i] == '<' && i + 1 < n && (isalpha((unsigned char)t[i + 1]) || strchr("/!?", t[i + 1]))) {
      size_t e = i + 1;
      // tag name
      st[i] = KEYWORD;
      while (e < n && t[e] != '>' && !isspace((unsigned char)t[e])) st[e++] = KEYWORD;
      while (e < n && t[e] != '>') {
        if (t[e] == '"' || t[e] == '\'') {
          char q = t[e];
          size_t s = e++;
          while (e < n && t[e] != q) e++;
          fill(st, s, e + 1, STRING);
          e++;
        } else if (isalpha((unsigned char)t[e])) {
          while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '-' || t[e] == ':' || t[e] == '_')) st[e++] = TYPE;
        } else e++;
      }
      if (e < n) st[e++] = KEYWORD;
      i = e;
    } else if (t[i] == '&') {
      size_t e = t.find(';', i);
      if (e != std::string::npos && e - i < 10) {
        fill(st, i, e + 1, NUMBER);
        i = e + 1;
      } else i++;
    } else i++;
  }
}

void highlight_code(Language lang, const std::string &t, std::string &st);

// Highlights t[a, b) as `lang`, writing into st.
void highlight_range(Language lang, const std::string &t, size_t a, size_t b, std::string &st) {
  if (b <= a) return;
  std::string sub = t.substr(a, b - a), sst;
  highlight(lang, sub, sst);
  std::copy(sst.begin(), sst.end(), st.begin() + (long)a);
}

size_t find_ci(const std::string &t, const char *needle, size_t from) {
  size_t nl = strlen(needle);
  for (size_t i = from; i + nl <= t.size(); i++)
    if (strncasecmp(t.c_str() + i, needle, nl) == 0) return i;
  return std::string::npos;
}

// HTML: XML plus embedded <script> and <style> (and PHP's <?php ?> blocks)
void highlight_html(const std::string &t, std::string &st, bool php) {
  highlight_xml(t, st);
  size_t n = t.size();
  // <script ...> ... </script>, <style ...> ... </style>
  struct Emb { const char *open, *close; Language lang; };
  static const Emb embs[] = {{"<script", "</script", JAVASCRIPT}, {"<style", "</style", CSS}};
  for (auto &em : embs) {
    size_t i = 0;
    while ((i = find_ci(t, em.open, i)) != std::string::npos) {
      size_t gt = t.find('>', i);
      if (gt == std::string::npos) break;
      size_t end = find_ci(t, em.close, gt);
      if (end == std::string::npos) end = n;
      highlight_range(em.lang, t, gt + 1, end, st);
      i = end;
    }
  }
  if (php) {
    size_t i = 0;
    while ((i = t.find("<?", i)) != std::string::npos) {
      size_t code = i + 2;
      if (t.compare(code, 3, "php") == 0) code += 3;
      else if (t.compare(code, 1, "=") == 0) code += 1;
      size_t end = t.find("?>", code);
      if (end == std::string::npos) end = n;
      fill(st, i, code, PREPROC);
      std::string sub = t.substr(code, end - code), sst;
      highlight_code(PHP, sub, sst);
      std::copy(sst.begin(), sst.end(), st.begin() + (long)code);
      if (end < n) fill(st, end, end + 2, PREPROC);
      i = end;
    }
  }
}

void highlight_code(Language lang, const std::string &t, std::string &st) {
  size_t n = t.size();
  st.assign(n, PLAIN);
  const Lang &L = lang_def(lang);
  bool line_start = true;
  bool ini_like = lang == INI || lang == TOML;
  size_t i = 0;
  while (i < n) {
    char c = t[i];
    if (c == '\n') { line_start = true; i++; continue; }
    // INI/TOML: [section] lines and keys
    if (ini_like && line_start) {
      size_t j = i;
      while (j < n && (t[j] == ' ' || t[j] == '\t')) j++;
      if (j < n && t[j] == '[') {
        size_t e = line_end(t, j);
        // a trailing comment after a TOML table header
        size_t hash = t.find('#', j);
        size_t he = hash != std::string::npos && hash < e && lang == TOML ? hash : e;
        fill(st, j, he, HEADING);
        fill(st, he, e, COMMENT);
        i = e;
        continue;
      }
      if (j < n && (t[j] == '#' || t[j] == ';')) {
        size_t e = line_end(t, j);
        fill(st, j, e, COMMENT);
        i = e;
        continue;
      }
      size_t eq = t.find('=', j), eol = line_end(t, j);
      if (eq != std::string::npos && eq < eol) {
        fill(st, j, eq, TYPE);
        i = eq + 1;
        line_start = false;
        if (lang == INI) {
          // INI values are plain text, except well-known keywords
          size_t vs = i;
          while (vs < eol && t[vs] == ' ') vs++;
          std::string v = t.substr(vs, eol - vs);
          while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
          if (L.keywords.count(lower(v))) fill(st, vs, vs + v.size(), KEYWORD);
          else if (!v.empty() && v.find_first_not_of("0123456789.-") == std::string::npos) fill(st, vs, vs + v.size(), NUMBER);
          i = eol;
        }
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
        fill(st, j, e, PREPROC);
        i = e;
        line_start = false;
        continue;
      }
    }
    // Makefile: "target:" at the start of a line
    if (lang == MAKEFILE && line_start && c != '\t' && c != ' ' && c != '#') {
      size_t e = i;
      while (e < n && t[e] != '\n' && t[e] != ':' && t[e] != '=' && t[e] != '#' && t[e] != '$') e++;
      if (e < n && t[e] == ':' && !(e + 1 < n && t[e + 1] == '=') && !(e + 1 < n && t[e + 1] == ':' && e + 2 < n && t[e + 2] == '=')) {
        std::string target = t.substr(i, e - i);
        fill(st, i, e, L.types.count(trim_ws(target)) ? TYPE : FUNCTION);
        i = e + 1;
        line_start = false;
        continue;
      }
    }
    bool was_line_start = line_start;
    if (c != ' ' && c != '\t') line_start = false;
    // block comments (before line comments: Lua's "--[[" starts with "--")
    if (L.block_open && starts(t, i, L.block_open) &&
        !(L.block_open[0] == '=' && !was_line_start) && !(L.block_open[0] == '\n')) {
      size_t e = t.find(L.block_close, i + strlen(L.block_open));
      e = e == std::string::npos ? n : e + strlen(L.block_close);
      if (lang == RUBY) e = line_end(t, e);
      fill(st, i, e, COMMENT);
      i = e;
      continue;
    }
    // Perl POD: =pod/=head... at the start of a line up to =cut
    if (lang == PERL && was_line_start && c == '=' && i + 1 < n && isalpha((unsigned char)t[i + 1])) {
      size_t e = t.find("\n=cut", i);
      e = e == std::string::npos ? n : line_end(t, e + 1);
      fill(st, i, e, COMMENT);
      i = e;
      continue;
    }
    // line comments
    if ((L.line_comment && starts(t, i, L.line_comment)) || (L.line_comment2 && starts(t, i, L.line_comment2))) {
      bool skip = (lang == SHELL || lang == PERL || lang == MAKEFILE) && i > 0 && t[i - 1] == '$'; // $#
      if (lang == SHELL && i > 0 && !isspace((unsigned char)t[i - 1]) && t[i - 1] != ';' && t[i - 1] != '(')
        skip = true; // a#b is a word in shell
      if (!skip) {
        size_t e = line_end(t, i);
        fill(st, i, e, COMMENT);
        i = e;
        continue;
      }
    }
    // strings
    if (L.triple_quotes && (starts(t, i, "\"\"\"") || starts(t, i, "'''"))) {
      std::string q = t.substr(i, 3);
      size_t e = t.find(q, i + 3);
      e = e == std::string::npos ? n : e + 3;
      fill(st, i, e, STRING);
      i = e;
      continue;
    }
    // Lua long strings [[ ... ]]
    if (lang == LUA && starts(t, i, "[[")) {
      size_t e = t.find("]]", i + 2);
      e = e == std::string::npos ? n : e + 2;
      fill(st, i, e, STRING);
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
      // apostrophes inside words in shell/makefiles (don't) are not strings
      if (c == '\'' && lang == MAKEFILE && i > 0 && isalpha((unsigned char)t[i - 1])) {
        i++;
        continue;
      }
      size_t e = i + 1;
      bool sql_quote = lang == SQL && c == '\'';
      while (e < n && t[e] != c) {
        if (t[e] == '\\' && c != '`' && !sql_quote && !(lang == SHELL && c == '\'')) e++;
        else if (t[e] == '\n' && c != '`' && lang != SHELL && lang != PHP && lang != PERL && lang != RUBY &&
                 lang != SQL)
          break;
        e++;
        if (sql_quote && e + 1 < n && t[e] == '\'' && t[e + 1] == '\'') e += 2; // '' escape
      }
      if (e < n && t[e] == c) e++;
      fill(st, i, e, STRING);
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
      fill(st, i, e, NUMBER);
      i = e;
      continue;
    }
    // shell / makefile variables
    if ((lang == SHELL || lang == MAKEFILE) && c == '$') {
      size_t e = i + 1;
      char open = e < n ? t[e] : 0;
      if (open == '{' || (open == '(' && lang == MAKEFILE)) {
        char close = open == '{' ? '}' : ')';
        int depth = 0;
        while (e < n && t[e] != '\n') {
          if (t[e] == open) depth++;
          else if (t[e] == close && --depth == 0) break;
          e++;
        }
        if (e < n && t[e] == close) e++;
      } else if (open == '(' && lang == SHELL) {
        e = i + 1; // $( command substitution: color just the $
      } else {
        while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '_' || strchr("@#?*!$-<^%", t[e]))) {
          e++;
          if (!isalnum((unsigned char)t[e - 1]) && t[e - 1] != '_') break;
        }
      }
      fill(st, i, e, TYPE);
      i = e;
      continue;
    }
    // sigil variables: $x (PHP, Perl), @x (Perl, Ruby), %x (Perl)
    if (L.sigils && strchr(L.sigils, c) && c && i + 1 < n &&
        (isalpha((unsigned char)t[i + 1]) || t[i + 1] == '_' || (c == '@' && t[i + 1] == '@') ||
         (lang == PERL && c == '$' && strchr("_0!@/\\&", t[i + 1])))) {
      size_t e = i + 1;
      if (t[e] == '@') e++;
      if (lang == PERL && !isalnum((unsigned char)t[e]) && t[e] != '_') e++;
      else
        while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '_' || (lang == PERL && t[e] == ':'))) e++;
      fill(st, i, e, TYPE);
      i = e;
      continue;
    }
    // Ruby symbols
    if (lang == RUBY && c == ':' && i + 1 < n && (isalpha((unsigned char)t[i + 1]) || t[i + 1] == '_') &&
        !(i > 0 && t[i - 1] == ':')) {
      size_t e = i + 1;
      while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '_' || t[e] == '?' || t[e] == '!')) e++;
      fill(st, i, e, NUMBER);
      i = e;
      continue;
    }
    // CSS: #hex colors and @rules
    if (lang == CSS && (c == '@' || (c == '#' && i + 1 < n && isxdigit((unsigned char)t[i + 1])))) {
      size_t e = i + 1;
      while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '-')) e++;
      fill(st, i, e, c == '@' ? KEYWORD : NUMBER);
      i = e;
      continue;
    }
    // decorators / annotations
    if (c == '@' && (lang == PYTHON || lang == JAVA || lang == KOTLIN || lang == TYPESCRIPT) && i + 1 < n &&
        isalpha((unsigned char)t[i + 1])) {
      size_t e = i + 1;
      while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '_' || t[e] == '.')) e++;
      fill(st, i, e, PREPROC);
      i = e;
      continue;
    }
    // identifiers
    if (isalpha((unsigned char)c) || c == '_' || (lang == CSS && c == '-')) {
      size_t e = i;
      while (e < n && (isalnum((unsigned char)t[e]) || t[e] == '_' || (lang == CSS && t[e] == '-') ||
                       (lang == RUBY && (t[e] == '?' || t[e] == '!') && e > i)))
        e++;
      std::string w = t.substr(i, e - i);
      char s = PLAIN;
      std::string lw = L.nocase ? lower(w) : w;
      if (L.keywords.count(w) || (L.nocase && L.keywords.count(lw))) s = KEYWORD;
      else if (L.types.count(w) || (L.nocase && L.types.count(lw))) s = TYPE;
      else if (lang == CSS) {
        // property names: identifier followed by ':' inside a block
        size_t k = e;
        while (k < n && t[k] == ' ') k++;
        if (k < n && t[k] == ':') s = TYPE;
      } else {
        size_t k = e;
        while (k < n && t[k] == ' ') k++;
        if (k < n && t[k] == '(' && lang != JSON) s = FUNCTION;
      }
      if (lang == JSON) s = PLAIN;
      if (s != PLAIN) fill(st, i, e, s);
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
          fill(st, s, k + 1, TYPE);
        }
      }
    }
  }
}

} // namespace

void highlight(Language lang, const std::string &t, std::string &st) {
  st.assign(t.size(), PLAIN);
  switch (lang) {
    case NONE: return;
    case MARKDOWN: highlight_markdown(t, st); return;
    case XML: highlight_xml(t, st); return;
    case HTML: highlight_html(t, st, false); return;
    case DIFF: highlight_diff(t, st); return;
    case YAML: highlight_yaml(t, st); return;
    case PHP:
      // plain PHP files start with <?php; anything else is HTML with PHP blocks
      if (t.find("<?") == std::string::npos) { highlight_code(PHP, t, st); return; }
      highlight_html(t, st, true);
      return;
    default: highlight_code(lang, t, st); return;
  }
}

long match_bracket(const std::string &t, const std::string &st, size_t pos) {
  if (pos >= t.size()) return -1;
  static const char *open = "([{", *close = ")]}";
  char c = t[pos];
  const char *o = strchr(open, c), *cl = strchr(close, c);
  if (!c || (!o && !cl)) return -1;
  auto code = [&](size_t k) {
    if (st.size() != t.size()) return true;
    char s = st[k];
    return s != COMMENT && s != STRING;
  };
  if (!code(pos)) return -1;
  char want = o ? close[o - open] : open[cl - close];
  int depth = 0;
  if (o) {
    for (size_t k = pos; k < t.size(); k++) {
      if (!code(k)) continue;
      if (t[k] == c) depth++;
      else if (t[k] == want && --depth == 0) return (long)k;
    }
  } else {
    for (size_t k = pos + 1; k-- > 0;) {
      if (!code(k)) continue;
      if (t[k] == c) depth++;
      else if (t[k] == want && --depth == 0) return (long)k;
    }
  }
  return -1;
}

// ---- themes ----------------------------------------------------------------------------

const std::vector<Theme> &themes() {
  //                         A plain   B comment C string  D keyword E number  F preproc G type    H heading I func    J added   K removed L bracket
  static const std::vector<Theme> t = {
      {"light", "Default Light", false, 0xffffff, 0xf3f4f6, 0x8a8f98, 0xb4d2f5, 0x3584e4, 0xd7e3f4,
       {0x24262b, 0x8a8f98, 0x26a269, 0x8f3cb5, 0xc64600, 0x9c6500, 0x1a7f99, 0x1c5aa8, 0x1c5aa8, 0x26a269, 0xc01c28, 0x24262b},
       {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
      {"dark", "Default Dark", true, 0x1e1f22, 0x26272b, 0x7f848e, 0x3a4d6b, 0x61afef, 0x3b4252,
       {0xdcdfe4, 0x7f848e, 0x98c379, 0xc678dd, 0xd19a66, 0xe5c07b, 0x56b6c2, 0x61afef, 0x61afef, 0x98c379, 0xe06c75, 0xffffff},
       {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
      {"solarized-light", "Solarized Light", false, 0xfdf6e3, 0xeee8d5, 0x93a1a1, 0xeee8d5, 0x657b83, 0xe4ddc8,
       {0x657b83, 0x93a1a1, 0x2aa198, 0x859900, 0xd33682, 0xcb4b16, 0xb58900, 0x268bd2, 0x268bd2, 0x859900, 0xdc322f, 0x073642},
       {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
      {"solarized-dark", "Solarized Dark", true, 0x002b36, 0x073642, 0x586e75, 0x274642, 0x93a1a1, 0x094552,
       {0x839496, 0x586e75, 0x2aa198, 0x859900, 0xd33682, 0xcb4b16, 0xb58900, 0x268bd2, 0x268bd2, 0x859900, 0xdc322f, 0xfdf6e3},
       {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
      {"monokai", "Monokai", true, 0x272822, 0x2f302a, 0x75715e, 0x49483e, 0xf8f8f0, 0x49483e,
       {0xf8f8f2, 0x75715e, 0xe6db74, 0xf92672, 0xae81ff, 0xfd971f, 0x66d9ef, 0xa6e22e, 0xa6e22e, 0xa6e22e, 0xf92672, 0xffffff},
       {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}},
      {"high-contrast", "High Contrast", false, 0xffffff, 0xf0f0f0, 0x000000, 0xa0c4ff, 0x000000, 0xffe066,
       {0x000000, 0x006400, 0x8b0000, 0x00008b, 0x8b008b, 0x5c4400, 0x004d4d, 0x00008b, 0x000000, 0x006400, 0x8b0000, 0x000000},
       {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
  };
  return t;
}

const Theme &theme(const std::string &id, bool dark) {
  for (auto &t : themes())
    if (id == t.id) return t;
  return themes()[dark ? 1 : 0];
}

} // namespace syntax
