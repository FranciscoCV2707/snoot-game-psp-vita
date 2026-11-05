/* Tamanos maximos de un capitulo en el disco (docs/presupuesto-memoria.md). vncompiler/formatos.py lee estas mismas
 * lineas para rechazar un capitulo que no cabe: mantener el formato "#define NOMBRE entero". */
#pragma once

#define VN_CHAPTER_MAX_BYTES 98304       /* Cxx.VNB: codigo y animaciones (96 KiB) */
#define VN_TEXT_MAX_BYTES 98304          /* Txx.TXT: textos del capitulo (96 KiB) */
