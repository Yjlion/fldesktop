#!/usr/bin/env python3
"""Writes small documents for the fl-docs format test into a directory:

 test.odt  OpenDocument text: title "fl-docs odt test" (meta.xml), a heading,
           a paragraph with a bold span and "FLDESKTOP", a bulleted list,
           a page break, a table and "Second page FLDESKTOP"   -> 2 pages
 test.epub EPUB 3: title "fl-docs epub test", two spine documents (each
           starts a page), "FLDESKTOP" in both                -> 2 pages
 big.epub  one long chapter, "FLDESKTOP" in its first and last paragraphs
           -> several hundred pages (laid out while shown)
 test.cbz  three PNGs named 1, 2 and 10 (natural order), sizes 300x400,
           320x400 and 340x400, inside a folder, plus a __MACOSX file
 nobook.zip a zip of text files (not a document)
"""
import os
import struct
import sys
import zipfile
import zlib


def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def stored_first(z, name, data):
    z.writestr(zipfile.ZipInfo(name), data, compress_type=zipfile.ZIP_STORED)


def odt(path):
    ns = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
          'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
          'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
          'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
          'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
          'xmlns:dc="http://purl.org/dc/elements/1.1/" '
          'xmlns:meta="urn:oasis:names:tc:opendocument:xmlns:meta:1.0"')
    content = f'''<?xml version="1.0" encoding="UTF-8"?>
<office:document-content {ns} office:version="1.3">
 <office:automatic-styles>
  <style:style style:name="T1" style:family="text"><style:text-properties fo:font-weight="bold"/></style:style>
  <style:style style:name="P1" style:family="paragraph"><style:paragraph-properties fo:break-before="page"/></style:style>
  <style:style style:name="C1" style:family="paragraph"><style:paragraph-properties fo:text-align="center"/></style:style>
 </office:automatic-styles>
 <office:body><office:text>
  <text:h text:outline-level="1">A heading</text:h>
  <text:p>Some <text:span text:style-name="T1">bold</text:span> text:<text:s text:c="2"/>FLDESKTOP here.</text:p>
  <text:list><text:list-item><text:p>first item</text:p></text:list-item>
   <text:list-item><text:p>second item</text:p></text:list-item></text:list>
  <text:p text:style-name="P1">Second page FLDESKTOP</text:p>
  <table:table><table:table-column table:number-columns-repeated="2"/>
   <table:table-row><table:table-cell><text:p>cell one</text:p></table:table-cell>
    <table:table-cell><text:p text:style-name="C1">cell two</text:p></table:table-cell></table:table-row>
  </table:table>
 </office:text></office:body>
</office:document-content>'''
    styles = f'''<?xml version="1.0" encoding="UTF-8"?>
<office:document-styles {ns} office:version="1.3">
 <office:automatic-styles>
  <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="21cm" fo:page-height="29.7cm"
    fo:margin-top="2cm" fo:margin-bottom="2cm" fo:margin-left="2cm" fo:margin-right="2cm"/></style:page-layout>
 </office:automatic-styles>
 <office:master-styles><style:master-page style:name="Standard" style:page-layout-name="pm1"/></office:master-styles>
</office:document-styles>'''
    meta = f'''<?xml version="1.0" encoding="UTF-8"?>
<office:document-meta {ns}><office:meta><dc:title>fl-docs odt test</dc:title></office:meta></office:document-meta>'''
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        stored_first(z, "mimetype", "application/vnd.oasis.opendocument.text")
        z.writestr("content.xml", content)
        z.writestr("styles.xml", styles)
        z.writestr("meta.xml", meta)
        z.writestr("META-INF/manifest.xml", "<manifest/>")


def epub(path):
    container = '''<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
 <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>'''
    opf = '''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>fl-docs epub test</dc:title>
  <dc:identifier id="id">test</dc:identifier></metadata>
 <manifest>
  <item id="c1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
  <item id="c2" href="text/ch%202.xhtml" media-type="application/xhtml+xml"/>
  <item id="img" href="images/dot.png" media-type="image/png"/>
 </manifest>
 <spine><itemref idref="c1"/><itemref idref="c2"/></spine>
</package>'''
    ch1 = '''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>One</title><style>p{color:red}</style></head>
<body><h1>Chapter&nbsp;one</h1><p>Hello <em>FLDESKTOP</em> &amp; friends.</p>
<ul><li>an item</li><li><p>a paragraph item</p></li></ul>
<p><img src="../images/dot.png" alt="dot"/></p></body></html>'''
    ch2 = '''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><h2>Chapter two</h2>
<p style="text-align:center">More fldesktop text.</p><pre>  code  block</pre></body></html>'''
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        stored_first(z, "mimetype", "application/epub+zip")
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/text/ch1.xhtml", ch1)
        z.writestr("OEBPS/text/ch 2.xhtml", ch2)
        z.writestr("OEBPS/images/dot.png", png(40, 30, (200, 30, 30)))


def big_epub(path):
    container = '''<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
 <rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>'''
    opf = '''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>fl-docs big book</dc:title>
  <dc:identifier id="id">big</dc:identifier></metadata>
 <manifest><item id="c" href="c.xhtml" media-type="application/xhtml+xml"/></manifest>
 <spine><itemref idref="c"/></spine>
</package>'''
    text = "All work and no play makes a dull book, line after line. " * 12
    paras = ["<p>%d %s</p>" % (i, text) for i in range(3000)]
    paras[0] = "<p>FLDESKTOP first</p>"
    paras[-1] = "<p>FLDESKTOP last</p>"
    ch = ('<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><body>'
          + "".join(paras) + "</body></html>")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        stored_first(z, "mimetype", "application/epub+zip")
        z.writestr("META-INF/container.xml", container)
        z.writestr("content.opf", opf)
        z.writestr("c.xhtml", ch)


def cbz(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("__MACOSX/comic/._1.png", b"junk")
        z.writestr("comic/10.png", png(340, 400, (0, 0, 200)))
        z.writestr("comic/2.png", png(320, 400, (0, 200, 0)))
        z.writestr("comic/1.png", png(300, 400, (200, 0, 0)))
        z.writestr("comic/notes.txt", "not a page")


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    odt(os.path.join(out, "test.odt"))
    epub(os.path.join(out, "test.epub"))
    big_epub(os.path.join(out, "big.epub"))
    cbz(os.path.join(out, "test.cbz"))
    with zipfile.ZipFile(os.path.join(out, "nobook.zip"), "w") as z:
        z.writestr("a.txt", "a")
        z.writestr("b.txt", "b")


if __name__ == "__main__":
    main()
