"""Punto de entrada: ``python -m tools.assetpipe``."""

from __future__ import annotations

import argparse
from pathlib import Path

from .manifest import run


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.assetpipe",
        description="Arma la carpeta de recursos de una plataforma y su manifest.json.",
    )
    parser.add_argument("--profile", required=True, help="perfil de plataforma (pc, ...)")
    parser.add_argument("--game", required=True, help="carpeta game del juego")
    parser.add_argument("--out", required=True, help="carpeta de salida")
    parser.add_argument("--language", help="traduccion de Ren'Py (game/tl/<idioma>); Snoot se porta solo en es")
    args = parser.parse_args(argv)

    game = Path(args.game)
    out = Path(args.out)

    try:
        manifest = run(args.profile, game, out, args.language)
    except ValueError as exc:
        parser.error(str(exc))

    total_bytes = sum(entry["bytes"] for entry in manifest["resources"])
    print(f"{args.profile}: {manifest['count']} recursos, {total_bytes} bytes -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
