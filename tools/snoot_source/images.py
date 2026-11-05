"""Imágenes Vita renderizadas desde las recetas y assets del fuente Ren'Py."""
import math
import struct
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import lz4.block
import numpy as np
from PIL import Image

from . import render
from .config import PROFILE

HEADER = "<4sBBHHHHHH4BH"
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.int16) - 8


def encode(image: Image.Image, kind: int, full: tuple[int, int], offset=(0, 0)) -> bytes:
    rgba = np.asarray(image.convert("RGBA")).astype(np.int16)
    h, w = rgba.shape[:2]
    if w % 2:
        rgba = np.pad(rgba, ((0, 0), (0, 1), (0, 0)))
        w += 1
    x, y = offset
    noise = BAYER[(np.arange(h)[:, None] + y) % 4, (np.arange(w)[None, :] + x) % 4]
    rgba = np.clip(rgba + noise[:, :, None] // 2, 0, 255).astype(np.uint16)
    if kind == 0:
        pixels = ((rgba[:, :, 0] >> 3) << 11) | ((rgba[:, :, 1] >> 2) << 5) | (rgba[:, :, 2] >> 3)
        fmt = 1
    else:
        pixels = ((rgba[:, :, 0] >> 4) << 12) | ((rgba[:, :, 1] >> 4) << 8)
        pixels |= ((rgba[:, :, 2] >> 4) << 4) | (rgba[:, :, 3] >> 4)
        fmt = 2
    raw = pixels.astype("<u2").tobytes()
    compressed = lz4.block.compress(raw, mode="high_compression", compression=9, store_size=False)
    return struct.pack(HEADER, b"IMG1", kind, fmt << 3, *full, x, y, w, h, 0, 0, 0, 0, 0) + struct.pack(
        "<II", len(raw), len(compressed)) + compressed


def convert(task) -> dict:
    ident, entry, directory = task
    if entry["recipe"]["op"] == "solid":
        color = render.parse_color(entry["recipe"]["color"])
        if color[3] in (0, 255):
            kind = 2 if color[3] else 3
            blob = struct.pack(HEADER, b"IMG1", kind, 1 << 3, *PROFILE.view, 0, 0, 0, 0, *color, 0) + bytes(8)
            (Path(directory) / f"I{ident:04d}.IMG").write_bytes(blob)
            return {"id": ident, "name": entry["names"][0], "kind": kind, "size": PROFILE.view, "bytes": len(blob)}
    image = render.render(entry["recipe"])
    ow, oh = image.size
    scale = 1920 / PROFILE.view[0]
    full = (max(1, round(ow / scale)), max(1, round(oh / scale)))
    opaque = float((np.asarray(image.getchannel("A")) >= 250).mean()) >= 0.97
    kind = 0 if opaque and ow >= 1000 else 1
    if kind == 0 and full[0] == PROFILE.view[0] and 0 < PROFILE.view[1] - full[1] <= 8:
        full = (full[0], PROFILE.view[1])
    if kind == 0:
        flat = Image.new("RGBA", image.size, (0, 0, 0, 255))
        flat.alpha_composite(image)
        image = flat
    image = image.convert("RGBa").resize(full, Image.Resampling.LANCZOS).convert("RGBA")
    ox = oy = 0
    if kind == 1 and entry.get("visible"):
        visible = entry["visible"]
        box = (max(0, math.floor(visible[0] / scale)), max(0, math.floor(visible[1] / scale)),
               min(full[0], math.ceil(visible[2] / scale)), min(full[1], math.ceil(visible[3] / scale)))
        if box[2] > box[0] and box[3] > box[1]:
            image = image.crop(box)
            ox, oy = box[:2]
    if kind == 1:
        box = image.getchannel("A").getbbox()
        if box:
            image = image.crop(box)
            ox += box[0]
            oy += box[1]
    blob = encode(image, kind, full, (ox, oy))
    path = Path(directory) / f"I{ident:04d}.IMG"
    path.write_bytes(blob)
    return {"id": ident, "name": entry["names"][0], "kind": kind, "size": image.size, "bytes": len(blob)}


def build(meta: dict, output: Path, table) -> list[dict]:
    output.mkdir(parents=True, exist_ok=True)
    tasks = [(i, dict(entry, recipe=render.expand(table, entry["recipe"])), str(output))
             for i, entry in enumerate(meta["images"]) if entry is not None]
    with ProcessPoolExecutor(max_workers=4) as pool:
        return list(pool.map(convert, tasks))
