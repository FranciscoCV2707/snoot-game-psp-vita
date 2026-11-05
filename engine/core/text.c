#include <stdio.h>
#include "text.h"
#include "vn_platform.h"

typedef struct {
	uint16_t cp;
	uint8_t style, w, h;
	uint16_t u, v;
	int8_t xoff, yoff;
	uint8_t adv;
} Glyph; /* en disco: 10 bytes FNT1, 12 bytes FNT2 */

static const Glyph *glyphs;
static int glyph_count;
static int line_h = 14;
static int ascent = 11;

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* FNT1: "FNT1", u8 alto de linea, u8 ascenso, u16 cantidad, tabla de 10 bytes, relleno a 4, atlas 256x256.
 * FNT2: lo mismo pero con u16 atlas_ancho/alto despues de la cantidad y registros de 12 bytes (u y v pasan a
 * 16 bits), porque el cuerpo del perfil de una consola moderna necesita un atlas mas alto de lo que 256 permite.
 * PS1 sigue leyendo FNT1 con sus 256x256 de siempre: el disco de v1.0 no cambia. */
int font_load(const uint8_t *file, int size)
{
	int table_end, atlas_off, rec, i;
	unsigned aw, ah;
	static Glyph table[512];
	int fnt2;

	if (size < 12)
		return -1;
	fnt2 = (file[0] == 'F' && file[1] == 'N' && file[2] == 'T' && file[3] == '2');
	if (!fnt2 && (file[0] != 'F' || file[1] != 'N' || file[2] != 'T' || file[3] != '1'))
		return -1;
	line_h = file[4];
	ascent = file[5];
	glyph_count = rd16(file + 6);
	if (glyph_count > 512)
		return -1;
	if (fnt2) {
		aw = rd16(file + 8);
		ah = rd16(file + 10);
		rec = 12;
		table_end = 12 + glyph_count * 12;
	} else {
		aw = VN_FONT_ATLAS_W;
		ah = VN_FONT_ATLAS_H;
		rec = 10;
		table_end = 8 + glyph_count * 10;
	}
	if (!aw || !ah || aw > 4096 || ah > 4096)
		return -1;
	/* La tabla del archivo no esta alineada a 2 bytes en el struct de C: se copia campo por campo */
	for (i = 0; i < glyph_count; i++) {
		const uint8_t *g = file + (fnt2 ? 12 : 8) + i * rec;
		table[i].cp = rd16(g);
		table[i].style = g[2];
		if (fnt2) {
			table[i].u = rd16(g + 3);
			table[i].v = rd16(g + 5);
			table[i].w = g[7];
			table[i].h = g[8];
			table[i].xoff = (int8_t)g[9];
			table[i].yoff = (int8_t)g[10];
			table[i].adv = g[11];
		} else {
			table[i].u = g[3];
			table[i].v = g[4];
			table[i].w = g[5];
			table[i].h = g[6];
			table[i].xoff = (int8_t)g[7];
			table[i].yoff = (int8_t)g[8];
			table[i].adv = g[9];
		}
	}
	glyphs = table;
	atlas_off = (table_end + 3) & ~3;
	if (size < atlas_off + (int)(aw * ah / 2))
		return -1;
	vn_ops->font_upload(file + atlas_off, (int)aw, (int)ah);
	return 0;
}

int font_line_height(void)
{
	return line_h;
}

