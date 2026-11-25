#!/usr/bin/env python3
"""Keeps the translation template up to date.

usage: update-po.py --pseudo <localedir>
                              builds an "en@pseudo" catalog with longer,
                              accented texts for checking layouts
                              (LANGUAGE=en@pseudo FLD_LOCALEDIR=<localedir> fl-settings)
       update-po.py           writes po/POTFILES.in and po/fldesktop.pot,
                              and merges the template into each po/<lang>.po
       update-po.py --check   fails when POTFILES.in or fldesktop.pot is out of date

POTFILES.in lists every source file that uses _(), N_(), C_() or P_()."""
import os, re, subprocess, sys, tempfile

top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
po = os.path.join(top, "po")
SKIP = ("build", ".git", "tests", "po")
mark = re.compile(r'\b[NCP]?_\(\s*"')


def sources():
    out = []
    for d, dirs, files in os.walk(top):
        rel = os.path.relpath(d, top)
        if rel.split(os.sep)[0] in SKIP or rel.startswith("build"):
            dirs[:] = []
            continue
        dirs.sort()
        for f in sorted(files):
            if f.endswith((".cpp", ".c", ".h")):
                path = os.path.join(d, f)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    if mark.search(fh.read()):
                        out.append(os.path.relpath(path, top))
    return out


def potfiles_text(files):
    return "# files with translatable text (written by tools/update-po.py)\n" + "".join(f + "\n" for f in files)


def make_pot(files, out):
    with tempfile.NamedTemporaryFile("w", delete=False) as lst:
        lst.write("".join(f + "\n" for f in files))
    subprocess.run(["xgettext", "--from-code=UTF-8", "--language=C++", "--add-comments=TRANSLATORS:",
                    "--keyword=_", "--keyword=N_", "--keyword=C_:1c,2", "--keyword=P_:1,2",
                    "--package-name=fldesktop", "--msgid-bugs-address=https://github.com/Yjlion/fldesktop/issues",
                    "--sort-by-file", "--no-wrap", "-D", top, "-f", lst.name, "-o", out], check=True)
    os.unlink(lst.name)


def strip_date(text):
    return re.sub(r'^"POT-Creation-Date: .*\\n"\n', "", text, flags=re.M)


def messages(text):
    """The (context, msgid, plural) entries of a .po/.pot text: compared by
    --check, as xgettext versions lay files out a little differently."""
    out, cur, key = set(), {}, None
    for line in text.splitlines() + [""]:
        m = re.match(r'^(msgctxt|msgid|msgid_plural|msgstr(?:\[\d+\])?) "(.*)"$', line)
        if m:
            if m.group(1) == "msgctxt" or (m.group(1) == "msgid" and "msgid" in cur):
                if "msgid" in cur and cur["msgid"]:
                    out.add((cur.get("msgctxt"), cur["msgid"], cur.get("msgid_plural")))
                cur = {}
            key = m.group(1)
            cur[key] = m.group(2)
        elif line.startswith('"') and key:
            cur[key] += line[1:-1]
        elif not line.strip():
            if cur.get("msgid"):
                out.add((cur.get("msgctxt"), cur["msgid"], cur.get("msgid_plural")))
            cur, key = {}, None
    return out


ACCENT = dict(zip("aeiouyAEIOUYcnsCNS", "åéîøüýÅÉÎØÜÝçñšÇÑŠ"))
KEEP = re.compile(r"%[-+ #0-9.]*[sdiufxXcp%]|%[1-9]|\\.|@\S*|&|\|")


def pseudo(text):
    """[Ŝéttîñgš~~~~]: about 40 % longer, accented, the same formats and markers"""
    out, last = [], 0
    for m in KEEP.finditer(text):
        out.append("".join(ACCENT.get(c, c) for c in text[last:m.start()]))
        out.append(m.group(0))
        last = m.end()
    out.append("".join(ACCENT.get(c, c) for c in text[last:]))
    body = "".join(out)
    pad = "~" * max(1, len(text) * 2 // 5)
    # each part of an FLTK "a|b|c" choice list gets its own brackets
    if "|" in text and not text.startswith("|"):
        return "|".join("[%s~]" % p for p in body.split("|"))
    return "[%s%s]" % (body, pad)


def write_pseudo(outdir):
    pot = os.path.join(po, "fldesktop.pot")
    lines = ['msgid ""', 'msgstr "Content-Type: text/plain; charset=UTF-8\\nPlural-Forms: nplurals=2; plural=(n != 1);\\n"', ""]
    for ctx, mid, plural in sorted(messages(open(pot, encoding="utf-8").read()), key=str):
        if ctx is not None:
            lines.append('msgctxt "%s"' % ctx)
        lines.append('msgid "%s"' % mid)
        if plural is not None:
            lines += ['msgid_plural "%s"' % plural, 'msgstr[0] "%s"' % pseudo(mid), 'msgstr[1] "%s"' % pseudo(plural)]
        else:
            lines.append('msgstr "%s"' % pseudo(mid))
        lines.append("")
    d = os.path.join(outdir, "en@pseudo", "LC_MESSAGES")
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, "fldesktop.po")
    open(src, "w", encoding="utf-8").write("\n".join(lines))
    subprocess.run(["msgfmt", "-o", os.path.join(d, "fldesktop.mo"), src], check=True)
    print("pseudo catalog written to", d)


def main():
    if "--pseudo" in sys.argv:
        write_pseudo(sys.argv[sys.argv.index("--pseudo") + 1])
        return
    files = sources()
    pot = os.path.join(po, "fldesktop.pot")
    with tempfile.TemporaryDirectory() as tmp:
        new = os.path.join(tmp, "fldesktop.pot")
        make_pot(files, new)
        new_text = open(new, encoding="utf-8").read()
    if "--check" in sys.argv:
        bad = False
        have = open(os.path.join(po, "POTFILES.in")).read() if os.path.exists(os.path.join(po, "POTFILES.in")) else ""
        if have != potfiles_text(files):
            print("po/POTFILES.in is out of date: run tools/update-po.py")
            bad = True
        old = open(pot, encoding="utf-8").read() if os.path.exists(pot) else ""
        if messages(old) != messages(new_text):
            print("po/fldesktop.pot is out of date: run tools/update-po.py")
            bad = True
        if not bad:
            print("translation template: %d files, up to date" % len(files))
        sys.exit(1 if bad else 0)
    open(os.path.join(po, "POTFILES.in"), "w").write(potfiles_text(files))
    old = open(pot, encoding="utf-8").read() if os.path.exists(pot) else ""
    if strip_date(old) != strip_date(new_text):
        open(pot, "w", encoding="utf-8").write(new_text)
    for f in sorted(os.listdir(po)):
        if f.endswith(".po"):
            subprocess.run(["msgmerge", "--quiet", "--update", "--backup=none", "--no-wrap", "--sort-by-file",
                            os.path.join(po, f), pot], check=True)
    print("%d files in POTFILES.in, template written" % len(files))


if __name__ == "__main__":
    main()
