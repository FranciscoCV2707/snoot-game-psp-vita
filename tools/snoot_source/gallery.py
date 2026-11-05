"""Galería Vita: categorías, cubiertas y miniaturas desde los recursos originales."""
from concurrent.futures import ProcessPoolExecutor
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from PIL import Image

from .config import GAME, PROFILE
from .images import encode
from . import render

LABELS = {"CG": "CG", "Animations": "Animaciones", "Lewd": "Adicionales",
          "Fullbody": "Personajes", "Backgrounds": "Fondos"}
SCALE = PROFILE.view[0] / 480
VIEW = PROFILE.view
MOVIE_SIZE = (VIEW[0], round(VIEW[0] * 9 / 16))
THUMB = (round(108 * SCALE), round(61 * SCALE))
MOVIE_FPS = Fraction(24000, 1001)


def movie_frame_count(duration: float) -> int:
    return max(1, round(duration * MOVIE_FPS))


def movie_frame_lengths(count: int) -> list[int]:
    return [
        round(Fraction((index + 1) * 60, 1) / MOVIE_FPS)
        - round(Fraction(index * 60, 1) / MOVIE_FPS)
        for index in range(count)
    ]


def extract_movie(path: str, item: int, duration: float, output: Path) -> list[str]:
    """Prepara cuadros de vídeo para el runtime Vita, que recibe imágenes ya decodificadas."""
    ffmpeg = shutil.which("ffmpeg") or str(Path.home() / "AppData/Local/Microsoft/WinGet/Links/ffmpeg.exe")
    count = movie_frame_count(duration)
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(GAME / path), "-an", "-vf",
               f"fps={MOVIE_FPS.numerator}/{MOVIE_FPS.denominator},scale={MOVIE_SIZE[0]}:{MOVIE_SIZE[1]}:flags=lanczos",
               "-frames:v", str(count), "-pix_fmt", "rgba",
               "-f", "rawvideo", "-"]
    raw = subprocess.run(command, capture_output=True, check=True).stdout
    frame_bytes = MOVIE_SIZE[0] * MOVIE_SIZE[1] * 4
    if len(raw) != count * frame_bytes:
        raise ValueError(f"Video {path}: se esperaban {count} cuadros y llegaron {len(raw) // frame_bytes}")
    names = []
    for frame_index in range(count):
        top = frame_index * frame_bytes
        image = Image.new("RGBA", VIEW, (0, 0, 0, 255))
        image.paste(Image.frombytes("RGBA", MOVIE_SIZE, raw[top:top + frame_bytes]),
                    (0, VIEW[1] - MOVIE_SIZE[1]))
        name = f"GM{item:03d}_{frame_index:03d}.IMG"
        (output / name).write_bytes(encode(image, 0, image.size))
        names.append(name)
    return names


