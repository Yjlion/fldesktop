#!/usr/bin/env python3
"""Writes a small PDF for the fl-docs test: a portrait page with "Hello
fldesktop" near the top and a landscape page with "Second page, fldesktop"."""
import sys

pages = [((612, 792), b"BT /F1 24 Tf 72 700 Td (Hello fldesktop) Tj ET"),
         ((792, 612), b"BT /F1 18 Tf 72 300 Td (Second page, fldesktop) Tj ET")]
objs = [b"<< /Type /Catalog /Pages 2 0 R >>", None,
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
kids = []
for (w, h), content in pages:
    stream = len(objs) + 1
    objs.append(b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content))
    objs.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Contents %d 0 R "
                b"/Resources << /Font << /F1 3 0 R >> >> >>" % (w, h, stream))
    kids.append(len(objs))
objs[1] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (b" ".join(b"%d 0 R" % k for k in kids), len(kids))
objs.append(b"<< /Title (fl-docs test) >>")
info = len(objs)
out = bytearray(b"%PDF-1.4\n")
offsets = []
for i, o in enumerate(objs, 1):
    offsets.append(len(out))
    out += b"%d 0 obj\n%s\nendobj\n" % (i, o)
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += b"trailer\n<< /Size %d /Root 1 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, info, xref)
open(sys.argv[1], "wb").write(out)
