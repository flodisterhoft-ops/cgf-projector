"""Draw the Pictures tab's rotate icons (src/icons/rotate_left.png, rotate_right.png).

  python tools/branding/make_rotate_icons.py

22x22 like up.png and down.png: a blue three-quarter arrow, drawn large and
scaled down. rotate_left is the mirror image of rotate_right.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageOps

ICONS = Path(__file__).resolve().parent.parent.parent / "src" / "icons"
S = 16            # drawing scale
N = 22 * S
DARK = (22, 44, 140, 255)
BLUE = (72, 110, 232, 255)


def arrow(colour, grow):
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = N / 2
    r = 6.6 * S
    w = 3.2 * S + 2 * grow
    # Clockwise arc from the left (180 deg) round to the top (270 + 360).
    start, end = -60, 200
    box = [cx - r - w / 2, cy - r - w / 2, cx + r + w / 2, cy + r + w / 2]
    d.arc(box, start, end, fill=colour, width=int(w))
    # Round the tail.
    a = math.radians(start)
    tx, ty = cx + r * math.cos(a), cy + r * math.sin(a)
    d.ellipse([tx - w / 2, ty - w / 2, tx + w / 2, ty + w / 2], fill=colour)
    # Arrowhead at the far end, pointing along the clockwise direction.
    a = math.radians(end)
    hx, hy = cx + r * math.cos(a), cy + r * math.sin(a)
    dx, dy = -math.sin(a), math.cos(a)          # tangent, clockwise on screen
    nx, ny = math.cos(a), math.sin(a)           # outward normal
    half = 4.6 * S + grow
    length = 6.4 * S + grow
    back = 0.6 * S + grow
    d.polygon([(hx - dx * back + nx * half, hy - dy * back + ny * half),
               (hx - dx * back - nx * half, hy - dy * back - ny * half),
               (hx + dx * length, hy + dy * length)], fill=colour)
    return img


def main():
    right = Image.alpha_composite(arrow(DARK, 0.9 * S), arrow(BLUE, 0))
    right = right.resize((22, 22), Image.LANCZOS)
    right.save(ICONS / "rotate_right.png")
    ImageOps.mirror(right).save(ICONS / "rotate_left.png")
    print("wrote rotate_right.png and rotate_left.png in", ICONS)


if __name__ == "__main__":
    main()
