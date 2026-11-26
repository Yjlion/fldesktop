#!/usr/bin/env python3
"""Marks user-visible string literals for translation with _() in C++ files.

usage: i18n-mark.py [--dry-run] <file.cpp>...

Only places where a literal is clearly shown to the user are changed:
  - the label (5th argument) of `new Widget(x, y, w, h, "Label")`
  - ->label("..."), ->tooltip("..."), ->copy_label("..."), ->copy_tooltip("...")
  - the text arguments of fl_alert, fl_message, fl_choice, fl_ask, fl_input, fl_password
  - the first argument of ->add("...") (menu and choice items)
  - fl-settings' form helpers: f.note("..."), f.row("..."), f.heading("..."), check(f, "..."), *_choice(f, "...")
Literals that look like identifiers, paths, FLTK symbols (@...) or formats
without words are left alone. Review the diff: text that a program compares
(w->label() == "...") has to use _() on both sides. The file gets
#include "fld/i18n.h" when something was marked."""
import re
import sys

LIT = r'"(?:[^"\\\n]|\\.)*"'
LITS = r'%s(?:\s*%s)*' % (LIT, LIT)


def translatable(lit):
    parts = re.findall(LIT, lit)
    s = "".join(p[1:-1] for p in parts)
    if not re.search(r"[A-Za-z]{2}", s):
        return False
    if s.startswith("@") and " " not in s:
        return False
    if re.fullmatch(r"[a-z0-9_.:/+-]+", s):  # identifiers, ids, paths, commands
        return False
    if s.startswith(("/", "fldesktop.", "org.", "http")):
        return False
    return True


def split_args(src, i):
    """src[i] is just after '('. Returns (list of (start, end) of top-level args, index of ')')."""
    depth, start, args, j = 0, i, [], i
    while j < len(src):
        c = src[j]
        if c == '"':
            m = re.compile(LIT).match(src, j)
            j = m.end() if m else j + 1
            continue
        if c == "'":
            m = re.compile(r"'(?:[^'\\\n]|\\.)*'").match(src, j)
            j = m.end() if m else j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                args.append((start, j))
                return args, j
            depth -= 1
        elif c == "," and depth == 0:
            args.append((start, j))
            start = j + 1
        j += 1
    return args, -1


def wrap_arg(src, a, b, edits):
    text = src[a:b]
    m = re.fullmatch(r"(\s*)(%s)(\s*)" % LITS, text, re.S)
    if m and translatable(m.group(2)):
        edits.append((a + len(m.group(1)), a + len(m.group(1)) + len(m.group(2))))


def mark(src):
    edits = []
    # widget constructors with a label
    for m in re.finditer(r"\bnew\s+(?:\w+::)*\w+\s*\(", src):
        args, end = split_args(src, m.end())
        if end > 0 and len(args) >= 5:
            wrap_arg(src, *args[4], edits)
    # setters
    for m in re.finditer(r"(?:->|\.)(?:label|tooltip|copy_label|copy_tooltip)\(", src):
        args, end = split_args(src, m.end())
        if end > 0 and len(args) == 1:
            wrap_arg(src, *args[0], edits)
    # dialogs: every literal argument
    for m in re.finditer(r"\bfl_(?:alert|message|choice|ask|input|password)\s*\(", src):
        args, end = split_args(src, m.end())
        for a in args:
            wrap_arg(src, *a, edits)
    # fl-settings form helpers
    for m in re.finditer(r"(?:->|\.)add\s*\(", src):  # menu and choice items
        args, end = split_args(src, m.end())
        if args:
            wrap_arg(src, *args[0], edits)
    for m in re.finditer(r"\.(?:row|heading|note)\s*\(", src):
        args, end = split_args(src, m.end())
        if args:
            wrap_arg(src, *args[0], edits)
    for m in re.finditer(r"\b(?:check|\w+_choice)\s*\(", src):
        args, end = split_args(src, m.end())
        if len(args) >= 2:
            wrap_arg(src, *args[1], edits)
    # skip literals already inside _( ) / N_( ) / C_( )
    out, last = [], 0
    for a, b in sorted(set(edits)):
        if a < last:
            continue
        before = src[max(0, a - 4):a]
        if re.search(r"\b[NCP]?_\(\s*$", before):
            continue
        out.append(src[last:a])
        out.append("_(" + src[a:b] + ")")
        last = b
    out.append(src[last:])
    return "".join(out), len([1 for _ in out]) // 2


def add_include(src):
    if 'fld/i18n.h' in src:
        return src
    incs = list(re.finditer(r'^#include [<"].*[>"]\n', src, re.M))
    if not incs:
        return '#include "fld/i18n.h"\n' + src
    pos = incs[0].start()
    return src[:pos] + '#include "fld/i18n.h"\n' + src[pos:]


def main():
    dry = "--dry-run" in sys.argv
    total = 0
    for path in [a for a in sys.argv[1:] if not a.startswith("--")]:
        src = open(path, encoding="utf-8").read()
        new, n = mark(src)
        if new != src:
            new = add_include(new)
            total += n
            print("%s: %d" % (path, n))
            if not dry:
                open(path, "w", encoding="utf-8").write(new)
    print("%d strings marked" % total)


if __name__ == "__main__":
    main()
