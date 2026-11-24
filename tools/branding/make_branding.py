"""Draw CGF Projector's splash and app icon.

The style follows SoftProjector's cloud pictures: overlapping ellipses with a
light fill, light-blue outlines and a faint navy shadow, and a navy word mark
with a light band through the middle, in Arial Rounded MT Bold (installed
with Microsoft Office; C:\\Windows\\Fonts\\ARLRDBD.TTF).

    python tools/branding/make_branding.py [--out src]

writes into src/:
    icons/splash.png, icons/splash@2x.png   splash and About picture (450 x 180)
    icons/app-<size>.png                    window and taskbar icon, 16 to 256 px
    cgf-projector.ico                       the .exe icon (app and launcher)

Each size is drawn on its own, eight times larger and then averaged, so small
icons stay sharp. Sizes up to 24 px use larger letters and only the outer
outline of the cloud.
"""
import argparse
import io
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = r"C:\Windows\Fonts\ARLRDBD.TTF"  # Arial Rounded MT Bold

# Word mark colour by height: 0 = top of the capitals, 1 = baseline.
# Sampled from SoftProjector's splash.
NAVY = [(-1.0, (0, 36, 88)), (0.0, (0, 36, 88)), (0.5, (134, 152, 212)),
        (0.69, (0, 35, 88)), (1.0, (2, 37, 89)), (2.0, (2, 37, 89))]
# Small icons: a darker band keeps the letters readable.
NAVY_SMALL = [(-1.0, (0, 36, 88)), (0.0, (0, 36, 88)), (0.5, (52, 84, 150)),
              (0.69, (0, 35, 88)), (2.0, (2, 37, 89))]

FILL_CENTER = np.array([236, 238, 241]) / 255
FILL_EDGE = np.array([213, 223, 238]) / 255
STROKE = np.array([160, 194, 244]) / 255
SHADOW = np.array([8, 43, 94]) / 255

# Clouds as ellipses (cx, cy, rx, ry), drawn back to front.
SPLASH_CLOUDS = [  # 450 x 180
    (40, 98, 34, 30), (418, 90, 29, 26),       # ends
    (78, 86, 40, 44), (396, 92, 44, 46),       # shoulders
    (130, 70, 50, 50), (225, 74, 50, 50),      # top
    (320, 74, 68, 66),
    (180, 104, 60, 45), (282, 102, 60, 45),    # middle
    (52, 112, 30, 26),                         # front
    (90, 134, 46, 36), (138, 146, 26, 22),
    (174, 130, 38, 33), (220, 128, 24, 20),
    (258, 126, 26, 21), (302, 128, 40, 32),
    (350, 126, 28, 27), (388, 112, 34, 30),
]
ICON_CLOUDS = [  # 256 x 256
    (64, 96, 46, 46), (200, 100, 44, 44),      # top
    (136, 76, 58, 58),
    (30, 140, 28, 32), (226, 142, 28, 32),     # ends
    (90, 136, 70, 50), (172, 136, 70, 50),     # middle
    (62, 178, 46, 38), (196, 178, 46, 38),     # front
    (128, 190, 50, 40),
]

SPLASH_SIZE = (450, 180)
ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)
SUPERSAMPLE = 8


class Canvas:
    """Premultiplied RGBA canvas, drawn large and averaged down."""

    def __init__(self, w, h, ss):
        self.ss = ss
        self.w, self.h = w * ss, h * ss
        self.rgb = np.zeros((self.h, self.w, 3), np.float32)
        self.a = np.zeros((self.h, self.w), np.float32)
        self.yy, self.xx = np.mgrid[0:self.h, 0:self.w].astype(np.float32)

    def mask(self, draw):
        im = Image.new("L", (self.w, self.h), 0)
        draw(ImageDraw.Draw(im))
        return np.asarray(im, np.float32) / 255

    def paint(self, color, alpha):
        a = alpha[..., None]
        self.rgb = color * a + self.rgb * (1 - a)
        self.a = alpha + self.a * (1 - alpha)

    def image(self):
        ss, h, w = self.ss, self.h // self.ss, self.w // self.ss
        rgb = self.rgb.reshape(h, ss, w, ss, 3).mean(axis=(1, 3))
        a = self.a.reshape(h, ss, w, ss).mean(axis=(1, 3))
        rgb = np.clip(rgb / np.maximum(a, 1e-6)[..., None], 0, 1)
        out = np.dstack([rgb, np.clip(a, 0, 1)[..., None]])
        return Image.fromarray(np.round(out * 255).astype(np.uint8), "RGBA")


