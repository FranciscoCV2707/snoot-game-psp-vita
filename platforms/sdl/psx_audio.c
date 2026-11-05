/* Decodificadores de audio de PS1 a PCM 16 bits: SPU-ADPCM (efectos y
 * ambientes, S###.SPU) y XA-ADPCM (MUSIC.XA). Algoritmo de psx-spx (nocash):
 * "SPU ADPCM Samples" y "CDROM XA Audio ADPCM Compression". */
#include "psx_audio.h"

/* psx-spx, "SPU ADPCM Pos/Neg Tables": coeficientes de los filtros 0-4 (XA usa
 * 0-3) */
static const int filter_pos[5] = {0, 60, 115, 98, 122};
static const int filter_neg[5] = {0, 0, -52, -55, -60};

/* Una unidad de 28 muestras. `header`: bits 0-3 shift, bits 4-6 filtro. `raw`:
 * muestras ya extendidas a 16 bits (4 bits SHL 12 u 8 bits SHL 8, como en
 * psx-spx). */
static void decode_unit(int header, const int32_t raw[28], int16_t *out, int32_t hist[2])
{
	int shift = header & 0x0F;
	int filter = (header >> 4) & 0x07;
	int i;

	if (shift > 12)
		shift = 9; /* psx-spx: los shift 13-15 se comportan como 9 */
	if (filter > 4)
		filter = 4;
	for (i = 0; i < 28; i++) {
		/* raw SAR shift + (old * pos + older * neg + 32) SAR 6; los SAR son
     * aritmeticos (gcc) como en psx-spx */
		int32_t s =
		    (raw[i] >> shift) + ((hist[0] * filter_pos[filter] + hist[1] * filter_neg[filter] + 32) >> 6);

		if (s > 32767)
			s = 32767;
		if (s < -32768)
			s = -32768;
		out[i] = (int16_t)s;
		hist[1] = hist[0];
		hist[0] = s;
	}
}

static int32_t nibble_sample(int byte, int high)
{
	int t = high ? byte >> 4 : byte & 0x0F;

	return (t >= 8 ? t - 16 : t) * 4096;
}

int psx_spu_decode(const uint8_t *adpcm, int blocks, int16_t *out, int32_t hist[2])
{
	int32_t raw[28];
	int b, i;

	/* bloque de 16 bytes: byte 0 shift/filtro, byte 1 flags (fin, bucle), bytes
   * 2-15 las 28 muestras, la baja primero */
	for (b = 0; b < blocks; b++) {
		const uint8_t *block = adpcm + 16 * b;

		for (i = 0; i < 28; i++)
			raw[i] = nibble_sample(block[2 + i / 2], i & 1);
		decode_unit(block[0], raw, out + 28 * b, hist);
	}
	return 28 * blocks;
}

int psx_xa_decode_sector(const uint8_t *data, int stereo, int bits8, int16_t *left, int16_t *right, int32_t hist[4])
{
	int units = bits8 ? 4 : 8;
	int32_t raw[28];
	int g, u, j, nl = 0, nr = 0;

	/* 18 grupos de 128 bytes: 16 de cabeceras (la de la unidad u en 4 + u) y 28
   * palabras de 4 bytes; la muestra j de la unidad u esta en la palabra j, byte
   * u (8 bits) o byte u/2, mitad u&1 (4 bits) */
	for (g = 0; g < 18; g++) {
		const uint8_t *group = data + 128 * g;

		for (u = 0; u < units; u++) {
			for (j = 0; j < 28; j++)
				raw[j] = bits8 ? (int8_t)group[16 + 4 * j + u] * 256
				               : nibble_sample(group[16 + 4 * j + u / 2], u & 1);
			/* estereo: unidades pares a la izquierda, impares a la derecha */
			if (stereo && (u & 1)) {
				decode_unit(group[4 + u], raw, right + nr, hist + 2);
				nr += 28;
			} else {
				decode_unit(group[4 + u], raw, left + nl, hist);
				nl += 28;
			}
		}
	}
	return nl;
}
