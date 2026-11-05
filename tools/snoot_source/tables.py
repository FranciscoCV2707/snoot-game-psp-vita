"""Tablas del juego para el runtime portátil; entradas extraídas del fuente Ren'Py."""

import struct

from tools.snoot_source.config import GAME

def parse_color(text):
    s = text.lstrip("#")
    if len(s) == 3:
        s = "".join(c * 2 for c in s)
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4)) if len(s) >= 6 else (255, 255, 255)

def build_game_bin(meta, start_chapter, start_offset):
    out = b"GAM1" + struct.pack("<HIH", start_chapter, start_offset, len(meta["speakers"]))
    for sp in meta["speakers"]:
        name = sp["name"].encode("utf-8")[:23]
        out += bytes(parse_color(sp["color"])) + bytes([len(name)]) + name
    out += struct.pack("<H", len(meta["positions"]))
    for pos in meta["positions"]:
        out += struct.pack("<hh", round(pos["xalign"] * 256), round(pos["yalign"] * 256))
    # imagenes de la galeria: id de imagen -> indice en la galeria
    gallery = [(iid, img["gallery"]) for iid, img in enumerate(meta["images"]) if img and img.get("gallery") is not None]
    out += struct.pack("<HH", len(meta["gallery"]), len(gallery))
    for iid, index in gallery:
        out += struct.pack("<HB", iid, index)
    return out

def spanish_strings(rel="screens.rpy"):
    """Pares old -> new de una traduccion de cadenas (titulos de los capitulos extra, creditos)."""
    out, old = {}, None
    for line in (GAME / "tl" / "es" / rel).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("old "):
            old = line[4:].strip().strip('"')
        elif line.startswith("new ") and old is not None:
            out[old] = line[4:].strip().strip('"')
            old = None
    return out

def literal_after(text, name):
    """Valor literal de `name = [...]` o `{...}` en un archivo de Ren'Py (sin las llamadas _() de traduccion)."""
    import ast
    import re
    start = text.index(name + " = ") + len(name) + 3
    depth, i, quote = 0, start, None
    while True:
        c = text[i]
        if quote:
            quote = None if c == quote else quote       # dentro de una cadena los corchetes no cuentan
        elif c in "\"'":
            quote = c
        elif c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
        i += 1
        if depth == 0 and not quote:
            break
    body = re.sub(r"_\((\"[^\"]*\"|'[^']*')\)", r"\1", text[start:i])
    return ast.literal_eval(body)

def credits_lines():
    """Creditos finales (`label lending`): (estilo, texto) con 0 nombre, 1 encabezado, 2 titulo."""
    text = (GAME / "src" / "credits.rpy").read_text(encoding="utf-8")
    es = spanish_strings("src/credits.rpy")
    # en la traduccion los saltos de linea van escritos como \n; "[[" es el escape de Ren'Py para "["
    tr = lambda t: es.get(t.replace("\n", "\\n"), t).replace("\\n", "\n").replace("[[", "[")
    start, groups, end = (literal_after(text, n) for n in ("list_credits_start", "list_credits", "list_credits_end"))
    lines = [(2, tr(start[0])), (1, tr(start[1])), (0, tr(start[2])), (0, "")]
    for head, names in groups.items():
        lines.append((1, tr(head)))
        lines += [(0, tr(n)) for n in names]
        lines.append((0, ""))
    lines.append((2, tr(end[0])))
    lines += [(0, part) for part in tr(end[1]).split("\n")]
    return lines

def chapter_title(label, extras):
    if label in extras:
        return extras[label]
    return "Capítulo " + label.removeprefix("chapter_").replace("_", ".")

def ui_table(program, meta):
    """Seccion UI01 de GAME.BIN: logo y candado del titulo, imagenes de los 4 finales, nombre de cada capitulo y la
    lista de capitulos extra (en el orden del menu original) con su entrada."""
    by_name = {n: i for i, img in enumerate(meta["images"]) if img for n in img["names"]}
    endings = sorted((img["ending"], i) for i, img in enumerate(meta["images"]) if img and img.get("ending"))
    es = spanish_strings()
    extras = {label: es.get(title, title) for title, label in meta["extra_chapters"]}
    names = {e["chapter"]: label for label, e in meta["entries"].items()}
    out = b"UI01" + struct.pack("<HH", by_name.get("ui logo", 0), by_name.get("ui lock", 0))
    out += struct.pack("<4H", *[i for _, i in endings][:4])
    out += struct.pack("<H", len(program.chapters))
    for idx in range(len(program.chapters)):
        name = chapter_title(names[idx], extras).encode("utf-8")[:40] if idx in names else b""
        out += bytes([len(name)]) + name
    out += bytes([len(meta["extra_chapters"])])
    for _, label in meta["extra_chapters"]:
        e = meta["entries"][label]
        name = extras[label].encode("utf-8")[:40]
        out += struct.pack("<HIB", e["chapter"], e["offset"], len(name)) + name
    pages = sorted(i for i, img in enumerate(meta["images"]) if img and img["names"][0].startswith("ui textscroll "))
    out += struct.pack("<HB", pages[0] if pages else 0, len(pages))
    lines = credits_lines()
    out += struct.pack("<H", len(lines))
    for style, line in lines:
        data = line.strip().encode("utf-8")[:60]
        out += bytes([style, len(data)]) + data
    return out

def dev_chapter_table(program, meta):
    """Menu de desarrollo: cada LOAD_CHAPTER de STORY, con la variable de ruta que lo protege (si la hay)."""
    names = {e["chapter"]: label for label, e in meta["entries"].items()}
    story = program.chapters[program.story_index]
    out, recent, cond = [], [], None
    for off, name, a in story.walk():
        if name == "JUMP_IF_FALSE" and [n for n, _ in recent] == ["PUSH_VAR", "PUSH_CONST", "OP"]:
            cond = (recent[0][1]["var"], recent[1][1]["value"])
        elif name == "LOAD_CHAPTER":
            label = names.get(a["chapter"], f"chapter_{a['chapter']}").removeprefix("chapter_").replace("_", ".")
            text = f"Capitulo {label}" + (f"  (ruta {cond[1]})" if cond else "")
            var, value = cond if cond else (255, 0)
            out.append((off, var, value, text.encode("utf-8")))
        recent = (recent + [(name, a)])[-3:]
    blob = struct.pack("<HH", program.story_index, len(out))
    for off, var, value, text in out:
        blob += struct.pack("<IBBB", off, var, value, len(text)) + text
    return blob

def text_block(raw, index):
    off, size = struct.unpack_from("<II", raw, 6 + 8 * index)
    return raw[off:off + size]
