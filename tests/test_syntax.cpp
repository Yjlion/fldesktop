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
  return test_result("syntax");
}
