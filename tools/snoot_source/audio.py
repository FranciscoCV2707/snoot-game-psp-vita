"""Audio Vita convertido desde los recursos del juego original."""
import hashlib
import json
import re
import shutil
import struct
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .config import GAME
from tools.assetpipe.profiles import PROFILES


def _duration(path: Path) -> float:
    result = subprocess.run([
        "ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1",
        str(path),
    ], check=True, capture_output=True, text=True, encoding="utf-8")
    return float(result.stdout.strip())


def _uses_pcm(entry: dict, paths: list[Path]) -> bool:
    source_names = entry.get("mix", [entry["path"]])
    short_effect = all(path.startswith(("audio/effects/", "audio/ui/")) for path in source_names)
    return short_effect and max(_duration(path) for path in paths) <= 10.0


def convert(task) -> dict:
    ident, entry, output, previous, profile_name = task
    profile = PROFILES[profile_name]
    paths = [GAME / p for p in entry.get("mix", [entry["path"]])]
    for path in paths:
        if not path.is_file():
            raise FileNotFoundError(path)
    pcm = _uses_pcm(entry, paths)
    suffix = "PCM" if pcm else "OGG"
    target = Path(output) / f"A{ident:04d}.{suffix}"
    source_hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]
    profile_key = (f"{profile_name}-{profile['audio_rate']}-mono-s16le" if pcm else
                   f"{profile_name}-{profile['music_rate']}-stereo-q{profile['music_quality']}")
    if (previous and previous.get("profile") == profile_key and target.exists()
            and previous["source"] == entry["path"] and previous["source_sha256"] == source_hashes
            and previous["sha256"] == hashlib.sha256(target.read_bytes()).hexdigest()):
        cached = dict(previous)
        cached["codec"] = "pcm_s16le_mono" if pcm else "vorbis_stereo"
        return cached
    copied = False
    if pcm:
        command = ["ffmpeg", "-v", "error", "-y"]
        for path in paths:
            command += ["-i", str(path)]
        if len(paths) > 1:
            command += ["-filter_complex", f"amix=inputs={len(paths)}:normalize=0"]
        command += ["-ar", str(profile["audio_rate"]), "-ac", "1", "-c:a", "pcm_s16le", "-f", "s16le", str(target)]
        subprocess.run(command, check=True, capture_output=True)
    else:
        probe = json.loads(subprocess.run([
            "ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=sample_rate,channels",
            "-of", "json", str(paths[0]),
        ], check=True, capture_output=True, text=True, encoding="utf-8").stdout)["streams"][0]
        copied = (len(paths) == 1 and paths[0].suffix.lower() == ".ogg"
                  and probe == {"sample_rate": "44100", "channels": 2})
        if copied:
            shutil.copy2(paths[0], target)
        else:
            command = ["ffmpeg", "-v", "error", "-y"]
            for path in paths:
                command += ["-i", str(path)]
            if len(paths) > 1:
                command += ["-filter_complex", f"amix=inputs={len(paths)}:normalize=0"]
            command += ["-ar", str(profile["music_rate"]), "-ac", "2", "-c:a", "libvorbis",
                        "-q:a", str(profile["music_quality"]), str(target)]
            subprocess.run(command, check=True, capture_output=True)
    return {"id": ident, "source": entry["path"], "file": target.name, "copied_original": copied,
            "codec": "pcm_s16le_mono" if pcm else "vorbis_stereo", "profile": profile_key,
            "source_sha256": source_hashes,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "bytes": target.stat().st_size}


def build(meta: dict, output: Path, profile_name: str = "vita") -> list[dict]:
    if profile_name != "vita":
        raise ValueError(f"perfil de audio fuente no soportado: {profile_name}")
    manifest = output / "AUDIO_SOURCE.json"
    previous = {entry["id"]: entry for entry in json.loads(manifest.read_text(encoding="utf-8"))} if manifest.exists() else {}
    with ThreadPoolExecutor(max_workers=2) as pool:
        entries = list(pool.map(convert, [(i, e, str(output), previous.get(i), profile_name) for i, e in enumerate(meta["audio"])]))
    for entry in entries:
        obsolete_suffix = "OGG" if entry["codec"] == "pcm_s16le_mono" else "PCM"
        (output / f"A{entry['id']:04d}.{obsolete_suffix}").unlink(missing_ok=True)
    flags = bytes((0 if re.search(r"[ _-](intro|end)$", Path(e["path"]).stem, re.I) else 1)
                  | (2 if entry["codec"] == "pcm_s16le_mono" else 0)
                  for entry, e in zip(entries, meta["audio"], strict=True))
    title = next(i for i, e in enumerate(meta["audio"]) if e["path"].endswith("/Beach Chill Out.ogg"))
    click = next(i for i, e in enumerate(meta["audio"]) if e["path"].endswith("/uiClick.wav"))
    back = next(i for i, e in enumerate(meta["audio"]) if e["path"].endswith("/uiBack.wav"))
    (output / "NATIVE.AUD").write_bytes(b"VNA1" + struct.pack("<HHHH", len(entries), title, click, back) + flags)
    (output / "AUDIO_SOURCE.json").write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return entries
