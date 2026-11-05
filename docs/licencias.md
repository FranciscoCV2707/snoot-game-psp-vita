# Licencias — qué exige cada una al publicar un port

Guía de publicación para los ports de `renpy-console-project` (quién es quién y con qué licencia está en
`CREDITS.md`). Antes de cada build público se cumple la lista de `PLAN.md` sección 10; aquí solo lo legal.

## Reglas generales de todos los builds

1. **Port no oficial:** el build y la página de descarga deben decir claramente que es un port no oficial
   sin afiliación con Cavemanon, Sony, Nintendo, Microsoft o los SDKs.
2. **Código fuente del port público y enlazado junto al build** (AGPL-3.0, ver abajo).
3. **Sin BIOS, firmware ni claves de consolas** dentro del build.
4. `CREDITS.md` (este repo) se copia al disco/ROM o se muestra en una pantalla de créditos del juego.
   Los textos de licencia que se piden abajo van a un directorio `LICENSES/` del build.
5. Los recursos convertidos (imágenes, audio, fuentes rasterizadas) son obras derivadas: se publican
   con **CC-BY-SA 4.0**.

## Juego y port

- **AGPL-3.0** (código del juego y, por obra derivada, del port):
  incluir el texto completo de la licencia en el build, publicar el código fuente completo del port
  (núcleo, backend y scripts de build) en un enlace junto al binario y señalar los cambios realizados.
  El texto sale de `GNU Affero General Public License Version Three.txt` del juego.
- **CC-BY-SA 4.0** (guion, arte, audio, traducciones de Cavemanon):
  - atribuir a Cavemanon y a los autores del juego (título, autor, enlace al original);
  - enlazar a la licencia (creativecommons.org/licenses/by-sa/4.0);
  - indicar los cambios (conversión de resolución, paletas, audio re-codificado, fuentes rasterizadas);
  - los recursos nuevos o modificados del port heredan la misma licencia (share-alike);
  - la lista de atribuciones de terceros del `LICENSE` del juego se copia sin recortar (Freesound,
    Sonniss GDC 2015-2017, imágenes de stock).
- **Fuentes tipográficas** rasterizadas en `FONT.BIN`: las SIL OFL (Noto) piden su propio aviso (abajo).
  `FallingSky.otf` no trae archivo de licencia en el juego: **por verificar** antes de publicar.

## Licencias de bibliotecas y SDKs

- **MIT** (nxdk, cabeceras VitaSDK, zlib de Ren'Py): incluir el aviso de copyright y la nota de
  permiso. No piden publicar fuente propio.
- **zlib** (SDL2, libctru, zlib): incluir el aviso de copyright; no hacer creer que los autores
  respaldan el port. No piden publicar fuente propio.
- **BSD 3-cláusulas / estilo Xiph** (libogg, libvorbis, pspsdk): conservar el aviso de copyright en la
  documentación del build; no usar los nombres para promocionar. No piden fuente.
- **ISC** (libnx): igual que MIT/BSD, conservar el aviso de copyright.
- **MPL 2.0** (PSn00bSDK): incluir el aviso de licencia en el build (archivo `LICENSE` del SDK) y
  publicar las **modificaciones hechas al propio SDK**, si las hay. No obliga a abrir el resto del port,
  que ya se publica por AGPL de todas formas.
- **AFL 2.0** (ps2sdk, gsKit): es copyleft débil a nivel de archivo; redistribuir obliga a poner a
  disposición el texto de la licencia y las fuentes de ps2sdk/gsKit (ya públicas) o un enlace a ellas.
- **SIL OFL 1.1** (Noto Sans JP, Noto Sans Thai Looped): incluir el texto de la OFL y el nombre
  reservado de la fuente al distribuirla; no vender la fuente suelta; las versiones modificadas (p. ej.
  `FONT.BIN` rasterizado) cambian de nombre de familia.
- **GPLv2** (mkpsxiso, dumpsxiso): herramienta de build, no se distribuye en el disco; basta con
  publicar los scripts de build que la usan (ya incluidos en el repo del port).
- **CC0** (PDCLib en nxdk): sin requisitos; se recomienda mencionar la atribución.

## Qué verificar antes de publicar (por verificar)

| Ítem | Por qué | Fuente consultada |
|---|---|---|
| `FallingSky.otf` | El juego no incluye licencia de la fuente; las fichas dicen que es derivada de Source Sans Pro (OFL), pero no hay archivo oficial | font.download/font/falling-sky |
| libogc (Wii/GC) | GitHub marca la licencia como "Other" (texto propio estilo BSD); leer su `LICENSE` cuando se construya ese backend | github.com/devkitPro/libogc |
| Paquetes de VitaSDK | Las cabeceras son MIT, pero cada paquete enlazado (vitaSha, SDL2-Vita...) trae su propia licencia en vdpm | github.com/vitasdk/vdpm |
| Audio de terceros del juego | El `LICENSE` del juego da autor y obra, no la licencia individual de cada sonido de Freesound; si alguna fuera no comercial, choca con la redistribución CC-BY-SA | `LICENSE` del juego |
| Butano y otros SDK de futuras fases (GBA/GB) | No se enlazan todavía; auditar al llegar la fase | `PLAN.md` sección 10 |
