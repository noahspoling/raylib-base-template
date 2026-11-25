#!/usr/bin/env python3
import math
import os
import random

from PIL import Image

SIZE = 64
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "packages", "core", "textures", "terrain")
FX = os.path.join(HERE, "..", "packages", "core", "textures", "fx")
FRAMES, FW, FH = 6, 24, 20


def put(img, x, y, c):
    img.putpixel((x % SIZE, y % SIZE), c)


def riverbed(seed=7):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (SIZE, SIZE))
    dark, base, light = (128, 112, 80, 255), (150, 132, 96, 255), (176, 158, 116, 255)
    for y in range(SIZE):
        for x in range(SIZE):
            ph = 2 * math.pi * (5 * x / SIZE + 1 * y / SIZE) + 0.9 * math.sin(2 * math.pi * 2 * y / SIZE)
            r = math.sin(ph)
            c = light if r > 0.72 else dark if r < -0.55 else base
            g = rnd.random()
            if g < 0.06:
                c = tuple(max(0, v - 18) for v in c[:3]) + (255,)
            elif g < 0.10:
                c = tuple(min(255, v + 14) for v in c[:3]) + (255,)
            img.putpixel((x, y), c)
    pebbles = [(108, 104, 98, 255), (92, 86, 80, 255), (132, 122, 104, 255), (84, 92, 96, 255)]
    for _ in range(14):
        px, py = rnd.randrange(SIZE), rnd.randrange(SIZE)
        c = rnd.choice(pebbles)
        w, h = rnd.choice([(2, 1), (2, 2), (3, 2), (1, 1)])
        for dy in range(h):
            for dx in range(w):
                put(img, px + dx, py + dy, c)
        put(img, px, py, tuple(min(255, v + 30) for v in c[:3]) + (255,))
        put(img, px + w - 1, py + h, (96, 84, 60, 255))
    return img


def kelp(seed=11):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (SIZE, SIZE), (30, 50, 36, 255))
    for y in range(SIZE):
        for x in range(SIZE):
            if rnd.random() < 0.08:
                img.putpixel((x, y), (40, 62, 42, 255))
    greens = [(48, 88, 46, 255), (62, 104, 50, 255), (80, 120, 56, 255), (70, 96, 44, 255)]
    for i in range(9):
        x0 = (i * 7 + rnd.randrange(4)) % SIZE
        c = rnd.choice(greens)
        hi = tuple(min(255, v + 34) for v in c[:3]) + (255,)
        phase = rnd.random() * 2 * math.pi
        amp = 1.5 + rnd.random() * 1.5
        for y in range(SIZE):
            x = x0 + round(amp * math.sin(2 * math.pi * 2 * y / SIZE + phase))
            put(img, x, y, c)
            put(img, x + 1, y, c)
            if (y + i * 3) % 6 == 0:
                side = 1 if (y // 6 + i) % 2 else -1
                put(img, x + (2 if side > 0 else -1), y, c)
                put(img, x + (3 if side > 0 else -2), y + 1, c)
            if (y + i) % 9 == 0:
                put(img, x, y, hi)
    return img


def splash(seed=5):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (FW * FRAMES, FH), (0, 0, 0, 0))
    white, light, edge = (240, 248, 255), (176, 214, 240), (120, 170, 220)
    drops = [(rnd.uniform(-7, 7), rnd.uniform(9, 14)) for _ in range(7)]
    cx, cy = FW // 2, FH - 3
    for f in range(FRAMES):
        t = (f + 1) / FRAMES
        fade = 1.0 - 0.75 * t

        def px(x, y, c, a):
            x, y = int(round(x)), int(round(y))
            if 0 <= x < FW and 0 <= y < FH:
                img.putpixel((f * FW + x, y), c + (int(255 * a),))
        rx, ry = 3 + 8 * t, 1.2 + 2.2 * t
        for i in range(48):
            a = 2 * math.pi * i / 48
            px(cx + rx * math.cos(a), cy + ry * math.sin(a), light if i % 3 else white, 0.85 * fade)
        for vx, vy in drops:
            x = cx + vx * t * 1.3
            y = cy - (vy * t * 1.7 - 11 * t * t * 1.7)
            if y < cy + 1:
                px(x, y, white, fade)
                px(x + 1, y, light, fade)
                px(x, y + 1, edge, 0.8 * fade)
        if f < 3:
            for h in range(6 - 2 * f):
                px(cx, cy - 2 - h, white, 0.9)
                px(cx - 1 + (h % 2) * 2, cy - 1 - h, light, 0.7)
    return img


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(FX, exist_ok=True)
    for folder, name, make in ((OUT, "riverbed", riverbed), (OUT, "kelp", kelp), (FX, "splash", splash)):
        path = os.path.join(folder, name + ".png")
        make().save(path)
        print("wrote", os.path.relpath(path))
