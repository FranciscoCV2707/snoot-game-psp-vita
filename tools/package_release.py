"""Empaqueta el VPK y los datos instalables de Vita para un release de GitHub."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_EXTENSIONS = {".IMG", ".OGG", ".PCM", ".TXT", ".VNB", ".AUD", ".BIN"}
LICENSES = (
    "LICENSE",
    "CC-BY-SA-4.0.txt",
    "GNU Affero General Public License Version Three.txt",
    "GPL-Exception",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-root",
        type=Path,
        default=ROOT,
        help="raíz que contiene build/vita y build/vita-source (por defecto: este repositorio)",
    )
    parser.add_argument("--tag", default="v0.1.0", help="etiqueta mostrada en el nombre del ZIP")
    parser.add_argument("--output", type=Path, help="ruta destino; por defecto, dist/SnootGame-PSVita-<tag>.zip")
    args = parser.parse_args()

    build_root = args.build_root.resolve(strict=True)
    manifest_path = build_root / "build/vita-source/manifest.json"
    data = build_root / "build/vita-source/DATA"
    vpk = build_root / "build/vita/snoot.vpk"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("profile"), manifest.get("language"), manifest.get("lewd"), manifest.get("audio_pending")) != (
        "vita", "es", True, False
    ):
        raise ValueError("Los datos no corresponden a un build Vita completo en español")
    if not vpk.is_file() or not data.is_dir():
        raise FileNotFoundError("Se requieren build/vita/snoot.vpk y build/vita-source/DATA/")

    payload = [
        (path, f"DATA/{path.name}")
        for path in sorted(data.iterdir())
        if path.is_file() and path.suffix.upper() in RUNTIME_EXTENSIONS and path.name.lower() != "shot_names.txt"
    ]
    if not payload:
        raise ValueError(f"No hay datos de runtime en {data}")

    game_root = Path(manifest["source"]).resolve(strict=True).parent
    license_files = [(game_root / name, f"LICENCIAS/{name}") for name in LICENSES]
    missing = [str(path) for path, _ in license_files if not path.is_file()]
    if missing:
        raise FileNotFoundError("Faltan avisos de licencia: " + ", ".join(missing))

    files = [(vpk, "snoot.vpk"), *payload, *license_files]
    readme = (
        f"SNOOT GAME PARA PS VITA — BUILD {args.tag}\n"
        "Idioma: español\nResolución de recursos: 960 x 544\n\n"
        "INSTALACIÓN\n"
        "1. Instala snoot.vpk con VitaShell o Vita3K.\n"
        "2. Copia el contenido de DATA/ a ux0:data/snoot/ en la consola.\n"
        "3. Inicia el juego. Las partidas se guardan en ux0:data/snoot/save/.\n\n"
        "El juego y sus recursos originales son de Cavemanon y sus colaboradores.\n"
        "Port nativo: Pako Tako. No oficial, sin afiliación con Cavemanon ni Sony.\n"
        "Los recursos se generaron desde la edición original de PC; este paquete\n"
        "incluye los avisos de licencia correspondientes en LICENCIAS/.\n\n"
        "Fuente del port: https://github.com/FranciscoCV2707/snoot-game-psp-vita\n"
        "Revisado en Vita3K; falta validar en hardware Vita real.\n"
    )

    output = args.output or ROOT / "dist" / f"SnootGame-PSVita-{args.tag}.zip"
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for path, name in files}
    checksums = json.dumps(hashes, ensure_ascii=False, indent=2) + "\n"
    with ZipFile(output, "w", compression=ZIP_STORED, allowZip64=True) as archive:
        for path, name in files:
            archive.write(path, name)
        archive.writestr("LEEME.txt", readme, compress_type=ZIP_DEFLATED, compresslevel=6)
        archive.writestr("SHA256SUMS.json", checksums, compress_type=ZIP_DEFLATED, compresslevel=6)
    print(f"Paquete: {output}")
    print(f"Archivos: {len(files) + 2}; tamaño: {output.stat().st_size} bytes")
    print(f"SHA-256 VPK: {hashes['snoot.vpk']}")


if __name__ == "__main__":
    main()
