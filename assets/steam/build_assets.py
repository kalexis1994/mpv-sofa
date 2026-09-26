#!/usr/bin/env python3
"""Rebuild the Steam artwork from the approved MPV Sofa source art.

Requires Pillow. Steam screenshot files are captured separately from the real app.
"""

from pathlib import Path
from PIL import Image, ImageDraw, ImageEnhance, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / "export"
SOURCE = HERE / "source"
IVORY = (247, 243, 233)
DARK = (34, 29, 26)
RED = (255, 46, 77)
FONT = ROOT / "assets/fonts/Dosis-ExtraBold.ttf"
WIDE = Image.open(SOURCE / "key-art-wide.png").convert("RGB")
VERTICAL = Image.open(SOURCE / "key-art-vertical.png").convert("RGB")
MARK = Image.open(ROOT / "assets/icons/mpv-sofa-logo.png").convert("RGBA")


def cover(image, size, focus=(0.5, 0.5)):
    w, h = size
    scale = max(w / image.width, h / image.height)
    scaled = image.resize((round(image.width * scale), round(image.height * scale)), Image.Resampling.LANCZOS)
    x = round((scaled.width - w) * focus[0])
    y = round((scaled.height - h) * focus[1])
    return scaled.crop((x, y, x + w, y + h))


def save(image, name, mode="RGB"):
    OUT.mkdir(parents=True, exist_ok=True)
    if mode:
        image = image.convert(mode)
    image.save(OUT / name, optimize=True)


def fit_font(text, max_width, max_height):
    for size in range(max_height, 10, -1):
        font = ImageFont.truetype(str(FONT), size)
        box = ImageDraw.Draw(Image.new("RGB", (1, 1))).textbbox((0, 0), text, font=font)
        if box[2] - box[0] <= max_width and box[3] - box[1] <= max_height:
            return font
    raise ValueError("The MPV Sofa wordmark does not fit")


def wordmark(image, region, color=IVORY):
    x, y, w, h = region
    font = fit_font("MPV Sofa", w, h)
    draw = ImageDraw.Draw(image)
    box = draw.textbbox((0, 0), "MPV Sofa", font=font)
    tw, th = box[2] - box[0], box[3] - box[1]
    draw.text((x + (w - tw) // 2 - box[0], y + (h - th) // 2 - box[1]), "MPV Sofa", font=font, fill=color)


def add_mark(image, box):
    x, y, side = box
    logo = MARK.resize((side, side), Image.Resampling.LANCZOS)
    image.paste(logo, (x, y), logo)


def capsule(size, artwork_fraction, mark_fraction, file_name):
    w, h = size
    image = Image.new("RGB", size, DARK)
    if w > 500:
        art_x = round(w * artwork_fraction)
        # Use the reference-driven key art, but let it dissolve behind the product name.
        art = cover(WIDE, (w - art_x, h), focus=(0.48, 0.5))
        fade = Image.new("L", (w - art_x, h))
        fd = ImageDraw.Draw(fade)
        for xx in range(fade.width):
            alpha = round(255 * min(1, max(0, xx / max(1, fade.width * 0.32))))
            fd.line((xx, 0, xx, h), fill=alpha)
        image.paste(art, (art_x, 0), fade)
        # Fade the left edge to the charcoal brand ground for guaranteed contrast.
        panel = Image.new("RGB", size, DARK)
        panel_alpha = Image.new("L", size)
        pd = ImageDraw.Draw(panel_alpha)
        for xx in range(w):
            alpha = round(242 * (1 - min(1, max(0, (xx - w * 0.26) / (w * 0.49)))))
            pd.line((xx, 0, xx, h), fill=alpha)
        image.paste(panel, (0, 0), panel_alpha)
    icon = round(h * mark_fraction)
    left = round(w * 0.055)
    add_mark(image, (left, (h - icon) // 2, icon))
    tx = left + icon + round(w * 0.012)
    name_max_x = round(w * (0.54 if w > 500 else 0.97))
    wordmark(image, (tx, round(h * 0.27), name_max_x - tx, round(h * 0.45)))
    save(image, file_name)


def portrait(size, file_name):
    w, h = size
    image = cover(VERTICAL, size, focus=(0.5, 0.42))
    # Keep lettering clear on both portrait aspect ratios, including Steam's wide vertical capsule.
    overlay = Image.new("RGBA", size, (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    for yy in range(round(h * 0.56), h):
        t = (yy / h - 0.56) / 0.44
        od.line((0, yy, w, yy), fill=(*DARK, round(205 * t)))
    image = Image.alpha_composite(image.convert("RGBA"), overlay).convert("RGB")
    wordmark(image, (round(w * 0.06), round(h * 0.735), round(w * 0.88), round(h * 0.105)))
    save(image, file_name)


def library_logo():
    image = Image.new("RGBA", (1280, 360), (0, 0, 0, 0))
    add_mark(image, (28, 24, 312))
    wordmark(image, (360, 79, 890, 194))
    save(image, "library_logo.png", "RGBA")


def page_background():
    # Optional artwork must stay subdued beneath Steam's foreground content.
    image = cover(WIDE, (1438, 810), focus=(0.5, 0.5))
    image = ImageEnhance.Brightness(image).enhance(0.20)
    image = ImageEnhance.Color(image).enhance(0.55)
    save(image, "page_background.png")


def icons():
    background = Image.new("RGB", (184, 184), IVORY)
    background.paste(MARK.resize((184, 184), Image.Resampling.LANCZOS), (0, 0), MARK.resize((184, 184), Image.Resampling.LANCZOS))
    background.save(OUT / "app_icon.jpg", quality=95, subsampling=0)
    square = Image.new("RGBA", (256, 256), DARK + (255,))
    mark = MARK.resize((250, 250), Image.Resampling.LANCZOS)
    square.paste(mark, (3, 3), mark)
    save(square, "shortcut_icon.png", "RGBA")


def main():
    capsule((920, 430), 0.35, 0.35, "store_header.png")
    capsule((462, 174), 0.62, 0.75, "store_small.png")
    capsule((1232, 706), 0.29, 0.30, "store_main.png")
    portrait((748, 896), "store_vertical.png")
    capsule((920, 430), 0.35, 0.35, "library_header.png")
    portrait((600, 900), "library_capsule.png")
    save(cover(WIDE, (3840, 1240), focus=(0.5, 0.5)), "library_hero.png")
    library_logo()
    page_background()
    icons()


if __name__ == "__main__":
    main()
