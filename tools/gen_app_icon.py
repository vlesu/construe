"""Generator of the program icon (S4-APP-001, docs/dev/spec/app-ux.md §2.6а).

The drawing is our own, defined by code (license-clean): a rounded square with a vertical gradient, a white ring «C»
(the sketch contour) and two orange dots at the ends of the arc (the sketch points). Rasterization — by the distances to the shapes with
SS×SS supersampling per pixel; without third-party libraries (zlib, struct).

Output (overwritten): assets/icons/construe-<N>.png for N from SIZES and assets/icons/construe.ico — the same
images in PNG format inside ICO (Windows Vista+).

Run: python -I tools/gen_app_icon.py
"""
import math
import os
import struct
import zlib

SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4  # суперсэмплинг по каждой оси

TOP = (0x2A, 0x6C, 0xC4)      # градиент фона сверху
BOTTOM = (0x14, 0x3C, 0x78)   # и снизу
RING = (0xFF, 0xFF, 0xFF)
DOT = (0xF5, 0x8E, 0x1E)

CORNER = 0.20      # радиус скругления квадрата — доля стороны
MARGIN = 0.04      # поле до края изображения
RING_R = 0.27      # средний радиус кольца
RING_W = 0.085     # полутолщина кольца
GAP = math.radians(50)  # половина раствора разрыва кольца (справа)
DOT_R = 0.085      # радиус точек


def rounded_square_sd(x, y):
    """Signed distance to the rounded square [MARGIN, 1 − MARGIN]² (negative inside)."""
    h = 0.5 - MARGIN - CORNER
    qx, qy = abs(x - 0.5) - h, abs(y - 0.5) - h
    ox, oy = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ox, oy) + min(max(qx, qy), 0.0) - CORNER


def ring_inside(x, y):
    dx, dy = x - 0.5, y - 0.5
    ang = math.atan2(-dy, dx)  # ось y изображения — вниз
    if abs(ang) < GAP:
        return False
    return abs(math.hypot(dx, dy) - RING_R) <= RING_W


def dot_centers():
    return [(0.5 + RING_R * math.cos(a), 0.5 - RING_R * math.sin(a)) for a in (GAP, -GAP)]


def render(n):
    dots = dot_centers()
    rows = []
    for py in range(n):
        row = bytearray()
        for px in range(n):
            acc = [0.0, 0.0, 0.0, 0.0]  # премультиплицированные r, g, b и альфа
            for sy in range(SS):
                for sx in range(SS):
                    x = (px + (sx + 0.5) / SS) / n
                    y = (py + (sy + 0.5) / SS) / n
                    if rounded_square_sd(x, y) > 0:
                        continue
                    t = min(max((y - MARGIN) / (1 - 2 * MARGIN), 0.0), 1.0)
                    col = tuple(TOP[i] + (BOTTOM[i] - TOP[i]) * t for i in range(3))
                    if ring_inside(x, y):
                        col = RING
                    if any(math.hypot(x - cx, y - cy) <= DOT_R for cx, cy in dots):
                        col = DOT
                    for i in range(3):
                        acc[i] += col[i]
                    acc[3] += 1
            a = acc[3] / (SS * SS)
            if acc[3] > 0:
                r, g, b = (round(acc[i] / acc[3]) for i in range(3))
            else:
                r = g = b = 0
            row += bytes((r, g, b, round(a * 255)))
        rows.append(bytes(row))
    return rows


def png(n, rows):
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + r for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def ico(images):
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b'', b''
    for n, blob in images:
        dim = 0 if n >= 256 else n
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    return header + entries + data


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, 'assets', 'icons')
    os.makedirs(out, exist_ok=True)
    images = []
    for n in SIZES:
        blob = png(n, render(n))
        with open(os.path.join(out, f'construe-{n}.png'), 'wb') as f:
            f.write(blob)
        images.append((n, blob))
    with open(os.path.join(out, 'construe.ico'), 'wb') as f:
        f.write(ico(images))


if __name__ == '__main__':
    main()
