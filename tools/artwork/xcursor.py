"""Minimal PNG reader and Xcursor writer/reader (no third party modules).

The PNG reader only handles what rsvg-convert writes: 8 bit RGBA (or RGB),
non-interlaced. Xcursor files hold premultiplied ARGB images, one or more
per nominal size (several = an animation).
"""
import struct
import zlib

XCURSOR_MAGIC = b"Xcur"
IMAGE_TYPE = 0xFFFD0002


def read_png(path):
    """Returns (width, height, rows) with rows a list of bytearrays of RGBA."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat = 8, b""
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(f"{path}: unsupported PNG (depth {depth}, color type {ctype})")
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows, prev = [], bytearray(stride)
    i = 0
    for _ in range(height):
        ftype = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 255
            elif ftype == 2:
                line[x] = (line[x] + b) & 255
            elif ftype == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        prev = line
        if bpp == 3:
            rgba = bytearray()
            for x in range(width):
                rgba += line[x * 3:x * 3 + 3] + b"\xff"
            line = rgba
        rows.append(line)
    return width, height, rows


def argb_pixels(rows):
    """RGBA rows -> little endian premultiplied ARGB words."""
    out = bytearray()
    for row in rows:
        for x in range(0, len(row), 4):
            r, g, b, a = row[x], row[x + 1], row[x + 2], row[x + 3]
            r, g, b = (r * a + 127) // 255, (g * a + 127) // 255, (b * a + 127) // 255
            out += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    return bytes(out)


def write_xcursor(path, images):
    """images: list of (nominal_size, png_path, xhot, yhot, delay_ms)."""
    chunks = []
    for nominal, png, xhot, yhot, delay in images:
        w, h, rows = read_png(png)
        xhot = max(0, min(w - 1, xhot))
        yhot = max(0, min(h - 1, yhot))
        header = struct.pack("<9I", 36, IMAGE_TYPE, nominal, 1, w, h, xhot, yhot, delay)
        chunks.append((nominal, header + argb_pixels(rows)))
    ntoc = len(chunks)
    pos = 16 + 12 * ntoc
    toc, body = b"", b""
    for nominal, chunk in chunks:
        toc += struct.pack("<3I", IMAGE_TYPE, nominal, pos)
        body += chunk
        pos += len(chunk)
    with open(path, "wb") as f:
        f.write(XCURSOR_MAGIC + struct.pack("<3I", 16, 0x10000, ntoc) + toc + body)


def read_xcursor(path):
    """Returns a list of dicts (nominal, width, height, xhot, yhot, delay) - for tests."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != XCURSOR_MAGIC:
        raise ValueError(f"{path}: not an Xcursor file")
    hsize, _, ntoc = struct.unpack("<3I", data[4:16])
    out = []
    for i in range(ntoc):
        typ, sub, pos = struct.unpack("<3I", data[hsize + 12 * i:hsize + 12 * i + 12])
        if typ != IMAGE_TYPE:
            continue
        csize, ctyp, nominal, ver, w, h, xh, yh, delay = struct.unpack("<9I", data[pos:pos + 36])
        if ctyp != IMAGE_TYPE or nominal != sub or w == 0 or h == 0 or w > 0x7FFF or h > 0x7FFF:
            raise ValueError(f"{path}: bad image chunk {i}")
        if xh >= w or yh >= h:
            raise ValueError(f"{path}: hotspot outside image {i}")
        if pos + csize + w * h * 4 > len(data):
            raise ValueError(f"{path}: truncated image {i}")
        out.append(dict(nominal=nominal, width=w, height=h, xhot=xh, yhot=yh, delay=delay))
    return out
