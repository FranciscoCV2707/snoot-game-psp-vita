"""Inventario, copia y manifiesto de recursos para un perfil de assetpipe."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from typing import Iterable

from PIL import Image

from .profiles import PROFILES

# Extensiones consideradas recurso, por tipo.
_KIND_BY_EXT: dict[str, str] = {
    # Imagenes
    ".png": "image",
    ".jpg": "image",
    ".jpeg": "image",
    ".webp": "image",
    ".gif": "image",
    ".bmp": "image",
    ".tif": "image",
    ".tiff": "image",
    ".ico": "image",
    ".svg": "image",
    # Audio
    ".ogg": "audio",
    ".wav": "audio",
    ".mp3": "audio",
    ".flac": "audio",
    ".opus": "audio",
    ".m4a": "audio",
    ".aac": "audio",
    # Video
    ".webm": "video",
    ".mp4": "video",
    ".avi": "video",
    ".mov": "video",
    ".mkv": "video",
    ".ogv": "video",
    ".m4v": "video",
    # Fuentes
    ".ttf": "font",
    ".otf": "font",
}

# Carpetas de cache que se ignoran siempre.
_CACHE_DIR_NAMES = {"__pycache__", "cache", ".cache", "renpy_cache"}

# Directorios de nivel superior del juego que se recorren como recursos.
_RESOURCE_ROOTS = {"images", "audio", "video", "movies"}


def _is_cached(parts: tuple[str, ...]) -> bool:
    return any(part.lower() in _CACHE_DIR_NAMES for part in parts)


def classify_kind(rel_path) -> str | None:
    """Devuelve el tipo de recurso para una ruta relativa, o None si no es recurso."""
    path = Path(rel_path)
    parts = path.parts
    if not parts or _is_cached(parts):
        return None
    ext = path.suffix.lower()
    if ext in (".rpy", ".rpyc"):
        return None
    if parts and parts[0].lower() in _RESOURCE_ROOTS:
        return _KIND_BY_EXT.get(ext)
    # Fuentes en cualquier carpeta.
    if ext in (".ttf", ".otf"):
        return "font"
    return None


def inventory(game_dir) -> list[Path]:
    """Lista ordenada de rutas relativas (Path) de recursos del juego."""
    game_dir = Path(game_dir)
    found: list[Path] = []
    for path in game_dir.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(game_dir)
        kind = classify_kind(rel)
        if kind is not None:
            found.append(rel)
    found.sort(key=lambda p: p.as_posix())
    return found


def sha256_file(path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def source_file(game_dir: Path, rel: Path, language: str | None) -> Path:
    """El archivo que Ren'Py usa con ese idioma: game/tl/<idioma>/<ruta> si existe (imagenes con texto
    traducido), si no game/<ruta>."""
    if language:
        translated = game_dir / "tl" / language / rel
        if translated.is_file():
            return translated
    return game_dir / rel


def copy_resources(game_dir, out_dir, resources: Iterable, language: str | None = None) -> None:
    """Copia los recursos al directorio de salida."""
    game_dir = Path(game_dir)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for rel in resources:
        rel = Path(rel)
        dst = out_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_file(game_dir, rel, language), dst)


def build_manifest(profile: str, game_dir, resources: Iterable, language: str | None = None) -> dict:
    """Construye el diccionario del manifiesto (sin escribirlo)."""
    game_dir = Path(game_dir)
    entries = []
    for rel in resources:
        rel = Path(rel)
        full = source_file(game_dir, rel, language)
        entries.append(
            {
                "path": rel.as_posix(),
                "kind": classify_kind(rel),
                "bytes": full.stat().st_size,
                "sha256": sha256_file(full),
            }
        )
    return {
        "profile": profile,
        "count": len(entries),
        "resources": entries,
    }


def _probe_duration(path) -> float:
    """Duracion en segundos del audio con ffprobe (formato=duration)."""
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
                         capture_output=True, text=True, check=True).stdout
    return float(out)


def convert_resources(profile: dict, game_dir: Path, out_dir: Path, resources: Iterable,
                      language: str | None = None) -> dict:
    """Reduce los recursos segun el perfil; devuelve el manifiesto con ``source`` por recurso y ``skipped``."""
    entries, skipped = [], []
    for rel in map(Path, resources):
        kind = classify_kind(rel)
        if kind in profile["skip_kinds"]:
            skipped.append({"source": rel.as_posix()})
            continue
        src = source_file(game_dir, rel, language)
        if kind == "audio":
            # D18: lo largo es musica (OGG Vorbis), lo corto efecto o UI (WAV PCM)
            kind = "music" if _probe_duration(src) > profile["music_threshold"] else "sound"
        out_rel = rel.with_suffix({"image": ".png", "music": ".ogg", "sound": ".wav"}.get(kind, rel.suffix))
        dst = out_dir / out_rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        ffmpeg = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src), "-map_metadata", "-1", "-bitexact"]
        if kind == "image":
            # thumbnail no agranda; sin info (metadatos) el PNG sale igual byte a byte
            with Image.open(src) as img:
                img.thumbnail(profile["image_size"], Image.LANCZOS)
                img.info.clear()
                img.save(dst, format="PNG")
        elif kind == "music":
            subprocess.run([*ffmpeg, "-ar", str(profile["music_rate"]), "-ac", str(profile["music_channels"]),
                            "-c:a", "libvorbis", "-q:a", str(profile["music_quality"]), str(dst)], check=True)
        elif kind == "sound":
            subprocess.run([*ffmpeg, "-ar", str(profile["audio_rate"]), "-ac", str(profile["audio_channels"]),
                            "-sample_fmt", "s16", str(dst)], check=True)
        else:
            shutil.copyfile(src, dst)
        entries.append({"path": out_rel.as_posix(), "source": rel.as_posix(), "kind": kind,
                        "bytes": dst.stat().st_size, "sha256": sha256_file(dst)})
    return {"profile": profile["name"], "count": len(entries), "resources": entries, "skipped": skipped}


def _is_out_inside_game(game_dir: Path, out_dir: Path) -> bool:
    try:
        out_dir.resolve().relative_to(game_dir.resolve())
        return True
    except ValueError:
        return False


def run(profile: str, game, out, language: str | None = None) -> dict:
    """Ejecuta el pipeline completo y devuelve el manifiesto."""
    if profile not in PROFILES:
        raise ValueError(
            f"perfil desconocido: {profile} (disponibles: {', '.join(sorted(PROFILES))})"
        )
    game_dir = Path(game)
    out_dir = Path(out)
    if not game_dir.is_dir():
        raise ValueError(f"no existe el directorio de juego: {game_dir}")
    if _is_out_inside_game(game_dir, out_dir):
        raise ValueError("--out no puede estar dentro de --game")
    resources = inventory(game_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    if language and not (game_dir / "tl" / language).is_dir():
        raise ValueError(f"el juego no trae la traduccion {language} (falta {game_dir / 'tl' / language})")
    if profile == "pc":
        copy_resources(game_dir, out_dir, resources, language)
        manifest = build_manifest(profile, game_dir, resources, language)
    else:
        manifest = convert_resources(PROFILES[profile], game_dir, out_dir, resources, language)
    if language:
        manifest["language"] = language
    manifest_path = out_dir / "manifest.json"
    with manifest_path.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2, sort_keys=True, ensure_ascii=False)
        f.write("\n")
    return manifest