static const Glyph *find_glyph(uint32_t cp, int style)
{
	int lo = 0, hi = glyph_count - 1;
	uint32_t key = cp * 2 + (style == 1 ? 1 : 0);

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint32_t k = glyphs[mid].cp * 2 + glyphs[mid].style;
		if (k == key)
			return &glyphs[mid];
		if (k < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return (style == 1) ? find_glyph(cp, 0) : 0;
}

/* Decodifica un caracter UTF-8 y avanza el puntero */
static uint32_t utf8_next(const char **pp)
{
	const uint8_t *p = (const uint8_t *)*pp;
	uint32_t c = p[0];

	if (c < 0x80) {
		*pp += 1;
		return c;
	}
	if ((c & 0xE0) == 0xC0) {
		*pp += 2;
		return ((c & 0x1F) << 6) | (p[1] & 0x3F);
	}
	if ((c & 0xF0) == 0xE0) {
		*pp += 3;
		return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
	}
	*pp += 4;
	return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

/* Agrega un glifo (el backend cierra cada llamada de dibujo con glyphs_end) */
static void add_glyph(int layer, int x, int y, const Glyph *g, int r, int gc, int b)
{
	if (!g->w || !g->h)
		return;
	vn_ops->draw_glyph(layer, x + g->xoff, y + ascent + g->yoff, g->u, g->v, g->w, g->h, r, gc, b);
}

void font_draw(int layer, int x, int y, const char *utf8, int r, int g, int b)
{
	const char *p = utf8;
	int cx = x;

	while (*p) {
		uint32_t cp = utf8_next(&p);
		const Glyph *gl;

		if (cp == '\n') {
			cx = x;
			y += line_h;
			continue;
		}
		gl = find_glyph(cp, 0);
		if (!gl)
			continue;
		add_glyph(layer, cx, y, gl, r, g, b);
		cx += gl->adv;
	}
	vn_ops->glyphs_end(layer);
}

int font_width(const char *utf8)
{
	const char *p = utf8;
	int w = 0;

	while (*p) {
		uint32_t cp = utf8_next(&p);
		const Glyph *gl;

		if (cp == '\n')
			break;
		gl = find_glyph(cp, 0);
		if (gl)
			w += gl->adv;
	}
	return w;
}

/* ---------------------------------------------------------------- maquina de escribir */
static int default_cps = TEXT_DEFAULT_CPS;

void text_set_default_cps(int cps)
{
	if (cps >= 0 && cps <= 120)
		default_cps = cps;
}

void text_start(TextRun *t, const char *utf8)
{
	t->start = t->page = t->pos = utf8;
	t->cps = default_cps;
	t->acc = 0;
	t->wait = 0;
	t->style = 0;
	t->at_page_end = 0;
	t->at_end = (*utf8 == 0);
	t->auto_next = 0;
}

/* Procesa un elemento (codigo de control o caracter). Devuelve 1 si era un caracter visible. */
static int step(TextRun *t)
{
	const char *p = t->pos;
	uint32_t c;

	if (!*p) {
		t->at_end = 1;
		return 0;
	}
	c = utf8_next(&p);
	switch (c) {
	case TEXT_STYLE:
		t->style = (uint8_t)utf8_next(&p);
		t->pos = p;
		return 0;
	case TEXT_WAIT:
		t->wait = (int)utf8_next(&p) * 6; /* decimas de segundo a cuadros */
		t->pos = p;
		return 0;
	case TEXT_SPEED: {
		int n = (int)utf8_next(&p);
		t->cps = n ? n : default_cps;
		t->pos = p;
		return 0;
	}
	case TEXT_NOWAIT:
		t->auto_next = 1;
		t->pos = p;
		return 0;
	case TEXT_FAST:
		t->pos = p;
		return 0;
	case TEXT_SIZE:
		(void)utf8_next(&p);
		t->pos = p;
		return 0;
	case TEXT_SHAKE_ON:
	case TEXT_SHAKE_OFF:
	case TEXT_SCARE_ON:
	case TEXT_SCARE_OFF:
		t->pos = p;
		return 0;
	case TEXT_PAGE:
		t->at_page_end = 1; /* se queda en el codigo hasta que se pida otra pagina */
		return 0;
	default:
		t->pos = p;
		return 1;
	}
}

void text_update(TextRun *t)
{
	if (t->at_end || t->at_page_end)
		return;
	if (t->wait > 0) {
		t->wait--;
		return;
	}
	if (t->cps == 0) {
		while (!t->at_end && !t->at_page_end && t->wait == 0)
			step(t);
		return;
	}
	t->acc += (t->cps * 256) / 60;
	while (t->acc >= 256 && !t->at_end && !t->at_page_end && t->wait == 0) {
		if (step(t))
			t->acc -= 256;
		else if (t->wait > 0)
			break;
	}
}

void text_reveal_page(TextRun *t)
{
	while (!t->at_end && !t->at_page_end)
		step(t);
	t->wait = 0;
}

int text_page_done(const TextRun *t)
{
	return t->at_page_end || t->at_end;
}

int text_finished(const TextRun *t)
{
	return t->at_end;
}

void text_next_page(TextRun *t)
{
	if (t->at_page_end) {
		const char *p = t->pos;
		utf8_next(&p); /* salta el codigo de pagina */
		t->pos = t->page = p;
		t->at_page_end = 0;
		t->acc = 0;
		if (!*p)
			t->at_end = 1;
	}
}

/* Temblor de {sh} y {sc}: un pixel de amplitud. Ren'Py tiembla 2 px en un lienzo 6 veces mayor; eso queda
 * en menos de un pixel de PS1 y no se ve. */
static uint32_t text_rnd = 1;

static int text_jitter(void)
{
	text_rnd = text_rnd * 1103515245u + 12345u;
	return (int)((text_rnd >> 16) % 3) - 1;
}

void text_draw(const TextRun *t, int layer, int x, int y, int r, int g, int b)
{
	const char *p = t->page;
	int cx = x, cy = y, style = 0, shake = 0, scare = 0;
	int sh_x = text_jitter(), sh_y = text_jitter();

	/* se redibuja desde el inicio de la pagina hasta lo revelado */
	while (p < t->pos && *p) {
		uint32_t c = utf8_next(&p);
		const Glyph *gl;

		if (c == TEXT_STYLE) {
			style = (int)utf8_next(&p);
			continue;
		}
		if (c == TEXT_WAIT || c == TEXT_SPEED || c == TEXT_SIZE) {
			(void)utf8_next(&p);
			continue;
		}
		if (c == TEXT_SHAKE_ON || c == TEXT_SHAKE_OFF || c == TEXT_SCARE_ON || c == TEXT_SCARE_OFF) {
			shake = c == TEXT_SHAKE_ON ? 1 : c == TEXT_SHAKE_OFF ? 0 : shake;
			scare = c == TEXT_SCARE_ON ? 1 : c == TEXT_SCARE_OFF ? 0 : scare;
			continue;
		}
		if (c == TEXT_NOWAIT || c == TEXT_FAST || c == TEXT_PAGE)
			continue;
		if (c == TEXT_NEWLINE) {
			cx = x;
			cy += line_h;
			continue;
		}
		gl = find_glyph(c, style);
		if (!gl)
			continue;
		{
			int ox = shake ? sh_x : 0, oy = shake ? sh_y : 0;

			if (scare) {
				ox += text_jitter();
				oy += text_jitter();
			}
			add_glyph(layer, cx + ox, cy + oy, gl, r, g, b);
			if (style == 2) /* negrita: se dibuja dos veces */
				add_glyph(layer, cx + ox + 1, cy + oy, gl, r, g, b);
		}
		cx += gl->adv;
	}
	vn_ops->glyphs_end(layer);
}