def clouds(cv, ellipses, k, stroke_w, shadow, inner):
    """k: canvas pixels per layout unit."""
    def box(e):
        cx, cy, rx, ry = e
        return [(cx - rx) * k, (cy - ry) * k, (cx + rx) * k, (cy + ry) * k]

    sw = max(1, round(stroke_w * k))
    union = cv.mask(lambda d: [d.ellipse(box(e), fill=255) for e in ellipses])
    if shadow:
        blur = Image.fromarray(np.uint8(union * 255)).filter(ImageFilter.GaussianBlur(2.6 * k))
        cv.paint(SHADOW, np.asarray(blur, np.float32) / 255 * shadow)
    for e in ellipses:
        cx, cy, rx, ry = e
        # Lighter towards the upper left of each ellipse.
        hx, hy = (cx - 0.25 * rx) * k, (cy - 0.35 * ry) * k
        dist = np.sqrt(((cv.xx - hx) / (rx * k * 1.3)) ** 2 + ((cv.yy - hy) / (ry * k * 1.3)) ** 2)
        t = (np.clip(dist, 0, 1) ** 1.6)[..., None]
        cv.paint(FILL_CENTER * (1 - t) + FILL_EDGE * t, cv.mask(lambda d: d.ellipse(box(e), fill=255)))
        if inner:
            cv.paint(STROKE, cv.mask(lambda d: d.ellipse(box(e), outline=255, width=sw)))
    if not inner:
        shrunk = Image.fromarray(np.uint8(union * 255)).filter(ImageFilter.MinFilter(2 * sw + 1))
        cv.paint(STROKE, np.clip(union - np.asarray(shrunk, np.float32) / 255, 0, 1))


def fit_font(text, max_w, max_cap):
    """Largest font size (in layout units) that fits; returns (size, width)."""
    size, fit = 10.0, (10.0, 0.0)
    while True:
        f = ImageFont.truetype(FONT, round(size * 20))
        left, _, right, _ = f.getbbox(text, anchor="ls")
        cap = -f.getbbox("H", anchor="ls")[1]
        if (right - left) / 20 > max_w or cap / 20 > max_cap:
            return fit
        fit = (size, (right - left) / 20)
        size += 0.25


def word(cv, text, stops, k, max_w, max_cap, center_x, baseline):
    size, width = fit_font(text, max_w, max_cap)
    font = ImageFont.truetype(FONT, round(size * k))
    top = baseline * k + font.getbbox("H", anchor="ls")[1]
    t = (np.arange(cv.h, dtype=np.float32) - top) / (baseline * k - top)
    ts = [s[0] for s in stops]
    rows = np.stack([np.interp(t, ts, [s[1][c] for s in stops]) for c in range(3)], axis=-1) / 255
    m = cv.mask(lambda d: d.text(((center_x - width / 2) * k, baseline * k), text,
                                 font=font, fill=255, anchor="ls"))
    cv.paint(np.repeat(rows[:, None, :], cv.w, axis=1), m)


def splash(scale):
    w, h = SPLASH_SIZE
    cv = Canvas(w * scale, h * scale, SUPERSAMPLE)
    k = scale * SUPERSAMPLE
    clouds(cv, SPLASH_CLOUDS, k, stroke_w=1.6, shadow=0.55, inner=True)
    word(cv, "CGF Projector", NAVY, k, max_w=392, max_cap=43, center_x=225, baseline=103)
    return cv.image()


def icon(px):
    cv = Canvas(px, px, SUPERSAMPLE)
    k = px / 256 * SUPERSAMPLE
    small = px <= 24
    clouds(cv, ICON_CLOUDS, k, stroke_w=3.2 if px >= 64 else 230 / px,
           shadow=0.5 if px >= 32 else 0, inner=not small)
    if small:
        word(cv, "CGF", NAVY_SMALL, k, max_w=228, max_cap=86, center_x=128, baseline=178)
    else:
        word(cv, "CGF", NAVY if px >= 64 else NAVY_SMALL, k, max_w=200, max_cap=70,
             center_x=128, baseline=170)
    return cv.image()


def write_ico(path, images):
    """Windows icon: 32-bit bitmaps up to 64 px, PNG for 256 px."""
    blobs = []
    for im in images:
        n = im.width
        if n >= 256:
            buf = io.BytesIO()
            im.save(buf, "PNG")
            blobs.append(buf.getvalue())
            continue
        px = np.asarray(im, np.uint8)[::-1]  # bottom-up rows
        bgra = px[:, :, [2, 1, 0, 3]].tobytes()
        # AND mask: 1 = transparent, for programs that ignore the alpha channel.
        mask_row = ((n + 31) // 32) * 4
        bits = np.packbits(px[:, :, 3] == 0, axis=1)
        mask = np.zeros((n, mask_row), np.uint8)
        mask[:, :bits.shape[1]] = bits
        header = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, 32, 0, len(bgra) + mask.size, 0, 0, 0, 0)
        blobs.append(header + bgra + mask.tobytes())
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        n = im.width
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(here, "..", "..", "src"))
    out = os.path.abspath(ap.parse_args().out)
    os.makedirs(os.path.join(out, "icons"), exist_ok=True)
    splash(1).save(os.path.join(out, "icons", "splash.png"))
    splash(2).save(os.path.join(out, "icons", "splash@2x.png"))
    icons = [icon(px) for px in ICON_SIZES]
    for px, im in zip(ICON_SIZES, icons):
        im.save(os.path.join(out, "icons", f"app-{px}.png"))
    write_ico(os.path.join(out, "cgf-projector.ico"), icons)
    print("wrote splash, %d icon sizes and cgf-projector.ico to %s" % (len(icons), out))


if __name__ == "__main__":
    main()
