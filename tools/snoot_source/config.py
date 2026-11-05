"""Perfiles del compilador directo de recursos Ren'Py de Snoot Game."""
from dataclasses import dataclass
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = Path(os.environ.get("SNOOT_SOURCE", r"E:/snoot/snootgame/game")).resolve()
DATA = Path(os.environ.get("SNOOT_BUILD", str(ROOT / "build/vita-source/compiled"))).resolve()
if DATA == GAME or DATA.is_relative_to(GAME):
    raise ValueError("La salida del port no puede estar dentro del juego original")


@dataclass(frozen=True)
class SourceProfile:
    name: str = "vita"
    view: tuple[int, int] = (960, 544)
    text_size: int = 21
    text_width: int = 730
    choice_width: int = 888
    textscroll_scale: float = 1.0


PROFILES = {"vita": SourceProfile()}
PROFILE_NAME = os.environ.get("SNOOT_PROFILE", "vita").strip().lower() or "vita"
try:
    PROFILE = PROFILES[PROFILE_NAME]
except KeyError as exc:
    raise ValueError(f"perfil fuente no soportado: {PROFILE_NAME}") from exc
