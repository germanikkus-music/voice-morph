"""Draws Voice Morph's neon backdrop (1280 x 628, the MPC tab area) to skin/backdrop.png.

The four panels sit behind the 4 x 4 control grid that tools/gen_ui.py lays out, one panel
per Q-Link column: VOICE, TUNE, FX, EQ. Needs Pillow (pip install pillow).

    python "plugins/Voice Morph/skin/make_backdrop.py"
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 1280, 628
S = 2                                   # draw at 2x, then scale down for smooth edges
SLOT_X = [23, 343, 663, 983]            # must match tools/gen_ui.py
SLOT_Y = [45, 196, 346, 497]
SLOT_W, SLOT_H = 278, 107
COLUMNS = [("VOICE", (45, 226, 255)), ("TUNE", (176, 107, 255)), ("FX", (255, 79, 216)), ("EQ", (63, 169, 255))]
FONT = "/System/Library/Fonts/Avenir Next Condensed.ttc"


def font(size, index=2):
    try:
        return ImageFont.truetype(FONT, size * S, index=index)
    except OSError:
        return ImageFont.load_default()


def glow(layer, radius):
    """The layer, plus a blurred copy of it underneath for a neon glow."""
    halo = layer.filter(ImageFilter.GaussianBlur(radius * S))
    return Image.alpha_composite(Image.alpha_composite(halo, halo), layer)


def panel_box(c):
    return (SLOT_X[c] - 10, 38, SLOT_X[c] + SLOT_W + 10, 618)


def s(box):
    return tuple(v * S for v in box)


def main():
    rnd = random.Random(7)

    # Base: deep navy, lighter towards the centre, with a faint carbon weave.
    base = Image.new("RGBA", (W * S, H * S))
    px = base.load()
    for y in range(H * S):
        for x in range(0, W * S):
            dx, dy = (x / (W * S) - 0.5), (y / (H * S) - 0.45)
            v = max(0.0, 1.0 - 1.6 * math.hypot(dx, dy * 1.3))
            weave = 3 if ((x // (3 * S)) + (y // (3 * S))) % 2 else 0
            px[x, y] = (int(6 + 10 * v) + weave, int(9 + 13 * v) + weave, int(20 + 26 * v) + weave, 255)

    # Panels: translucent glass with a fine analyser grid and row hairlines.
    glass = Image.new("RGBA", base.size, (0, 0, 0, 0))
    g = ImageDraw.Draw(glass)
    for c, (_, col) in enumerate(COLUMNS):
        x0, y0, x1, y1 = panel_box(c)
        g.rounded_rectangle(s((x0, y0, x1, y1)), radius=14 * S, fill=(14, 20, 42, 210))
        for gx in range(x0 + 12, x1 - 6, 22):
            g.line(s((gx, y0 + 6, gx, y1 - 6)), fill=col + (9,), width=S)
        for gy in range(y0 + 12, y1 - 6, 22):
            g.line(s((x0 + 6, gy, x1 - 6, gy)), fill=col + (9,), width=S)
        for r in range(1, 4):
            hy = (SLOT_Y[r - 1] + SLOT_H + SLOT_Y[r]) // 2
            g.line(s((x0 + 18, hy, x1 - 18, hy)), fill=col + (40,), width=S)
    base = Image.alpha_composite(base, glass)

    # Neon outlines and corner brackets.
    neon = Image.new("RGBA", base.size, (0, 0, 0, 0))
    n = ImageDraw.Draw(neon)
    for c, (_, col) in enumerate(COLUMNS):
        x0, y0, x1, y1 = panel_box(c)
        n.rounded_rectangle(s((x0, y0, x1, y1)), radius=14 * S, outline=col + (150,), width=2 * S)
        L = 26
        for (cx, cy, sx, sy) in ((x0 - 5, y0 - 5, 1, 1), (x1 + 5, y0 - 5, -1, 1), (x0 - 5, y1 + 5, 1, -1), (x1 + 5, y1 + 5, -1, -1)):
            n.line(s((cx, cy, cx + sx * L, cy)), fill=col + (255,), width=3 * S)
            n.line(s((cx, cy, cx, cy + sy * L)), fill=col + (255,), width=3 * S)
    base = Image.alpha_composite(base, glow(neon, 6))

    # Header: one glowing voice waveform running across the top, cyan to magenta to blue.
    wave = Image.new("RGBA", base.size, (0, 0, 0, 0))
    wd = ImageDraw.Draw(wave)
    bursts = [(rnd.uniform(0.02, 0.98), rnd.uniform(0.015, 0.05), rnd.uniform(0.5, 1.0)) for _ in range(22)]
    pts = []
    for x in range(0, W * S, 2):
        t = x / (W * S)
        env = 0.12 + sum(a * math.exp(-((t - c) / w) ** 2) for c, w, a in bursts)   # syllable-like bursts
        env = min(1.0, env)
        y = 19 + 14 * env * math.sin(t * 2 * math.pi * 90 + 4 * math.sin(t * 23)) * (0.75 + 0.25 * rnd.random())
        pts.append((x, y * S))
    for i in range(len(pts) - 1):
        t = i / len(pts)
        seg = min(3, int(t * 4))
        a, b = COLUMNS[seg][1], COLUMNS[min(3, seg + 1)][1]
        f = t * 4 - seg
        colr = tuple(int(a[k] + (b[k] - a[k]) * f) for k in range(3))
        wd.line([pts[i], pts[i + 1]], fill=colr + (170,), width=S)
    base = Image.alpha_composite(base, glow(wave, 3))

    # Column titles in dark plates over the waveform.
    title = Image.new("RGBA", base.size, (0, 0, 0, 0))
    t = ImageDraw.Draw(title)
    f = font(24, 0)
    plates = Image.new("RGBA", base.size, (0, 0, 0, 0))
    p = ImageDraw.Draw(plates)
    for c, (name, col) in enumerate(COLUMNS):
        text = " ".join(name)   # letter-spaced
        w = t.textlength(text, font=f) / S
        cx = SLOT_X[c] + SLOT_W / 2
        p.rounded_rectangle(s((cx - w / 2 - 16, 4, cx + w / 2 + 16, 34)), radius=8 * S, fill=(8, 12, 26, 235),
                            outline=col + (200,), width=S)
        t.text(((cx - w / 2) * S, 4 * S), text, font=f, fill=col + (255,))
    base = Image.alpha_composite(base, plates)
    base = Image.alpha_composite(base, glow(title, 4))

    out = base.resize((W, H), Image.LANCZOS).convert("RGB")
    path = os.path.join(HERE, "backdrop.png")
    out.save(path, optimize=True)
    print("wrote", path)


if __name__ == "__main__":
    main()
