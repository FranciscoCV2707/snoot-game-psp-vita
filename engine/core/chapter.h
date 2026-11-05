/* Capitulo en curso (Cxx.VNB y Txx.TXT) en un solo bloque del heap a su tamano real. Lo usa el cargador de la VM. */
#pragma once

#include "vm.h"

/* Libera el capitulo anterior y carga `chapter`. Devuelve 0 si salio bien; -1 si falta, excede los limites de
 * core/vn_limits.h o no hay heap (lo dice por printf y el capitulo anterior ya no esta). */
int chapter_open(int chapter, VmChapter *out);
/* Texto `local` del capitulo cargado. */
const char *chapter_text(int local);
