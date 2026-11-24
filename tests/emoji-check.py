#!/usr/bin/env python3
"""Fails when program text uses emoji: characters from U+1F000 on (and the
emoji variation selector), which need a colour emoji font that is often not
installed (Debian does not install one by default). Without it they show as
boxes with a hex code; the programs use the icon theme's symbolic icons.

Comments are not checked."""
import os
import re
import sys

top = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
bad = re.compile("[\U0001F000-\U0010FFFF️]")
string = re.compile(r'"(?:[^"\\\n]|\\.)*"')
failures = []
for root in ("apps", "shell", "common", "greeter", "portal", "compositor"):
    for d, _, files in os.walk(os.path.join(top, root)):
        for f in files:
            if not f.endswith((".cpp", ".h", ".c")):
                continue
            path = os.path.join(d, f)
            for n, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                code = line.split("//")[0] if '"' not in line.split("//")[0] else line
                for m in string.finditer(code):
                    if bad.search(m.group(0)):
                        failures.append(f"{os.path.relpath(path, top)}:{n}: {m.group(0)}")
for f in failures:
    print(f)
print(f"emoji-check: {len(failures)} problems")
sys.exit(1 if failures else 0)
