#!/usr/bin/env python3
"""Generate the icon resources ShellNSE's resource script references.

The DLL used to ship with no icon resources at all, while the overlay
handler and both DefaultIcon registrations pointed at icon indices inside
it -- which is what drew a blank page badge over archive files.

Everything here is drawn procedurally with the standard library only, so
the icons can be regenerated on any machine:

    python3 tools/make_icons.py

Writes res/*.ico (plus res/icon-preview.png for eyeballing the result).
"""

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(os.path.dirname(HERE), "res")

# ── palette ───────────────────────────────────────────────────────────
AMBER_HI = (0xF7, 0xC0, 0x5E)
AMBER_LO = (0xE2, 0x8B, 0x1B)
STRAP_HI = (0xB9, 0x6A, 0x13)
STRAP_LO = (0x9A, 0x55, 0x0D)
EDGE     = (0x6E, 0x3C, 0x07)
FOLD_HI  = (0x5C, 0xA2, 0xE8)
FOLD_LO  = (0x2E, 0x6C, 0xB2)
PAPER    = (0xFF, 0xFF, 0xFF)


# ── tiny RGBA canvas ──────────────────────────────────────────────────
class Canvas:
    def __init__(self, w):
        self.w = w
        self.px = [[(0, 0, 0, 0)] * w for _ in range(w)]

    def blend(self, x, y, rgb, a):
        if a <= 0 or not (0 <= x < self.w and 0 <= y < self.w):
            return
        dr, dg, db, da = self.px[y][x]
        if a >= 255 or da == 0:
            if a >= 255:
                self.px[y][x] = (rgb[0], rgb[1], rgb[2], 255)
                return
        sa = a / 255.0
        na = sa + (da / 255.0) * (1 - sa)
        if na <= 0:
            return
        nr = (rgb[0] * sa + dr * (da / 255.0) * (1 - sa)) / na
        ng = (rgb[1] * sa + dg * (da / 255.0) * (1 - sa)) / na
        nb = (rgb[2] * sa + db * (da / 255.0) * (1 - sa)) / na
        self.px[y][x] = (int(nr + .5), int(ng + .5), int(nb + .5), int(na * 255 + .5))

    # shape helpers take a predicate returning True inside the shape
    def fill(self, box, inside, colour_at, alpha=255):
        x0, y0, x1, y1 = box
        for y in range(max(0, int(y0)), min(self.w, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(self.w, int(x1) + 1)):
                if inside(x + .5, y + .5):
                    self.blend(x, y, colour_at(x + .5, y + .5), alpha)

    def downsample(self, n):
        """Box-filter to n x n, straight (non-premultiplied) RGBA."""
        ss = self.w // n
        out = bytearray(n * n * 4)
        area = ss * ss
        for y in range(n):
            for x in range(n):
                r = g = b = a = 0
                for sy in range(y * ss, y * ss + ss):
                    row = self.px[sy]
                    for sx in range(x * ss, x * ss + ss):
                        pr, pg, pb, pa = row[sx]
                        r += pr * pa
                        g += pg * pa
                        b += pb * pa
                        a += pa
                i = (y * n + x) * 4
                if a:
                    out[i + 0] = min(255, r // a)
                    out[i + 1] = min(255, g // a)
                    out[i + 2] = min(255, b // a)
                out[i + 3] = a // area
        return bytes(out)


def vgrad(y0, y1, top, bottom):
    span = max(1.0, y1 - y0)

    def at(_x, y):
        t = min(1.0, max(0.0, (y - y0) / span))
        return (int(top[0] + (bottom[0] - top[0]) * t),
                int(top[1] + (bottom[1] - top[1]) * t),
                int(top[2] + (bottom[2] - top[2]) * t))
    return at


def solid(rgb):
    return lambda _x, _y: rgb


def rounded(x0, y0, x1, y1, r):
    def inside(x, y):
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return inside


def ring(x0, y0, x1, y1, r, t):
    outer = rounded(x0, y0, x1, y1, r)
    inner = rounded(x0 + t, y0 + t, x1 - t, y1 - t, max(0.0, r - t))
    return lambda x, y: outer(x, y) and not inner(x, y)


def disc(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def poly(points):
    def inside(x, y):
        hit = False
        n = len(points)
        for i in range(n):
            ax, ay = points[i]
            bx, by = points[(i + 1) % n]
            if (ay > y) != (by > y):
                xx = ax + (y - ay) * (bx - ax) / (by - ay)
                if x < xx:
                    hit = not hit
        return hit
    return inside


# ── the four icons ────────────────────────────────────────────────────
def draw_box(c, pad_scale=0.10, strap=True):
    """The archive 'package': rounded crate with a vertical strap."""
    w = c.w
    p = w * pad_scale
    x0, y0, x1, y1 = p, p * 1.25, w - p, w - p * 1.25
    r = w * 0.09

    c.fill((x0, y0, x1, y1), rounded(x0, y0, x1, y1, r),
           vgrad(y0, y1, AMBER_HI, AMBER_LO))
    c.fill((x0, y0, x1, y1), ring(x0, y0, x1, y1, r, w * 0.022),
           solid(EDGE), alpha=90)

    if strap:
        # Wrapped-parcel banding: one vertical strap and one horizontal tape
        # line. A white clasp was tried here and read as a pause symbol once
        # the strap was behind it.
        body = rounded(x0, y0, x1, y1, r)
        sw = w * 0.16
        c.fill((x0, y0, x1, y1),
               lambda x, y: body(x, y) and abs(x - w / 2) <= sw / 2,
               vgrad(y0, y1, STRAP_HI, STRAP_LO))
        ty = y0 + (y1 - y0) * 0.46
        th = w * 0.13
        c.fill((x0, ty - th / 2, x1, ty + th / 2),
               lambda x, y: body(x, y) and abs(y - ty) <= th / 2,
               solid(STRAP_HI), alpha=235)


def icon_archive(w):
    c = Canvas(w)
    draw_box(c)
    return c


def icon_app(w):
    """Package with a lighter top face, used as the extension's own icon."""
    c = Canvas(w)
    draw_box(c)
    p = w * 0.10
    x0, y0, x1 = p, p * 1.25, w - p
    lid = y0 + (w - p * 2.5) * 0.26
    c.fill((x0, y0, x1, lid),
           lambda x, y: rounded(x0, y0, x1, w - p * 1.25, w * 0.09)(x, y) and y <= lid,
           solid(PAPER), alpha=52)
    return c


def icon_folder_archive(w):
    """Folder with a small crate in front: the archive-as-folder object."""
    c = Canvas(w)
    p = w * 0.08
    top = w * 0.26
    tabw = w * 0.42
    # back tab
    c.fill((p, top - w * .12, p + tabw, top + 1),
           poly([(p, top), (p, top - w * .11), (p + tabw * .72, top - w * .11),
                 (p + tabw * .88, top), (p + tabw, top)]),
           solid(FOLD_LO))
    # body
    bx0, by0, bx1, by1 = p, top, w - p, w - p * 1.6
    c.fill((bx0, by0, bx1, by1), rounded(bx0, by0, bx1, by1, w * 0.07),
           vgrad(by0, by1, FOLD_HI, FOLD_LO))
    # crate in front
    cw = (bx1 - bx0) * 0.52
    cx0 = bx0 + (bx1 - bx0 - cw) / 2
    cy0 = by0 + (by1 - by0) * 0.30
    cy1 = by1 - (by1 - by0) * 0.06
    c.fill((cx0, cy0, cx0 + cw, cy1),
           rounded(cx0, cy0, cx0 + cw, cy1, w * 0.05),
           vgrad(cy0, cy1, AMBER_HI, AMBER_LO))
    sw = cw * 0.18
    c.fill((cx0 + cw / 2 - sw / 2, cy0, cx0 + cw / 2 + sw / 2, cy1),
           lambda x, y: rounded(cx0, cy0, cx0 + cw, cy1, w * 0.05)(x, y)
           and abs(x - (cx0 + cw / 2)) <= sw / 2,
           solid(STRAP_HI))
    return c


def icon_overlay(w):
    """Bottom-left badge. Must stay legible at ~10 px, so: disc + strap."""
    c = Canvas(w)
    r = w * 0.46
    cx = cy = w / 2
    c.fill((0, 0, w, w), disc(cx, cy, r), solid(PAPER))
    c.fill((0, 0, w, w), disc(cx, cy, r - w * 0.07),
           vgrad(cy - r, cy + r, AMBER_HI, AMBER_LO))
    # One white box glyph, nothing finer: this is drawn at about 10 px.
    bw = bh = w * 0.42
    bx0, by0 = cx - bw / 2, cy - bh / 2
    c.fill((bx0, by0, bx0 + bw, by0 + bh),
           rounded(bx0, by0, bx0 + bw, by0 + bh, w * 0.07), solid(PAPER))
    return c


# ── encoders ──────────────────────────────────────────────────────────
def png_bytes(n, rgba):
    raw = b"".join(b"\x00" + rgba[y * n * 4:(y + 1) * n * 4] for y in range(n))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def bmp_bytes(n, rgba):
    """BITMAPINFOHEADER + bottom-up BGRA + 1bpp AND mask."""
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, n * n * 4,
                      0, 0, 0, 0)
    xor = bytearray()
    for y in range(n - 1, -1, -1):
        for x in range(n):
            i = (y * n + x) * 4
            xor += bytes((rgba[i + 2], rgba[i + 1], rgba[i + 0], rgba[i + 3]))
    stride = ((n + 31) // 32) * 4
    mask = bytearray()
    for y in range(n - 1, -1, -1):
        row = bytearray(stride)
        for x in range(n):
            if rgba[(y * n + x) * 4 + 3] < 128:
                row[x >> 3] |= 0x80 >> (x & 7)
        mask += row
    return hdr + bytes(xor) + bytes(mask)


def write_ico(path, images):
    """images: list of (size, rgba). >=128 px stored as PNG."""
    entries, blobs, offset = [], [], 6 + 16 * len(images)
    for n, rgba in images:
        data = png_bytes(n, rgba) if n >= 128 else bmp_bytes(n, rgba)
        entries.append(struct.pack("<BBBBHHII", n & 0xFF, n & 0xFF, 0, 0, 1, 32,
                                   len(data), offset))
        blobs.append(data)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        for e in entries:
            f.write(e)
        for b in blobs:
            f.write(b)
    return sum(len(b) for b in blobs) + 6 + 16 * len(images)


# ── drive ─────────────────────────────────────────────────────────────
ICONS = [
    ("shellnse.ico",       icon_app,            [16, 20, 24, 32, 48, 64, 128, 256]),
    ("archive.ico",        icon_archive,        [16, 20, 24, 32, 48, 64, 128, 256]),
    ("folder_archive.ico", icon_folder_archive, [16, 20, 24, 32, 48, 64, 128, 256]),
    ("overlay.ico",        icon_overlay,        [16, 20, 24, 32, 48]),
]


def render(fn, n):
    ss = 8 if n <= 32 else (4 if n <= 64 else 2)
    return fn(n * ss).downsample(n)


def main():
    os.makedirs(RES, exist_ok=True)
    preview_rows = []
    for name, fn, sizes in ICONS:
        images = [(n, render(fn, n)) for n in sizes]
        size = write_ico(os.path.join(RES, name), images)
        print(f"{name:<20} {len(sizes)} sizes  {size:>7} bytes")
        preview_rows.append((name, dict(images)))

    # preview sheet: 64 px and 16 px of each icon on a checkerboard
    cell, pad = 72, 8
    W = pad + len(preview_rows) * (cell + pad)
    H = pad + cell + pad + 24
    sheet = bytearray(W * H * 4)
    for i in range(W * H):
        x, y = i % W, i // W
        v = 0xFF if ((x // 8) + (y // 8)) % 2 else 0xDD
        sheet[i * 4:i * 4 + 4] = bytes((v, v, v, 255))

    def paste(rgba, n, ox, oy):
        for y in range(n):
            for x in range(n):
                s = (y * n + x) * 4
                a = rgba[s + 3] / 255.0
                if a <= 0:
                    continue
                d = ((oy + y) * W + ox + x) * 4
                for k in range(3):
                    sheet[d + k] = int(rgba[s + k] * a + sheet[d + k] * (1 - a))

    for i, (_n, imgs) in enumerate(preview_rows):
        ox = pad + i * (cell + pad)
        big = max(k for k in imgs if k <= 64)
        paste(imgs[big], big, ox, pad)
        paste(imgs[16], 16, ox + 4, pad + cell + 4)
    with open(os.path.join(RES, "icon-preview.png"), "wb") as f:
        f.write(png_bytes_rect(W, H, bytes(sheet)))
    print("wrote res/icon-preview.png")


def png_bytes_rect(w, h, rgba):
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
