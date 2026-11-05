/* Decodificadores de audio de PS1 (psx_audio.c): SPU-ADPCM y XA-ADPCM a PCM 16
 * bits, sin SDL ni memoria dinamica. */
#pragma once

#include <stdint.h>

/* Decodifica `blocks` bloques SPU-ADPCM de 16 bytes (28 muestras cada uno);
 * hist guarda las dos ultimas muestras entre llamadas. Devuelve la cantidad de
 * muestras. */
int psx_spu_decode(const uint8_t *adpcm, int blocks, int16_t *out, int32_t hist[2]);
/* Decodifica un sector XA (los 2304 bytes de datos tras la subcabecera) de un
 * carril; hist[0..1] es el canal izquierdo (o mono) y hist[2..3] el derecho.
 * Devuelve las muestras por canal. */
int psx_xa_decode_sector(const uint8_t *data, int stereo, int bits8, int16_t *left, int16_t *right, int32_t hist[4]);
