"""Compila Snoot para PS Vita con VitaSDK en Docker y empaqueta snoot.vpk."""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from platforms.sdl import build as sdlbuild  # noqa: E402  (view_flags/presentation_flags: una sola tabla)

IMAGE = "vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb"
SDL2_TAG = "release-2.32.10"
SDL2_SRC = ROOT / "build" / "vita" / "sdl2-src"
SDL2_BUILD = ROOT / "build" / "vita" / "sdl2-build"
SDL2_PREFIX = ROOT / "build" / "vita" / "sdl2"
OUT = ROOT / "build" / "vita"


def docker_sh(script: str) -> None:
    subprocess.run(
        ["docker", "run", "--rm", "-v", f"{ROOT.as_posix()}:/src", "-w", "/src", IMAGE, "sh", "-c", script],
        cwd=ROOT,
        check=True,
    )


def ensure_sdl2() -> None:
    if (SDL2_PREFIX / "include" / "SDL2" / "SDL.h").is_file() and (SDL2_PREFIX / "lib" / "libSDL2.a").is_file():
        return
    SDL2_SRC.mkdir(parents=True, exist_ok=True)
    SDL2_BUILD.mkdir(parents=True, exist_ok=True)
    SDL2_PREFIX.mkdir(parents=True, exist_ok=True)
    if not (SDL2_SRC / "CMakeLists.txt").is_file():
        docker_sh(
            "rm -rf /src/build/vita/sdl2-src && "
            f"git clone --depth 1 --branch {SDL2_TAG} https://github.com/libsdl-org/SDL.git /src/build/vita/sdl2-src"
        )
    docker_sh(
        "rm -rf /src/build/vita/sdl2-build && mkdir -p /src/build/vita/sdl2-build && "
        "cd /src/build/vita/sdl2-build && "
        "cmake -S /src/build/vita/sdl2-src -B . "
        "-DCMAKE_TOOLCHAIN_FILE=/usr/local/vitasdk/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release "
        "-DCMAKE_INSTALL_PREFIX=/src/build/vita/sdl2 && "
        "make -j4 install"
    )


def build(trace: bool = False, profile: str | None = None, defines=()) -> Path:
    OUT.mkdir(parents=True, exist_ok=True)
    generated = OUT / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    (generated / "game_config.h").write_text(
        '#pragma once\n#define VN_SEEN_BYTES 2048\n#define VN_SAVE_TITLE "SNOOT GAME"\n'
        '#define VN_SAVE_PREFIX "BASLUS-99999SNOOT"\n#define VN_SAVE_MAGIC "SNS1"\n',
        encoding="utf-8",
        newline="\n",
    )
    ensure_sdl2()
    command = [
        "docker", "run", "--rm",
        "-v", f"{ROOT.as_posix()}:/src",
        "-w", "/src",
        IMAGE, "make", "-f", "/src/platforms/vita/Makefile",
    ]
    if trace:
        command.append("VN_TRACE=1")
    flags = sdlbuild.view_flags(profile) + sdlbuild.presentation_flags(profile)
    if profile == "vita":
        flags.extend(["-DVN_NATIVE_AUDIO", "-DVN_VITA_NATIVE_MENU"])
    flags.extend(f"-D{d}" for d in defines)
    if flags:
        command.append(f"EXTRA_CFLAGS={' '.join(flags)}")
    # make no mira las flags para decidir que recompilar: si cambio el perfil o el --trace, hay que tirar los objetos
    if sdlbuild.flags_changed(OUT, [*(["-DVN_TRACE"] if trace else []), *flags]):
        shutil.rmtree(OUT / "obj", ignore_errors=True)
    subprocess.run(command, cwd=ROOT, check=True)
    return OUT / "snoot.vpk"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--profile", default="vita", help="perfil de recursos Vita (vita por defecto)")
    parser.add_argument("--define", action="append", default=[], metavar="K=V",
                        help="-D extra (p.ej. SDL_TARGET_RES_CACHE=6) para medir presupuestos sin tocar el header")
    args = parser.parse_args()
    print(build(args.trace, args.profile, args.define))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
