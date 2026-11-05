# Compilación y datos para PS Vita

## Flujo de generación

`tools.snoot_source` lee los `.rpy`, imágenes, audio, fuentes y traducción española de la instalación original.
Genera bytecode y archivos en `build/vita-source/DATA/`; después `platforms/vita/build.py` compila el runtime
con SDL2/VitaSDK y crea `build/vita/snoot.vpk`.

```powershell
.\.venv\Scripts\python.exe -m tools.snoot_source --game 'E:\snoot\snootgame\game' --out build/vita-source --profile vita
.\.venv\Scripts\python.exe platforms/vita/build.py --profile vita
py -3.12 tools/package_release.py --tag v0.1.0
```

El último comando empaqueta el VPK, los datos de runtime y sus licencias en `dist/` para adjuntarlo a un release.

`--trace` en el segundo comando activa el registro del runtime. `--define NOMBRE=VALOR` permite pasar una
macro C experimental al build de Vita.

## Instalación

Instala el VPK y copia los archivos que están **dentro** de `build/vita-source/DATA/` a `ux0:data/snoot/`.
El backend busca allí `GAME.BIN`, scripts, imágenes y audio; las partidas quedan en `ux0:data/snoot/save/`.

## Alcance

- Resolución de recursos y salida: 960 × 544.
- Idioma: español.
- Contenido sensible: se genera con `persistent.lewd` activado, igual que se acordó para estos ports.
- Entrada: controles de Vita y pantalla táctil.
- Assets: se generan localmente; no se guardan ni distribuyen en este repositorio.

El emulador ayuda a revisar arranque, interfaz y flujo de juego, pero una compilación exitosa no demuestra por
sí sola compatibilidad completa en hardware real.
