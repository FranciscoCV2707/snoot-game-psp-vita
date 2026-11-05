"""Compila Snoot para SDL2 con el toolchain MSYS2 ucrt64."""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MSYS = Path(r"C:\msys64\ucrt64\bin")


def profile_view(profile: str) -> tuple[int, int]:
    """La vista logica de la consola, sacada de la unica tabla de perfiles que hay en este repo."""
    import sys

    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    from tools.assetpipe.profiles import PROFILES

    if profile not in PROFILES:
        raise SystemExit(f"perfil desconocido: {profile} (hay {', '.join(sorted(PROFILES))})")
    return PROFILES[profile].get("image_size", (320, 180))


def view_flags(profile: str | None) -> list[str]:
    """La vista logica de la consola (engine/platform/video.h) segun el perfil de `tools/assetpipe`.

    Sin perfil quedan los numeros de PS1, que son los que producen el disco de v1.0. La vista de una consola
    nativa ocupa la pantalla entera: no hay letterbox que escalar.
    """
    if not profile:
        return []
    w, h = profile_view(profile)
    return [f"-DSCREEN_W={w}", f"-DSCREEN_H={h}", "-DVIEW_Y=0", f"-DVIEW_H={h}"]


def presentation_flags(profile: str | None) -> list[str]:
    """El rectangulo de presentacion del backend SDL (`sdl_target.h`, usado cuando MANUAL_SCALE = 1).

    Los valores de PS1 (una ventana 4:3 centrada) dejan el render de un perfil nativo aplastado: en PSP
    SCREEN_W=480 dentro de VIEW_W=360 escala 0,75x. Con perfil, la vista ocupa la pantalla completa y el
    escalado pasa a ser identidad.
    """
    if not profile:
        return []
    w, h = profile_view(profile)
    return ["-DSDL_TARGET_VIEW_X=0", "-DSDL_TARGET_VIEW_Y=0", f"-DSDL_TARGET_VIEW_W={w}", f"-DSDL_TARGET_VIEW_H={h}"]


def flags_changed(out_dir: Path, flags: list[str]) -> bool:
    """True si las flags de compilacion son distintas a las del ultimo build de esta carpeta.

    `make` no compara CFLAGS para decidir que recompilar: cambiar de perfil sin limpiar deja los objetos con la
    vista del perfil anterior, que es exactamente el caso en que alguien jura que `--profile` no hizo nada. Los
    builds de consola (psp, vita) limpian su `obj/` cuando esto devuelve True.
    """
    marker = out_dir / ".flags"
    key = " ".join(sorted(flags)) + "\n"
    previous = marker.read_text(encoding="utf-8") if marker.exists() else ""
    if previous == key:
        return False
    out_dir.mkdir(parents=True, exist_ok=True)
    marker.write_text(key, encoding="utf-8")
    return True


def build(trace: bool = False, profile: str | None = None, defines=()) -> Path:
    if profile == "psp":
        import sys

        if str(ROOT) not in sys.path:
            sys.path.insert(0, str(ROOT))
        from tools.profile.psp_menu import build as build_menu

        build_menu(Path(os.environ.get("SNOOT_SOURCE", "E:/snoot/snootgame/game")),
                   ROOT / "build/psp-data/DATA", ROOT / "build/psp-menu-preview")
    out = ROOT / "build" / "pc"
    out.mkdir(parents=True, exist_ok=True)
    generated = out / "generated"
    generated.mkdir(exist_ok=True)
    (generated / "game_config.h").write_text(
        '#pragma once\n#define VN_SEEN_BYTES 2048\n#define VN_SAVE_TITLE "SNOOT GAME"\n'
        '#define VN_SAVE_PREFIX "BASLUS-99999SNOOT"\n#define VN_SAVE_MAGIC "SNS1"\n',
        encoding="utf-8",
        newline="\n",
    )
    env = os.environ.copy()
    env["PATH"] = str(MSYS) + os.pathsep + r"C:\msys64\usr\bin" + os.pathsep + env.get("PATH", "")
    config = subprocess.run([r"C:\msys64\usr\bin\bash.exe", str(MSYS / "sdl2-config"), "--cflags", "--libs"],
                            check=True, capture_output=True, text=True, encoding="utf-8", env=env)
    flags = [flag for flag in config.stdout.split() if flag != "-mwindows"]
    sources = []
    for folder in ("engine/core", "engine/app", "engine/modules", "games/snoot", "platforms/sdl"):
        sources.extend(sorted((ROOT / folder).glob("*.c")))
    command = [str(MSYS / "gcc.exe"), "-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter", "-pedantic", "-O2"]
    command.extend(f"-I{ROOT / path}" for path in (
        "engine/core", "engine/platform", "engine/app", "engine/modules", "games/snoot",
        "platforms/sdl", "build/pc/generated",
    ))
    if trace:
        command.append("-DVN_TRACE")
    command.extend(view_flags(profile))
    command.extend(presentation_flags(profile))
    if profile == "psp":
        command.extend(["-DVN_PSP_NATIVE_MENU", "-DVN_NATIVE_AUDIO"])
    elif profile == "vita":
        command.extend(["-DVN_VITA_NATIVE_MENU", "-DVN_NATIVE_AUDIO"])
    command.extend(f"-D{d}" for d in defines)
    command.extend(str(source) for source in sources)
    command.extend(flags)
    if profile in ("psp", "vita"):
        command.extend(["-lvorbisfile", "-lvorbis", "-logg"])
        for library in ("libvorbisfile-3.dll", "libvorbis-0.dll", "libogg-0.dll"):
            shutil.copy2(MSYS / library, out / library)
    target = out / "snoot.exe"
    command.extend(["-o", str(target)])
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    shutil.copy2(MSYS / "SDL2.dll", out / "SDL2.dll")
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--profile", help="vista de la consola a la que se apunta (vacio = la de PS1, la de siempre)")
    parser.add_argument("--define", action="append", default=[], metavar="K=V",
                        help="-D extra; sirve para medir un limite del backend sin tocar el codigo")
    args = parser.parse_args()
    print(build(args.trace, args.profile, args.define))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
