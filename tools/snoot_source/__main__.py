"""Genera los datos portatiles directamente desde Snoot Game, sin abrir artefactos de otros ports."""
import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path


def background_sources(meta: dict) -> dict[int, str]:
    """Map generated camera stills to their single original background source."""

    def files(recipe):
        if isinstance(recipe, dict):
            if recipe.get("op") == "file" and isinstance(recipe.get("path"), str):
                yield recipe["path"]
            for value in recipe.values():
                yield from files(value)
        elif isinstance(recipe, list):
            for value in recipe:
                yield from files(value)

    sources = {}
    for image_id, entry in enumerate(meta["images"]):
        if entry is None or not any(name.startswith("camera shot ") for name in entry.get("names", [])):
            continue
        paths = set(files(entry.get("recipe")))
        if len(paths) != 1:
            continue
        path = next(iter(paths))
        if path.startswith("images/backgrounds/"):
            sources[image_id] = Path(path).stem
    return sources


def hash_source_tree(root: Path) -> dict[str, str]:
    root = root.resolve(strict=True)
    files = sorted(path for path in root.rglob("*") if path.is_file())
    if any(path.is_symlink() or not path.resolve(strict=True).is_relative_to(root) for path in files):
        raise ValueError(f"El árbol fuente contiene archivos externos o enlaces: {root}")
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=Path(r"E:/snoot/snootgame/game"))
    parser.add_argument("--out", type=Path, default=Path("build/vita-source"))
    parser.add_argument("--skip-images", action="store_true", help="reutilizar imágenes de una compilación propia anterior")
    parser.add_argument("--profile", choices=("vita",), default="vita",
                        help="genera recursos a 960x544 para PS Vita")
    parser.add_argument("--audio-only", action="store_true", help="generar solo NATIVE.AUD y sus pistas; requiere el fuente original")
    args = parser.parse_args()
    source_root = args.game.resolve(strict=True)
    source_file_hashes = hash_source_tree(source_root)
    os.environ["SNOOT_SOURCE"] = str(source_root)
    os.environ["SNOOT_BUILD"] = str(args.out.resolve() / "compiled")
    os.environ["SNOOT_PROFILE"] = args.profile
    from . import audio, font, gallery, images, tables
    from .compiler.compile import compile_all
    from .vnb import Program

    work = args.out.resolve() / "compiled"
    output = args.out.resolve() / "DATA"
    output.mkdir(parents=True, exist_ok=True)
    project, _, result = compile_all(work)
    for warning in result.warnings:
        print(f"Aviso del compilador: {warning}", flush=True)
    if result.errors:
        raise RuntimeError("\n".join(result.errors))
    program = Program(work)
    meta = program.meta
    if args.audio_only:
        output = args.out.resolve() / "DATA"
        output.mkdir(parents=True, exist_ok=True)
        audio_entries = audio.build(meta, output, args.profile)
        pcm_count = sum(e["codec"] == "pcm_s16le_mono" for e in audio_entries)
        print(f"Audio {args.profile}: {len(audio_entries) - pcm_count} OGG y {pcm_count} PCM; {output}", flush=True)
        return
    bases = {chapter["index"]: chapter["text_base"] for chapter in meta["chapters"]}
    resource_map = {
        "text": {bases[i] + j: entry["id"] for i, ir in result.irs.items() for j, entry in enumerate(ir.strings)},
        "speakers": {i: entry["name"] for i, entry in enumerate(meta["speakers"])},
        "images": {i: entry["names"] for i, entry in enumerate(meta["images"]) if entry is not None},
        "background_sources": background_sources(meta),
        "tracks": {i: entry["path"] for i, entry in enumerate(meta["audio"])},
        "tags": meta["tags"],
    }
    (work / "snoot-map.json").write_text(json.dumps(resource_map, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Fuente: {args.game}; {len(program.chapters)} unidades; español; lewd=True", flush=True)
    font.main()
    game_bin = tables.build_game_bin(meta, program.story_index, 0)
    game_bin += tables.dev_chapter_table(program, meta) + tables.ui_table(program, meta)
    (output / "GAME.BIN").write_bytes(game_bin)
    shutil.copy2(work / "FONT.BIN", output / "FONT.BIN")
    raw = (work / "TEXT_ES.BIN").read_bytes()
    for chapter in meta["chapters"]:
        i = chapter["index"]
        source = "STORY.VNB" if chapter["name"] == "STORY" else f"C{i:02d}.VNB"
        shutil.copy2(work / "SCRIPT" / source, output / f"C{i:02d}.VNB")
        (output / f"T{i:02d}.TXT").write_bytes(tables.text_block(raw, i))
    if not args.skip_images:
        converted = images.build(meta, output, project.images)
        # DATA is incremental so builds can reuse expensive assets, but old
        # numbered images must not leak into a package after the source shrinks.
        expected_images = {f"I{entry['id']:04d}.IMG" for entry in converted}
        for path in output.glob("I[0-9][0-9][0-9][0-9].IMG"):
            if path.name not in expected_images:
                path.unlink()
        (work / "images.json").write_text(json.dumps(converted, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"Imágenes desde el fuente: {len(converted)}", flush=True)
    else:
        missing = [i for i, entry in enumerate(meta["images"])
                   if entry is not None and not (output / f"I{i:04d}.IMG").is_file()]
        if missing:
            raise FileNotFoundError(f"Faltan {len(missing)} imágenes: ejecutar sin --skip-images")
    if args.profile == "vita":
        from tools.profile.vita_menu import build as build_menu

        build_menu(args.game, output, args.out / "menu-preview", args.profile)
    gallery.build(meta, output, project.images, reuse=args.skip_images)
    for name, asset in (("BOX_NAME", "textbox_long.png"), ("BOX_NONE", "textbox_no_hitch.png")):
        images.convert((0, {"recipe": {"op": "file", "path": f"gui/{asset}"}, "names": [name]}, str(work)))
        shutil.move(str(work / "I0000.IMG"), str(output / f"{name}.IMG"))
    for name, asset in (("CHOICE_IDLE", "choice_idle_background.png"),
                        ("CHOICE_HOVER", "choice_hover_background.png")):
        images.convert((0, {"recipe": {"op": "file", "path": f"gui/button/{asset}"}, "names": [name]}, str(work)))
        shutil.move(str(work / "I0000.IMG"), str(output / f"{name}.IMG"))
    images.convert((0, {"recipe": {"op": "file", "path": "gui/game_menu.png"}, "names": ["SYSTEM_MENU"]}, str(work)))
    shutil.move(str(work / "I0000.IMG"), str(output / "SYSTEM_MENU.IMG"))
    audio_entries = audio.build(meta, output, args.profile)
    pcm_count = sum(e["codec"] == "pcm_s16le_mono" for e in audio_entries)
    print(f"Audio fuente: {len(audio_entries) - pcm_count} OGG y {pcm_count} efectos/UI PCM mono", flush=True)
    sources = {str(p.relative_to(args.game)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(args.game.rglob("*.rpy"))}
    manifest = {"source": str(args.game.resolve()), "language": "es", "lewd": True, "profile": args.profile,
                "scripts_sha256": sources, "chapters": len(program.chapters),
                "images": sum(e is not None for e in meta["images"]), "audio": len(audio_entries), "audio_pending": False}
    (args.out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Datos {args.profile.upper()}: {output}", flush=True)


if __name__ == "__main__":
    main()
