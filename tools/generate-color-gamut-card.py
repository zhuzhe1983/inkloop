#!/usr/bin/env python3
"""Generate the 528x792 Inkloop colour-gamut diagnostic card.

The reference is intentionally rendered as ordinary sRGB pixels. It should be
uploaded through the same path as a normal image so the selected six-colour
renderer, Bluetooth transfer and physical panel are tested together.
"""

from __future__ import annotations

import colorsys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


WIDTH = 528
HEIGHT = 792
MARGIN = 12
CONTENT_WIDTH = WIDTH - MARGIN * 2
OUTPUT = Path(__file__).resolve().parents[1] / "public" / "calibration" / "inkloop-full-gamut-reference-v2.png"

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
PAPER = (244, 242, 232)
DEVICE_COLORS = [
    ("K", (0, 0, 0)),
    ("W", (255, 255, 255)),
    ("Y", (255, 255, 0)),
    ("R", (255, 0, 0)),
    ("B", (0, 0, 255)),
    ("G", (0, 255, 0)),
]


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        "/System/Library/Fonts/SFNS.ttf" if not bold else "/System/Library/Fonts/SFNSBold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size=size)
        except OSError:
            continue
    return ImageFont.load_default()


FONT_10 = font(10)
FONT_11 = font(11)
FONT_13 = font(13, bold=True)
FONT_16 = font(16, bold=True)


def contrast_text(rgb: tuple[int, int, int]) -> tuple[int, int, int]:
    luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
    return BLACK if luminance > 145 else WHITE


def section_label(draw: ImageDraw.ImageDraw, y: int, label: str, note: str = "") -> int:
    draw.text((MARGIN, y), label, fill=BLACK, font=FONT_13)
    if note:
        note_width = draw.textlength(note, font=FONT_10)
        draw.text((WIDTH - MARGIN - note_width, y + 2), note, fill=(80, 80, 76), font=FONT_10)
    return y + 18


def mix(left: tuple[int, int, int], right: tuple[int, int, int], ratio: float) -> tuple[int, int, int]:
    return tuple(round(a * (1 - ratio) + b * ratio) for a, b in zip(left, right))


def draw_patch_row(
    draw: ImageDraw.ImageDraw,
    y: int,
    colors: list[tuple[int, int, int]],
    height: int,
    labels: list[str] | None = None,
) -> int:
    count = len(colors)
    for index, rgb in enumerate(colors):
        x0 = MARGIN + round(CONTENT_WIDTH * index / count)
        x1 = MARGIN + round(CONTENT_WIDTH * (index + 1) / count)
        draw.rectangle((x0, y, x1, y + height), fill=rgb)
        if labels:
            label = labels[index]
            text_width = draw.textlength(label, font=FONT_10)
            draw.text((x0 + (x1 - x0 - text_width) / 2, y + height - 14), label, fill=contrast_text(rgb), font=FONT_10)
    return y + height


def draw_ramp(
    draw: ImageDraw.ImageDraw,
    y: int,
    label: str,
    left: tuple[int, int, int],
    right: tuple[int, int, int],
    steps: int = 16,
) -> int:
    draw.text((MARGIN, y + 7), label, fill=BLACK, font=FONT_11)
    ramp_x = MARGIN + 46
    ramp_width = CONTENT_WIDTH - 46
    for index in range(steps):
        ratio = index / (steps - 1)
        x0 = ramp_x + round(ramp_width * index / steps)
        x1 = ramp_x + round(ramp_width * (index + 1) / steps)
        draw.rectangle((x0, y, x1, y + 28), fill=mix(left, right, ratio))
    return y + 31


def hsv_rgb(hue: float, saturation: float, value: float) -> tuple[int, int, int]:
    return tuple(round(channel * 255) for channel in colorsys.hsv_to_rgb(hue, saturation, value))


