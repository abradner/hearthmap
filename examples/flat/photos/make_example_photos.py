#!/usr/bin/env python3
"""Draw the fictional example flat's plant "photos": simple generated shapes, no real photos.

They exist so the example exercises the photo path end to end (examples/flat/config.json names
them). Re-run to regenerate: python3 examples/flat/photos/make_example_photos.py (needs Pillow).
"""

from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
SIZE = 256

# id -> (background, leaf colour, leaf count)
PLANTS = {
    "monstera": ((24, 40, 30), (46, 125, 70), 3),
    "fiddle-leaf-fig": ((30, 34, 28), (70, 120, 50), 5),
    "basil": ((28, 38, 24), (110, 170, 70), 7),
    "snake-plant": ((32, 32, 26), (120, 150, 60), 4),
    "tomato": ((36, 28, 26), (200, 60, 50), 6),
}


def draw(bg, leaf, count):
    img = Image.new("RGB", (SIZE, SIZE), bg)
    d = ImageDraw.Draw(img)
    for i in range(count):
        cx = SIZE * (i + 1) / (count + 1)
        top = 40 + (i % 2) * 30
        d.ellipse([cx - 24, top, cx + 24, SIZE - 40], fill=leaf)
    d.rectangle([0, SIZE - 36, SIZE, SIZE], fill=(80, 60, 45))  # pot
    return img


if __name__ == "__main__":
    for pid, spec in PLANTS.items():
        draw(*spec).save(HERE / f"{pid}.png", optimize=True)
        print(f"wrote {pid}.png")
