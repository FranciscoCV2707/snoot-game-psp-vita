#include "input.h"
#include "res.h"
#include "special.h"
#include "text.h"
#include "video.h"

/* Chat de telefono con burbujas (Wani: Better EMR Phone). Los mensajes llegan con OP_PHONE (main.c los pasa con
 * phone_add). Lado 0: el otro personaje, a la izquierda; lado 1: la protagonista, a la derecha. Una foto es un
 * mensaje con imagen. Se lee de abajo hacia arriba: empieza en el ultimo mensaje; arriba/abajo recorre y X sale.
 * Un salto de linea dentro del texto lo pone la PC (el motor no parte lineas). */
#define PH_MAX 32
#define PH_W (VIEW_W * 15 / 32)
#define PH_PAD (VIEW_W / 80)
#define PH_GAP (VIEW_W / 80)
#define PH_MARGIN (VIEW_W * 3 / 160)

static struct { int side, img; const char *text; } ph[PH_MAX];
static int ph_n, ph_scroll;

void phone_clear(void)
{
	ph_n = 0;
}

void phone_add(int side, const char *text, int img)
{
	if (ph_n < PH_MAX) {
		ph[ph_n].side = side ? 1 : 0;
		ph[ph_n].img = img;
		ph[ph_n].text = text;
		ph_n++;
	}
}

static int text_lines(const char *s)
{
	int n = 1;

	for (; *s; s++)
		if ((unsigned char)*s == TEXT_NEWLINE)
			n++;
	return n;
}

/* Alto de la burbuja: su texto y, si la hay, su foto */
static int bubble_h(int i)
{
	int h = text_lines(ph[i].text) * font_line_height() + 2 * PH_PAD;

	if (ph[i].img) {
		const Image *img = res_get(ph[i].img);

		if (img && img->kind <= 1)
			h += img->full_h + PH_PAD;
	}
	return h;
}

static int phone_total(void)
{
	int i, h = PH_MARGIN;

	for (i = 0; i < ph_n; i++)
		h += bubble_h(i) + PH_GAP;
	return h + PH_MARGIN;
}

static int phone_max_scroll(void)
{
	int max = phone_total() - VIEW_H;

	return max > 0 ? max : 0;
}

static int phone_begin(void)
{
	if (!ph_n)
		return 0;
	ph_scroll = phone_max_scroll();
	return 1;
}

static int phone_update(void)
{
	int max = phone_max_scroll(), step = input_down(BTN_R1) ? 8 : 3;

	if (input_down(BTN_UP))
		ph_scroll = ph_scroll - step < 0 ? 0 : ph_scroll - step;
	if (input_down(BTN_DOWN))
		ph_scroll = ph_scroll + step > max ? max : ph_scroll + step;
	return input_pressed(BTN_CROSS);
}

static void phone_draw(void)
{
	int lh = font_line_height(), y = PH_MARGIN - ph_scroll, i;

	res_new_scene();
	video_rect(LAYER_BG, 0, VIEW_Y, SCREEN_W, VIEW_H, 16, 20, 32, 0);
	for (i = 0; i < ph_n; i++) {
		int h = bubble_h(i), x = ph[i].side ? SCREEN_W - PH_MARGIN - PH_W : PH_MARGIN;
		const char *s = ph[i].text;
		int ty = y + PH_PAD, line = 0;

		if (y + h > 0 && y < VIEW_H) {
			video_rect(LAYER_BOX, x, VIEW_Y + y, PH_W, h, ph[i].side ? 48 : 96, ph[i].side ? 88 : 80,
			           ph[i].side ? 120 : 64, 0);
			if (ph[i].img) {
				const Image *img = res_get(ph[i].img);

				if (img && img->kind <= 1)
					img_draw(img, LAYER_TEXT, x + PH_PAD, VIEW_Y + ty);
			}
			/* las lineas de texto van despues de la foto, si la hay */
			ty = y + PH_PAD + (ph[i].img ? h - text_lines(s) * lh - 2 * PH_PAD : 0);
			for (;;) {
				char buf[96];
				int n = 0;

				while (s[n] && (unsigned char)s[n] != TEXT_NEWLINE && n < (int)sizeof(buf) - 1) {
					buf[n] = s[n];
					n++;
				}
				buf[n] = 0;
				font_draw(LAYER_TEXT, x + PH_PAD, VIEW_Y + ty + line * lh, buf, 128, 128, 128);
				s += n;
				if (!*s)
					break;
				if ((unsigned char)*s == TEXT_NEWLINE) {
					s++;
					line++;
				}
			}
		}
		y += h + PH_GAP;
	}
	video_clip_scene();
}

const Special special_phone = { SP_IN_GAME | SP_OWN_R1 | SP_OWN_SCENE, phone_begin, phone_update, phone_draw, 0 };