def main() -> None:
    image = Image.new("RGB", (WIDTH, HEIGHT), PAPER)
    draw = ImageDraw.Draw(image)

    draw.rectangle((0, 0, WIDTH, 38), fill=BLACK)
    draw.text((MARGIN, 9), "INKLOOP / FULL-GAMUT TEST 02", fill=WHITE, font=FONT_16)
    draw.text((WIDTH - 118, 13), "528 x 792 sRGB", fill=(205, 205, 200), font=FONT_10)

    y = 48
    y = section_label(draw, y, "A  SIX NATIVE TARGETS", "solid patches")
    patch_width = CONTENT_WIDTH // len(DEVICE_COLORS)
    for index, (label, rgb) in enumerate(DEVICE_COLORS):
        x0 = MARGIN + index * patch_width
        x1 = WIDTH - MARGIN if index == len(DEVICE_COLORS) - 1 else x0 + patch_width
        draw.rectangle((x0, y, x1, y + 62), fill=rgb)
        draw.text((x0 + 7, y + 6), label, fill=contrast_text(rgb), font=FONT_16)
        draw.text((x0 + 7, y + 43), ",".join(str(value) for value in rgb), fill=contrast_text(rgb), font=FONT_10)
    y += 72

    y = section_label(draw, y, "B  NEUTRAL SCALE", "sRGB 0-255")
    gray_steps = [round(255 * index / 10) for index in range(11)]
    y = draw_patch_row(
        draw,
        y,
        [(value, value, value) for value in gray_steps],
        42,
        [str(round(index * 100 / 10)) for index in range(11)],
    ) + 10

    y = section_label(draw, y, "C  PAIRWISE TRANSITIONS", "16 equal sRGB steps")
    for label, left, right in [
        ("R-Y", (255, 0, 0), (255, 255, 0)),
        ("Y-G", (255, 255, 0), (0, 255, 0)),
        ("G-B", (0, 255, 0), (0, 0, 255)),
        ("B-R", (0, 0, 255), (255, 0, 0)),
        ("K-W", (0, 0, 0), (255, 255, 255)),
    ]:
        y = draw_ramp(draw, y, label, left, right)
    y += 5

    y = section_label(draw, y, "D  HUE x SATURATION", "value = 100%")
    hue_columns = 12
    saturations = [1.0, 0.75, 0.5, 0.25]
    for saturation in saturations:
        colors = [hsv_rgb(index / hue_columns, saturation, 1.0) for index in range(hue_columns)]
        y = draw_patch_row(draw, y, colors, 34)
    y += 10

    y = section_label(draw, y, "E  HUE x LIGHTNESS", "saturation = 100%")
    for value in [1.0, 0.75, 0.5, 0.25]:
        colors = [hsv_rgb(index / hue_columns, 1.0, value) for index in range(hue_columns)]
        y = draw_patch_row(draw, y, colors, 34)
    y += 10

    y = section_label(draw, y, "F  DOT / EDGE CHECK", "1 2 4 8 px")
    box_height = min(64, HEIGHT - y - 24)
    box_width = CONTENT_WIDTH // 4
    for column, cell_size in enumerate([1, 2, 4, 8]):
        x0 = MARGIN + column * box_width
        x1 = WIDTH - MARGIN if column == 3 else x0 + box_width
        for py in range(y, y + box_height, cell_size):
            for px in range(x0, x1, cell_size):
                alternate = ((px - x0) // cell_size + (py - y) // cell_size) % 2
                draw.rectangle((px, py, min(px + cell_size - 1, x1), min(py + cell_size - 1, y + box_height)), fill=BLACK if alternate else WHITE)
        draw.rectangle((x0, y, x1, y + 14), fill=PAPER)
        draw.text((x0 + 5, y + 2), f"{cell_size}px", fill=BLACK, font=FONT_10)

    draw.text((MARGIN, HEIGHT - 16), "REFERENCE: ordinary sRGB input / render with the same path as a user image", fill=(70, 70, 66), font=FONT_10)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    image.save(OUTPUT, "PNG", optimize=True)
    print(OUTPUT)


if __name__ == "__main__":
    main()
