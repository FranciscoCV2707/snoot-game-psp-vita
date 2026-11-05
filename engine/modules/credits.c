#include <string.h>
#include "app.h"
#include "input.h"
#include "special.h"
#include "text.h"
#include "video.h"

#define MAX_CREDITS 128

/* Creditos: suben solos (X los acelera, Start los termina) y al final vuelve el guion */
static int credits_count, credits_y, credits_used;
static uint8_t credits_style[MAX_CREDITS];
static uint16_t credits_off[MAX_CREDITS];
static char credits_text[4096];

void credits_clear(void)
{
	credits_count = credits_used = 0;
}

void credits_add(int style, const char *text, int len)
{
	if (credits_count >= MAX_CREDITS || credits_used + len + 1 > (int)sizeof(credits_text))
		return;
	credits_style[credits_count] = (uint8_t)style;
	credits_off[credits_count] = (uint16_t)credits_used;
	memcpy(credits_text + credits_used, text, len);
	credits_text[credits_used + len] = 0;
	credits_used += len + 1;
	credits_count++;
}

static int credits_begin(void)
{
	if (!credits_count)
		return 0;
	credits_y = VIEW_Y + VIEW_H;
	return 1;
}

static int credits_update(void)
{
	int lh = font_line_height() + VIEW_H * 4 / 180;
	int speed = input_down(BTN_CROSS) ? VIEW_H * 4 / 180 : 1;

	credits_y -= speed;
	return credits_y + credits_count * lh < 0 || input_pressed(BTN_START);
}

static void credits_draw(void)
{
	int lh = font_line_height() + VIEW_H * 4 / 180, i;

	video_rect(LAYER_BOX, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0);
	for (i = 0; i < credits_count; i++) {
		int y = credits_y + i * lh, st = credits_style[i];

		if (y < VIEW_Y - lh || y > VIEW_Y + VIEW_H)
			continue;
		draw_center(y, credits_text + credits_off[i], st == 2 ? 255 : st == 1 ? 255 : 128, st == 2 ? 220 : st == 1 ? 160 : 128,
		            st == 2 ? 40 : st == 1 ? 60 : 128);
	}
}

const Special special_credits = { SP_IN_GAME, credits_begin, credits_update, credits_draw, 0 };
