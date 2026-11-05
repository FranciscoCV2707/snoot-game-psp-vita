#include <string.h>
#include "app.h"
#include "input.h"
#include "special.h"
#include "text.h"
#include "ui_text.h"
#include "video.h"

/* Teclado en pantalla (renpy.input). Snoot: el mensaje del capitulo 13D; lo escrito no se guarda. */
#define KB_COLS 10
#define KB_ROWS 6                            /* 5 de teclas y la de acciones */
#define KB_MAX 36                            /* caracteres: una linea */
#define KB_X (VIEW_W / 16)
#define KB_Y (VIEW_H == 180 ? 84 : VIEW_H - KB_ROWS * KB_H)
#define KB_W (VIEW_W * 7 / 80)
#define KB_H (VIEW_H / 10)
#define KB_HELP_Y (VIEW_H == 180 ? SCREEN_H - 22 : KB_Y - font_line_height())

static const char *const kb_keys[2][KB_ROWS - 1][KB_COLS] = {
	{ { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
	  { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
	  { "a", "s", "d", "f", "g", "h", "j", "k", "l", "ñ" },
	  { "z", "x", "c", "v", "b", "n", "m", ",", ".", "?" },
	  { "á", "é", "í", "ó", "ú", "¿", "¡", "!", "'", "-" } },
	{ { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
	  { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
	  { "A", "S", "D", "F", "G", "H", "J", "K", "L", "Ñ" },
	  { "Z", "X", "C", "V", "B", "N", "M", ",", ".", "?" },
	  { "Á", "É", "Í", "Ó", "Ú", "¿", "¡", "!", "'", "-" } },
};
#define kb_actions (game_text + T_KB_SHIFT)              /* 4 entradas seguidas en ui_text.h */
static char kb_text[KB_MAX * 2 + 1];
static int kb_len, kb_row, kb_col, kb_upper, kb_wait;

/* Consulta tactil sin backend (PS1, GBA): el teclado solo responde a los botones ahi */
__attribute__((weak)) int input_touch(int *x, int *y)
{
	(void)x;
	(void)y;
	return 0;
}

static int kb_begin(void)
{
	kb_len = kb_row = kb_col = kb_upper = 0;
	kb_text[0] = 0;
	return 1;
}

static void kb_type(const char *s)
{
	int n = (int)strlen(s), used = (int)strlen(kb_text);

	if (kb_len >= KB_MAX || used + n >= (int)sizeof(kb_text))
		return;
	memcpy(kb_text + used, s, n + 1);
	kb_len++;
}

static void kb_erase(void)
{
	int used = (int)strlen(kb_text);

	if (!kb_len)
		return;
	while (used > 0 && (kb_text[used - 1] & 0xC0) == 0x80)   /* bytes de continuacion de UTF-8 */
		used--;
	kb_text[used - 1] = 0;
	kb_len--;
}

/* Tecla tocada con el dedo (DS, 3DS, Vita): selecciona y activa la tecla de una vez */
static int kb_tap(int x, int y)
{
	int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;

	if (col < 0 || col >= KB_COLS || row < 0 || row >= KB_ROWS)
		return 0;                    /* fuera del teclado */
	kb_row = row;
	kb_col = col;
	if (row < KB_ROWS - 1) {
		kb_type(kb_keys[kb_upper][row][col]);
		return 0;
	}
	switch (col * 4 / KB_COLS) {      /* la fila de acciones, igual que con CROSS */
	case 0:
		kb_upper = !kb_upper;
		break;
	case 1:
		kb_type(" ");
		break;
	case 2:
		kb_erase();
		break;
	default:
		return 1;
	}
	return 0;
}

static int kb_update(void)
{
	int tx, ty;

	if (input_touch(&tx, &ty))
		return kb_tap(tx, ty);
	if (input_pressed(BTN_UP))
		kb_row = (kb_row + KB_ROWS - 1) % KB_ROWS;
	if (input_pressed(BTN_DOWN))
		kb_row = (kb_row + 1) % KB_ROWS;
	if (input_pressed(BTN_LEFT))
		kb_col = (kb_col + KB_COLS - 1) % KB_COLS;
	if (input_pressed(BTN_RIGHT))
		kb_col = (kb_col + 1) % KB_COLS;
	if (input_pressed(BTN_L1))
		kb_upper = !kb_upper;
	if (input_pressed(BTN_CIRCLE))
		kb_erase();
	if (input_pressed(BTN_START))
		return 1;
	if (!input_pressed(BTN_CROSS))
		return 0;
	if (kb_row < KB_ROWS - 1) {
		kb_type(kb_keys[kb_upper][kb_row][kb_col]);
		return 0;
	}
	switch (kb_col * 4 / KB_COLS) {
	case 0:
		kb_upper = !kb_upper;
		break;
	case 1:
		kb_type(" ");
		break;
	case 2:
		kb_erase();
		break;
	default:
		return 1;
	}
	return 0;
}

static void kb_key(int x, int y, int w, const char *label, int sel)
{
	font_draw(LAYER_TEXT, x + (w - font_width(label)) / 2, y + SYS_Y(2), label, sel ? 255 : 128, sel ? 220 : 128, sel ? 40 : 128);
	video_rect(LAYER_TEXT, x + SYS_X(1), y, w - SYS_X(2), KB_H - SYS_Y(2), sel ? 90 : 40, sel ? 70 : 40, sel ? 10 : 40, 0);
}

static void kb_draw(void)
{
	int r, c, action = kb_row == KB_ROWS - 1 ? kb_col * 4 / KB_COLS : -1;

	panel();
	draw_center(SYS_Y(28), TX(T_KB_TITLE), 255, 220, 40);
	font_draw(LAYER_TEXT, SYS_X(24), SYS_Y(56), kb_text, 128, 128, 128);
	if ((video_frames() / 20) & 1)
		video_rect(LAYER_TEXT, SYS_X(25) + font_width(kb_text), SYS_Y(57), SYS_X(2), font_line_height() - SYS_Y(2), 255, 220, 40, 0);
	video_rect(LAYER_TEXT, SYS_X(16), SYS_Y(52), SCREEN_W - SYS_X(32), KB_H + SYS_Y(2), 30, 30, 30, 0);
	for (r = 0; r < KB_ROWS - 1; r++)
		for (c = 0; c < KB_COLS; c++)
			kb_key(KB_X + c * KB_W, KB_Y + r * KB_H, KB_W, kb_keys[kb_upper][r][c], r == kb_row && c == kb_col);
	for (c = 0; c < 4; c++)
		kb_key(KB_X + c * (KB_W * KB_COLS / 4), KB_Y + (KB_ROWS - 1) * KB_H, KB_W * KB_COLS / 4,
		       c == 0 && kb_upper ? TX(T_KB_SHIFT_ON) : kb_actions[c], c == action);
	draw_center(KB_HELP_Y, TX(T_KB_HELP), 96, 96, 96);
}

/* Prueba automatica: escribe un texto de ejemplo y lo deja un segundo en pantalla (para las capturas) */
static int kb_autoplay(void)
{
	if (!kb_len) {
		const char *s = TX(T_KB_SAMPLE);

		for (; *s; s++) {
			char c[2] = { *s, 0 };

			kb_type(c);
		}
		kb_row = KB_ROWS - 1;
		kb_col = KB_COLS - 1;
		kb_wait = 0;
	}
	return ++kb_wait >= 60;
}

const Special special_keyboard = { 0, kb_begin, kb_update, kb_draw, kb_autoplay };
