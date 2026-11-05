#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chapter.h"
#include "vn_limits.h"
#include "vn_platform.h"

/* Unico usuario del heap en el motor: se libera antes de cada carga, asi que el bloque nuevo vuelve a tener todo el
 * margen libre (no hay huecos de otras reservas). */
static uint8_t *block;
static const uint8_t *text;

static uint16_t rd16(const uint8_t *p)
{
	return p[0] | (p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Los buferes se redondean a sectores de 2048 bytes: las lecturas de CD (PS1) no aceptan otro tamano */
static int sectors_bytes(int size)
{
	return (size + 2047) / 2048 * 2048;
}

int chapter_open(int chapter, VmChapter *out)
{
	char vnb_path[24], text_path[24];
	int vnb_size, text_size, vnb_bytes;

	if (block)
		free(block);
	block = 0;
	text = 0;
	sprintf(vnb_path, "C%02d.VNB", chapter);
	sprintf(text_path, "T%02d.TXT", chapter);
	vnb_size = vn_ops->file_size(vnb_path);
	text_size = vn_ops->file_size(text_path);
	if (vnb_size < 0 || text_size < 0) {
		printf("capitulo %d no disponible\n", chapter);
		return -1;
	}
	if (vnb_size < 32 || vnb_size > VN_CHAPTER_MAX_BYTES || text_size < 2 || text_size > VN_TEXT_MAX_BYTES) {
		printf("capitulo %d: VNB %d / texto %d bytes fuera de los limites %d / %d\n", chapter, vnb_size, text_size,
		       VN_CHAPTER_MAX_BYTES, VN_TEXT_MAX_BYTES);
		return -1;
	}
	vnb_bytes = sectors_bytes(vnb_size);
	block = malloc(vnb_bytes + sectors_bytes(text_size));
	if (!block) {
		printf("capitulo %d: sin heap para %d bytes\n", chapter, vnb_bytes + sectors_bytes(text_size));
		return -1;
	}
	if (vn_ops->file_read(vnb_path, block, vnb_bytes) != vnb_size || memcmp(block, "VNB1", 4) != 0 ||
	    vn_ops->file_read(text_path, block + vnb_bytes, sectors_bytes(text_size)) != text_size) {
		printf("capitulo %d no disponible\n", chapter);
		free(block);
		block = 0;
		return -1;
	}
	text = block + vnb_bytes;
	out->code = block + 32;
	out->code_size = rd32(block + 8);
	out->text_base = rd32(block + 12);
	out->text_count = rd16(block + 16);
	out->anim_count = rd16(block + 18);
	out->anims = block + rd32(block + 24);
	return 0;
}

const char *chapter_text(int local)
{
	return (const char *)(text + 2 + rd32(text + 2 + 4 * local));
}