def fit(source: Image.Image, size: tuple[int, int]) -> Image.Image:
    image = source.convert("RGBA")
    image.thumbnail(size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", size, (0, 0, 0, 255))
    canvas.alpha_composite(image, ((size[0] - image.width) // 2, (size[1] - image.height) // 2))
    return canvas


def make_view(task):
    index, recipe, directory = task
    source = render.render(recipe)
    view = Image.new("RGBA", VIEW, (0, 0, 0, 255))
    view.alpha_composite(fit(source, (VIEW[0], VIEW[1] - round(24 * SCALE))), (0, 0))
    (Path(directory) / f"GV{index:03d}.IMG").write_bytes(encode(view, 0, view.size))
    return index, fit(source, THUMB)


def build(meta: dict, output: Path, table, reuse=False) -> dict:
    entries = meta["gallery"]
    dataset = json.loads((GAME / "src/gallery_dataset.json").read_text(encoding="utf-8"))
    tabs = [{"source": tab["tab_name"], "label": LABELS[tab["tab_name"]],
             "entries": [i for i, entry in enumerate(entries) if entry["tab"] == tab["tab_name"]]}
            for tab in dataset["tabs"]]
    tabs = [tab for tab in tabs if tab["entries"]]
    movies = [(i, entry["movie"]) for i, entry in enumerate(entries) if entry.get("movie")]
    expected = [f"GV{i:03d}.IMG" for i in range(len(entries))] + ["GLOCK.IMG"]
    expected += [f"G{t:02d}{p:02d}.IMG" for t, tab in enumerate(tabs)
                 for p in range((len(tab["entries"]) + 11) // 12)]
    for item, movie in movies:
        expected += [
            f"GM{item:03d}_{frame:03d}.IMG"
            for frame in range(movie_frame_count(sum(movie["times"])))
        ]
    sources = [GAME / "gui/main_menu.png", GAME / "gui/game_menu.png",
               GAME / "gui/gallery/unlocked_cg_button_cover.png", GAME / "src/gallery_dataset.json"]
    sources += [GAME / movie["path"] for _, movie in movies]
    manifest = {"tabs": tabs, "entries": len(entries), "columns": 4, "rows": 3,
                "profile": PROFILE.name, "view": VIEW,
                "animations": [{"entry": i, "movie": entry["movie"], "fps": float(MOVIE_FPS)}
                               for i, entry in enumerate(entries)
                               if entry.get("movie")],
                "sources": {str(p.relative_to(GAME)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                "files": expected}
    previous = output / f"{PROFILE.name.upper()}_GALLERY.json"
    if not (reuse and previous.exists() and json.loads(previous.read_text(encoding="utf-8")) == manifest
            and all((output / name).is_file() for name in expected)):
        tasks = []
        for i, entry in enumerate(entries):
            ident = entry.get("thumb", entry["id"])
            tasks.append((i, render.expand(table, meta["images"][ident]["recipe"]), str(output)))
        with ProcessPoolExecutor(max_workers=4) as pool:
            thumbs = dict(pool.map(make_view, tasks))
        with Image.open(sources[0]) as image:
            base = image.convert("RGBA").resize(VIEW, Image.Resampling.LANCZOS)
        with Image.open(sources[1]) as image:
            base.alpha_composite(image.convert("RGBA").resize(base.size, Image.Resampling.LANCZOS))
        for t, tab in enumerate(tabs):
            for page in range((len(tab["entries"]) + 11) // 12):
                frame = base.copy()
                for cell, ident in enumerate(tab["entries"][page * 12:(page + 1) * 12]):
                    frame.alpha_composite(thumbs[ident],
                                          (round(12 * SCALE) + cell % 4 * round(116 * SCALE),
                                           round(48 * SCALE) + cell // 4 * round(69 * SCALE)))
                (output / f"G{t:02d}{page:02d}.IMG").write_bytes(encode(frame, 0, frame.size))
        with Image.open(sources[2]) as source:
            cover = source.convert("RGBA").resize(THUMB, Image.Resampling.LANCZOS)
        (output / "GLOCK.IMG").write_bytes(encode(cover, 1, cover.size))
        for item, movie in movies:
            extract_movie(movie["path"], item, sum(movie["times"]), output)
    data = bytearray(b"SGP2" + struct.pack("<HH", len(tabs), len(entries)))
    for tab in tabs:
        name = tab["label"].encode("utf-8")
        data += bytes([len(name)]) + name + struct.pack("<H", len(tab["entries"]))
        data += struct.pack(f"<{len(tab['entries'])}H", *tab["entries"])
    data += b"IMGS" + struct.pack("<H", len(entries))
    data += struct.pack(f"<{len(entries)}H", *[entry.get("thumb", entry["id"]) for entry in entries])
    data += b"ANIM" + struct.pack("<H", len(movies))
    for item, movie in movies:
        if len(movie["patches"]) != len(movie["times"]):
            raise ValueError(f"Animacion de galeria invalida: {entries[item]['name']}")
        frame_count = movie_frame_count(sum(movie["times"]))
        lengths = movie_frame_lengths(frame_count)
        data += struct.pack("<HH", item, frame_count)
        data += struct.pack(f"<{frame_count}H", *lengths)
    (output / "GALLERY.BIN").write_bytes(data)
    for path in output.glob("G????.IMG"):
        if path.stem[1:].isdigit() and path.name not in expected:
            path.unlink()
    previous.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest
