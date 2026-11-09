"""Generate Chrome Web Store screenshots and promotional images.

Run with:
    uv run --with pillow python assets/store/make.py
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent


def screenshot(source: str, target: str, *, crop_top: bool = False) -> None:
    image = Image.open(ROOT / source).convert("RGB")
    target_ratio = 1280 / 800
    ratio = image.width / image.height
    if ratio > target_ratio:
        width = round(image.height * target_ratio)
        left = (image.width - width) // 2
        image = image.crop((left, 0, left + width, image.height))
    elif ratio < target_ratio:
        height = round(image.width / target_ratio)
        top = 0 if crop_top else (image.height - height) // 2
        image = image.crop((0, top, image.width, top + height))
    image.resize((1280, 800), Image.Resampling.LANCZOS).save(OUT / target, optimize=True)


def promo(size: tuple[int, int], target: str) -> None:
    width, height = size
    overview = Image.open(ROOT / "assets/screenshots/overview.png").convert("RGB")
    background = overview.resize(size, Image.Resampling.LANCZOS).filter(ImageFilter.GaussianBlur(width / 90))
    background = Image.alpha_composite(
        background.convert("RGBA"),
        Image.new("RGBA", size, (12, 20, 29, 190)),
    )

    # Use the existing brand mark only. The blurred product screenshot supplies context without
    # competing with the logo or introducing a second, invented visual language.
    logo = Image.open(ROOT / "assets/logo/logo-256.png").convert("RGBA")
    logo_size = round(height * 0.68)
    logo = logo.resize((logo_size, logo_size), Image.Resampling.LANCZOS)
    logo_x = (width - logo_size) // 2
    logo_y = (height - logo_size) // 2

    shadow = Image.new("RGBA", size, (0, 0, 0, 0))
    shadow_draw = ImageDraw.Draw(shadow)
    shadow_margin = round(logo_size * 0.04)
    shadow_draw.rounded_rectangle(
        (
            logo_x + shadow_margin,
            logo_y + shadow_margin * 2,
            logo_x + logo_size - shadow_margin,
            logo_y + logo_size,
        ),
        radius=round(logo_size * 0.18),
        fill=(0, 0, 0, 110),
    )
    background = Image.alpha_composite(background, shadow.filter(ImageFilter.GaussianBlur(logo_size / 18)))
    background.alpha_composite(logo, (logo_x, logo_y))
    background.convert("RGB").save(OUT / target, optimize=True)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    screenshot("assets/screenshots/overview.png", "screenshot-overview-1280x800.png")
    screenshot("assets/screenshots/settings-light.png", "screenshot-settings-1280x800.png", crop_top=True)
    promo((440, 280), "promo-small-440x280.png")
    promo((1400, 560), "promo-marquee-1400x560.png")


if __name__ == "__main__":
    main()
