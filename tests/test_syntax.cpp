#include "syntax.h"
#include "test.h"

using namespace syntax;

static std::string styles(Language l, const std::string &t) {
  std::string s;
  highlight(l, t, s);
  return s;
}

int main() {
  CHECK_EQ(detect("/x/main.cpp", ""), C_CPP);
  CHECK_EQ(detect("script", "#!/usr/bin/env python3"), PYTHON);
  CHECK_EQ(detect("run", "#!/bin/sh"), SHELL);
  CHECK_EQ(detect("CMakeLists.txt", ""), CMAKE);
  CHECK_EQ(detect("notes.md", ""), MARKDOWN);
  CHECK_EQ(detect("a.txt", ""), NONE);

  std::string t = "int x = 42; // hi\n";
  std::string s = styles(C_CPP, t);
  CHECK_EQ(s.size(), t.size());
  CHECK_EQ(s.substr(0, 3), std::string(3, TYPE));
  CHECK_EQ(s[8], NUMBER);
  CHECK_EQ(s.substr(12, 5), std::string(5, COMMENT));

  t = "#include <stdio.h>\nreturn \"a\\\"b\";";
  s = styles(C_CPP, t);
  CHECK_EQ(s[0], PREPROC);
  CHECK_EQ(s[19], KEYWORD);
  CHECK_EQ(s[26], STRING);
  CHECK_EQ(s[t.size() - 2], STRING);

  t = "def f():\n    '''doc'''\n";
  s = styles(PYTHON, t);
  CHECK_EQ(s[0], KEYWORD);
  CHECK_EQ(s[4], FUNCTION);
  CHECK_EQ(s[14], STRING);

  t = "echo $HOME # c";
  s = styles(SHELL, t);
  CHECK_EQ(s[5], TYPE);
  CHECK_EQ(s[13], COMMENT);

  t = "[Section]\nkey=value\n";
  s = styles(INI, t);
  CHECK_EQ(s[0], HEADING);
  CHECK_EQ(s[10], TYPE);
  CHECK_EQ(s[14], PLAIN);

  t = "{\"a\": \"b\"}";
  s = styles(JSON, t);
  CHECK_EQ(s[1], TYPE);
  CHECK_EQ(s[6], STRING);
  // nothing for plain text
  CHECK_EQ(styles(NONE, "int x"), std::string("AAAAA"));

  // ---- detection of the newer languages
  CHECK_EQ(detect("a.ts", ""), TYPESCRIPT);
  CHECK_EQ(detect("index.php", ""), PHP);
  CHECK_EQ(detect("Gemfile", ""), RUBY);
  CHECK_EQ(detect("init.lua", ""), LUA);
  CHECK_EQ(detect("x", "#!/usr/bin/perl -w"), PERL);
  CHECK_EQ(detect("q.SQL", ""), SQL);
  CHECK_EQ(detect("ci.yml", ""), YAML);
  CHECK_EQ(detect("Cargo.toml", ""), TOML);
  CHECK_EQ(detect("GNUmakefile", ""), MAKEFILE);
  CHECK_EQ(detect("fix.patch", ""), DIFF);
  CHECK_EQ(detect("page.html", ""), HTML);
  CHECK_EQ(detect("feed.xml", ""), XML);
  CHECK_EQ(detect("App.kt", ""), KOTLIN);
  CHECK_EQ(detect("Program.cs", ""), CSHARP);
  CHECK_EQ(detect("meson.build", ""), MESON);
  CHECK_EQ(detect("noext", "diff --git a/x b/x"), DIFF);
  CHECK_EQ((int)all_languages().size(), LANGUAGE_COUNT - 1);
  for (int l = 1; l < LANGUAGE_COUNT; l++) CHECK(std::string(language_name((Language)l)) != "Plain Text");

  // every language yields a style string of the right length on odd input
  for (int l = 0; l < LANGUAGE_COUNT; l++) {
    std::string odd = "\"unterminated '\n/* x\n<?php $a ?> [[ --[[ `\n=begin\n";
    CHECK_EQ(styles((Language)l, odd).size(), odd.size());
  }

  t = "const x: number = 1;";
  s = styles(TYPESCRIPT, t);
  CHECK_EQ(s[0], KEYWORD);
  CHECK_EQ(s[9], TYPE);
  CHECK_EQ(s[18], NUMBER);

  t = "<p><?php echo $name; // hi ?></p>";
  s = styles(PHP, t);
  CHECK_EQ(s[0], KEYWORD);     // <p>
  CHECK_EQ(s[3], PREPROC);     // <?php
  CHECK_EQ(s[9], KEYWORD);     // echo
  CHECK_EQ(s[14], TYPE);       // $name
  CHECK_EQ(s[21], COMMENT);
  t = "<?php\n# hash comment\n$x = 'a';";
  s = styles(PHP, t);
  CHECK_EQ(s[6], COMMENT);
  CHECK_EQ(s[26], STRING);

  t = "def go(x) # c\n  @a = :sym\nend";
  s = styles(RUBY, t);
  CHECK_EQ(s[0], KEYWORD);
  CHECK_EQ(s[10], COMMENT);
  CHECK_EQ(s[16], TYPE);
  CHECK_EQ(s[21], NUMBER);
  CHECK_EQ(s[26], KEYWORD);
  t = "=begin\nx\n=end\nputs 1";
  s = styles(RUBY, t);
  CHECK_EQ(s[7], COMMENT);
  CHECK_EQ(s[14], KEYWORD);

  t = "local s = [[long]] --[[ block\n]] -- line";
  s = styles(LUA, t);
  CHECK_EQ(s[0], KEYWORD);
  CHECK_EQ(s[12], STRING);
  CHECK_EQ(s[20], COMMENT);
  CHECK_EQ(s[30], COMMENT);
  CHECK_EQ(s[34], COMMENT);

  t = "my @list = (1); # c\n$x{a}";
  s = styles(PERL, t);
  CHECK_EQ(s[0], KEYWORD);
  CHECK_EQ(s[3], TYPE);
  CHECK_EQ(s[16], COMMENT);
  CHECK_EQ(s[20], TYPE);

  t = "SELECT name FROM t WHERE a = 'it''s' -- c";
  s = styles(SQL, t);
  CHECK_EQ(s[0], KEYWORD);
  CHECK_EQ(s[12], KEYWORD);
  CHECK_EQ(s[29], STRING);
  CHECK_EQ(s[34], STRING); // the '' escape stays inside the string
  CHECK_EQ(s[37], COMMENT);

  t = "---\nkey: value # c\nlist:\n  - 42\n  - true\n";
  s = styles(YAML, t);
  CHECK_EQ(s[0], HEADING);
  CHECK_EQ(s[4], TYPE);
  CHECK_EQ(s[9], PLAIN);
  CHECK_EQ(s[15], COMMENT);
  CHECK_EQ(s[20], TYPE);
  CHECK_EQ(s[29], NUMBER);
  CHECK_EQ(s[36], KEYWORD);

  t = "[package]\nname = \"x\"\nok = true";
  s = styles(TOML, t);
  CHECK_EQ(s[0], HEADING);
  CHECK_EQ(s[10], TYPE);
  CHECK_EQ(s[17], STRING);
  CHECK_EQ(s[26], KEYWORD);

  t = "CC := gcc\nall: $(OBJ)\n\t$(CC) -o $@ # c\n";
  s = styles(MAKEFILE, t);
  CHECK_EQ(s[10], FUNCTION);
  CHECK_EQ(s[15], TYPE);
  CHECK_EQ(s[23], TYPE);
  CHECK_EQ(s[33], TYPE);
  CHECK_EQ(s[36], COMMENT);

  t = "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-old\n+new\n ctx\n";
  s = styles(DIFF, t);
  CHECK_EQ(s[0], HEADING);
  CHECK_EQ(s[8], HEADING);
  CHECK_EQ(s[16], PREPROC);
  CHECK_EQ(s[28], REMOVED);
  CHECK_EQ(s[33], ADDED);
  CHECK_EQ(s[39], PLAIN);

  t = "<div class=\"a\"><script>var x = 1;</script><style>p { color: red; }</style></div>";
  s = styles(HTML, t);
  CHECK_EQ(s[1], KEYWORD);
  CHECK_EQ(s[5], TYPE);
  CHECK_EQ(s[11], STRING);
  CHECK_EQ(s[23], KEYWORD); // var
  CHECK_EQ(s[31], NUMBER);
  CHECK_EQ(s[53], TYPE);    // color:
  // XML stays plain inside text
  CHECK_EQ(styles(XML, "<a>var</a>")[3], PLAIN);

  t = "fun main() { val s = \"\"\"x\"\"\" }";
  s = styles(KOTLIN, t);
  CHECK_EQ(s[0], KEYWORD);
  CHECK_EQ(s[4], FUNCTION);
  CHECK_EQ(s[21], STRING);

  t = "#region X\nusing System;\nstring s = @\"x\";";
  s = styles(CSHARP, t);
  CHECK_EQ(s[0], PREPROC);
  CHECK_EQ(s[10], KEYWORD);
  CHECK_EQ(s[24], TYPE);

  t = "project('x', 'c')\nif true # c\nendif";
  s = styles(MESON, t);
  CHECK_EQ(s[0], TYPE);
  CHECK_EQ(s[8], STRING);
  CHECK_EQ(s[18], KEYWORD);
  CHECK_EQ(s[26], COMMENT);

  // ---- bracket matching
  t = "f(a[1], \"(\") { x }";
  s = styles(C_CPP, t);
  CHECK_EQ(match_bracket(t, s, 1), 11L);
  CHECK_EQ(match_bracket(t, s, 11), 1L);
  CHECK_EQ(match_bracket(t, s, 3), 5L);
  CHECK_EQ(match_bracket(t, s, 13), 17L);
  CHECK_EQ(match_bracket(t, s, 9), -1L); // inside a string
  CHECK_EQ(match_bracket(t, s, 0), -1L); // not a bracket
  CHECK_EQ(match_bracket("((", "", 0), -1L);
  CHECK_EQ(match_bracket("(())", "", 0), 3L);

  // ---- themes
  CHECK(themes().size() >= 5);
  CHECK_EQ(std::string(theme("auto", true).id), std::string("dark"));
  CHECK_EQ(std::string(theme("nope", false).id), std::string("light"));
  CHECK_EQ(std::string(theme("monokai", false).id), std::string("monokai"));
  return test_result("syntax");
}
