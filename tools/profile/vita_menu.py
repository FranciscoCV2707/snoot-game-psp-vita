"""Compone las páginas del menú Vita con el arte y la traducción del juego original."""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
SIZES = {"vita": (960, 544)}
HEADER = "<4sBBHHHHHH4BH"
MAIN = ("Start", "Continue", "Bonus Chapters", "Gallery", "Options")


def translations(game: Path) -> dict[str, str]:
    result = {}
    old = None
    for line in (game / "tl/es/screens.rpy").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("old "):
            old = ast.literal_eval(line[4:])
        elif line.startswith("new ") and old is not None:
            result[old] = ast.literal_eval(line[4:])
            old = None
    return result


def encode(image: Image.Image) -> bytes:
    """RGB565 con tramado ordenado; mismo contenedor del runtime, sin paleta de PS1."""
    rgb = np.asarray(image.convert("RGB")).astype(np.int16)
    # Bayer centrado: evita bandas al reducir el fondo del original a 5/6/5 bits.
    matrix = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]])
    h, w = rgb.shape[:2]
    field = matrix[np.arange(h)[:, None] % 4, np.arange(w)[None, :] % 4] - 8
    rgb = np.clip(rgb + field[:, :, None] // 2, 0, 255).astype(np.uint16)
    # IMG1 guarda RGB565 canónico; img_upload_true convierte a BGR565 solo
    # cuando sube la textura al renderer Vita.
    pixels = ((rgb[:, :, 0] >> 3) << 11) | ((rgb[:, :, 1] >> 2) << 5) | (rgb[:, :, 2] >> 3)
    raw = pixels.astype("<u2").tobytes()
    # Un bloque LZ4 de literales evita añadir una dependencia de build. Una sola
    # página residente; el tamaño adicional está en almacenamiento, no en RAM.
    extra = len(raw) - 15
    packed = b"\xf0" + b"\xff" * (extra // 255) + bytes([extra % 255]) + raw
    header = struct.pack(HEADER, b"IMG1", 0, 1 << 3, w, h, 0, 0, w, h, 0, 0, 0, 0, 0)
    return header + struct.pack("<II", len(raw), len(packed)) + packed


def build(game: Path, output: Path, preview: Path, profile: str = "vita") -> dict:
    game = game.resolve()
    if profile not in SIZES:
        raise ValueError(f"perfil de menú no soportado: {profile}")
    size = SIZES[profile]
    scale = size[0] / 480

    def px(value: int) -> int:
        return round(value * scale)

    output.mkdir(parents=True, exist_ok=True)
    preview.mkdir(parents=True, exist_ok=True)
    used = [game / p for p in (
        "gui/main_menu.png", "gui/overlay/main_menu.png", "gui/snootgame.png",
        "gui/button/menubuttons/template_idle.png",
        "gui/button/menubuttons/template_idle_epilogue.png", "images/cgs/big ending.webp",
        "gui/fonts/FallingSky.otf", "tl/es/screens.rpy", "gui.rpy",
    )]
    for path in used:
        if not path.is_file():
            raise FileNotFoundError(path)
    tr = translations(game)
    colors = {}
    for line in (game / "gui.rpy").read_text(encoding="utf-8").splitlines():
        for key in ("hover_color", "accent_color"):
            if line.startswith(f"define gui.{key} = "):
                colors[key] = ast.literal_eval(line.split("=", 1)[1].strip())
    font_path = game / "gui/fonts/FallingSky.otf"
    font = ImageFont.truetype(str(font_path), px(15))
    vita_badge_font = ImageFont.truetype(str(font_path), px(8)) if profile == "vita" else None
    vita_hint_font = ImageFont.truetype(str(font_path), px(7)) if profile == "vita" else None

    def background(path: Path) -> Image.Image:
        image = Image.open(path).convert("RGBA")
        content_size = (size[0], round(size[0] * 9 / 16))
        image = image.resize(content_size, Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", size)
        canvas.alpha_composite(image, (0, (size[1] - content_size[1]) // 2))
        return canvas

    base = background(game / "gui/main_menu.png")
    main_overlay = Image.open(game / "gui/overlay/main_menu.png").convert("RGBA")
    main_overlay = main_overlay.resize(base.size, Image.Resampling.LANCZOS)
    main_menu = base.copy()
    main_menu.alpha_composite(main_overlay)
    logo = Image.open(game / "gui/snootgame.png").convert("RGBA")
    logo.thumbnail((px(137), px(62)), Image.Resampling.LANCZOS)
    logo_pos = (px(331) + (px(143) - logo.width) // 2, px(8))
    base.alpha_composite(logo, logo_pos)
    main_menu.alpha_composite(logo, logo_pos)
    button = Image.open(game / "gui/button/menubuttons/template_idle.png").convert("RGBA")
    button = button.resize((px(125), px(32)), Image.Resampling.LANCZOS)
    epilogue_button = Image.open(game / "gui/button/menubuttons/template_idle_epilogue.png").convert("RGBA")
    epilogue_button = epilogue_button.resize((px(125), px(32)), Image.Resampling.LANCZOS)
    epilogue_base = background(game / "images/cgs/big ending.webp")
    epilogue_base.alpha_composite(logo, logo_pos)
    files = []

    def emit(name: str, image: Image.Image) -> None:
        blob = encode(image)
        (output / f"{name}.IMG").write_bytes(blob)
        image.convert("RGB").save(preview / f"{name}.png")
        files.append({"name": f"{name}.IMG", "bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()})

    def buttons(labels: tuple[str, ...], cursor: int, background: Image.Image = main_menu,
                template: Image.Image = button) -> Image.Image:
        frame = background.copy()
        draw = ImageDraw.Draw(frame)
        top = px(82)
        for row, key in enumerate(labels):
            y = top + row * px(33)
            frame.alpha_composite(template, (px(340), y))
            label = "Continuar" if key == "Continue" else tr[key]
            if key == "Bonus Chapters":
                label = "Capítulos extra"
            draw.text((px(402), y + px(15)), label, font=font, anchor="mm",
                      fill=colors["hover_color"] if row == cursor else "#ffffff", stroke_width=1, stroke_fill="#000000")
        if profile == "vita":
            # Identidad Vita pequeña, en el espacio inferior libre del menú original.
            # La pista anuncia el tacto y los controles sin sustituir la navegación clásica.
            badge = Image.new("RGBA", frame.size, (0, 0, 0, 0))
            badge_draw = ImageDraw.Draw(badge)
            x, y, w, h = px(326), px(248), px(148), px(21)
            badge_draw.rounded_rectangle((x, y, x + w, y + h), radius=px(5),
                                         fill=(18, 31, 67, 224), outline=(96, 224, 246, 235), width=max(1, px(1)))
            badge_draw.rounded_rectangle((x + px(5), y + px(5), x + px(7), y + h - px(5)),
                                         radius=px(1), fill=(96, 224, 246, 255))
            badge_draw.text((x + px(12), y + px(2)), "VITA EDITION", font=vita_badge_font,
                            fill=(145, 239, 255, 255))
            badge_draw.text((x + px(12), y + px(11)), "TACTIL + CONTROLES", font=vita_hint_font,
                            fill=(240, 244, 255, 255))
            frame.alpha_composite(badge)
        return frame

    for row in range(len(MAIN)):
        emit(f"MENU{row}", buttons(MAIN, row))
        emit(f"MENU{row}E", buttons(MAIN, row, epilogue_base, epilogue_button))
    # Opciones usa controles vivos del motor sobre el mismo arte del original.
    frame = base.copy()
    panel = Image.new("RGBA", size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(panel)
    draw.rounded_rectangle((px(28), px(8), px(310), px(264)), radius=px(8), fill=(0, 0, 0, 220))
    frame = Image.alpha_composite(frame, panel)
    emit("OPTIONS", frame)
    manifest = {
        "source": str(game), "language": "es", "lewd": True, "size": size,
        "format": "RGB565", "resident_pages": 1,
        "texture_storage_upper_bytes": size[0] * size[1] * 4,
        "sources": {str(p.relative_to(game)): hashlib.sha256(p.read_bytes()).hexdigest() for p in used},
        "files": files,
    }
    (output / f"{profile.upper()}_MENU.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    for old in ("MENU5", "EXTRA0", "EXTRA1", "EXTRA2", "EXTRA3", "HELP", "ABOUT"):
        (output / f"{old}.IMG").unlink(missing_ok=True)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=Path(r"E:/snoot/snootgame/game"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/vita-source/DATA")
    parser.add_argument("--preview", type=Path, default=ROOT / "build/vita-source/menu-preview")
    args = parser.parse_args()
    result = build(args.game, args.output, args.preview)
    print(f"Menú Vita: {len(result['files'])} páginas, español, arte original; {args.preview}")


if __name__ == "__main__":
    main()
