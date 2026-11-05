#include <string.h>
#include "input.h"
#include "special.h"
#include "video.h"

/* Lienzo de dibujo con cursor (Wani, cap. 3): la cruceta mueve el cursor, X traza mientras se mantiene, O deshace
 * el ultimo trazo, L1 cambia el grosor, Start termina. El dibujo es un mapa de 1 bit por pixel de 128x72 (1152
 * bytes): eso es lo que se guarda en la memory card (canvas_bits / canvas_load). En pantalla va a doble tamano. */
#define CV_W 128
#define CV_H 72
#define CV_SCALE (VIEW_W * 4 / (5 * CV_W))
#define CV_X ((SCREEN_W - CV_W * CV_SCALE) / 2)
#define CV_Y (VIEW_Y + (VIEW_H - CV_H * CV_SCALE) / 2)
#define CV_BYTES (CV_W * CV_H / 8)

#define CV_GET(b, x, y) ((b)[((y) * CV_W + (x)) / 8] & (0x80 >> (((y) * CV_W + (x)) % 8)))
#define CV_SET(b, x, y) ((b)[((y) * CV_W + (x)) / 8] |= (uint8_t)(0x80 >> (((y) * CV_W + (x)) % 8)))

static uint8_t cv[CV_BYTES], cv_undo[CV_BYTES];
static int cur_x, cur_y, cur_thick = 1, drawing;

/* Cursor al centro y el dibujo que haya (guardado o de antes) se queda */
static int canvas_begin(void)
{
	cur_x = CV_W / 2;
	cur_y = CV_H / 2;
	drawing = 0;
	return 1;
}

static void stamp(void)
{
	int dx, dy, x, y;

	for (dy = 0; dy < cur_thick; dy++)
		for (dx = 0; dx < cur_thick; dx++) {
			x = cur_x + dx;
			y = cur_y + dy;
			if (x < CV_W && y < CV_H)
				CV_SET(cv, x, y);
		}
}

static int canvas_update(void)
{
	if (input_down(BTN_LEFT) && cur_x > 0)
		cur_x--;
	if (input_down(BTN_RIGHT) && cur_x < CV_W - 1)
		cur_x++;
	if (input_down(BTN_UP) && cur_y > 0)
		cur_y--;
	if (input_down(BTN_DOWN) && cur_y < CV_H - 1)
		cur_y++;

	if (input_pressed(BTN_L1))
		cur_thick = cur_thick % 3 + 1;
	if (input_pressed(BTN_CROSS)) {
		memcpy(cv_undo, cv, CV_BYTES);
		drawing = 1;
	}
	if (drawing && input_down(BTN_CROSS))
		stamp();
	else
		drawing = 0;
	if (input_pressed(BTN_CIRCLE) && !drawing)
		memcpy(cv, cv_undo, CV_BYTES);
	return input_pressed(BTN_START);
}

/* Cada fila se dibuja en tiras: una tira por cada racha de pixeles seguidos */
static void canvas_draw(void)
{
	int x, y;

	video_rect(LAYER_BG, CV_X - 2, CV_Y - 2, CV_W * CV_SCALE + 4, CV_H * CV_SCALE + 4, 255, 255, 255, 0);
	video_rect(LAYER_BG, CV_X, CV_Y, CV_W * CV_SCALE, CV_H * CV_SCALE, 0, 0, 0, 0);
	for (y = 0; y < CV_H; y++) {
		x = 0;
		while (x < CV_W) {
			int start = x;

			while (x < CV_W && CV_GET(cv, x, y))
				x++;
			if (x > start)
				video_rect(LAYER_SPRITE0, CV_X + start * CV_SCALE, CV_Y + y * CV_SCALE, (x - start) * CV_SCALE,
				           CV_SCALE, 200, 200, 200, 0);
			x++;
		}
	}
	video_rect(LAYER_TEXT, CV_X + cur_x * CV_SCALE, CV_Y + cur_y * CV_SCALE, CV_SCALE, CV_SCALE, 255, 255, 0, 0);
}

const Special special_canvas = { SP_OWN_R1 | SP_OWN_SCENE, canvas_begin, canvas_update, canvas_draw, 0 };

/* El dibujo a 1 bit por pixel (fila a fila, bit 7 = primer pixel) para la memory card */
int canvas_bits(uint8_t *out)
{
	memcpy(out, cv, CV_BYTES);
	return CV_BYTES;
}

/* Carga un dibujo guardado; deshacer no lo lleva mas atras de lo cargado */
void canvas_load(const uint8_t *in)
{
	memcpy(cv, in, CV_BYTES);
	memcpy(cv_undo, in, CV_BYTES);
}
