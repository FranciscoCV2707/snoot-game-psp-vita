#include <string.h>
#include "str.h"

#define STR_MAGIC 0x0160
#define STR_TYPE_MDEC 0x8001

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
	return rd16(p) | (uint32_t)rd16(p + 2) << 16;
}

void str_init(StrDemux *d, uint8_t *buf0, uint8_t *buf1, uint32_t cap)
{
	memset(d, 0, sizeof(*d));
	d->buf[0] = buf0;
	d->buf[1] = buf1;
	d->cap = cap;
	d->ready = -1;
}

int str_feed(StrDemux *d, const uint8_t *s)
{
	uint16_t type = rd16(s + 2), id = rd16(s + 4), count = rd16(s + 6);
	uint32_t frame = rd32(s + 8), len = rd32(s + 12), off;
	int n;

	if (rd16(s) != STR_MAGIC)
		return -1;
	if (type != STR_TYPE_MDEC)
		return 0;
	if (d->started && frame < d->frame_id)
		return 0;

	if (!d->started || frame != d->frame_id) {
		if (d->started && d->have < d->want)
			d->dropped++;                      /* el cuadro anterior se quedo sin sectores */
		d->started = 1;
		d->frame_id = frame;
		d->have = 0;
		d->want = count;
		d->bs_len = len;
		d->width = rd16(s + 16);
		d->height = rd16(s + 18);
		/* el buffer libre: el que no espera al consumidor */
		d->fill = d->ready == -1 ? d->fill ^ 1 : 1 - d->ready;
	}

	off = (uint32_t)id * STR_PAYLOAD;
	if (off < d->bs_len) {
		n = d->bs_len - off < STR_PAYLOAD ? (int)(d->bs_len - off) : STR_PAYLOAD;
		if (off + n <= d->cap)
			memcpy(d->buf[d->fill] + off, s + STR_HEADER, n);
	}
	d->have++;

	if (d->have == d->want) {
		if (d->ready != -1)
			d->dropped++;                      /* el consumidor no saco el anterior: el nuevo gana */
		d->ready = d->fill;
		d->ready_len = d->bs_len;
		d->ready_frame = d->frame_id;
		d->ready_w = d->width;
		d->ready_h = d->height;
		d->frames++;
	}
	return 1;
}

int str_take(StrDemux *d, const uint8_t **bs, uint32_t *len, uint16_t *w, uint16_t *h, uint32_t *frame)
{
	if (d->ready == -1)
		return 0;
	*bs = d->buf[d->ready];
	*len = d->ready_len;
	*w = d->ready_w;
	*h = d->ready_h;
	*frame = d->ready_frame;
	d->ready = -1;
	return 1;
}
