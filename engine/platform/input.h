/* Control: botones con deteccion de pulsacion. */
#pragma once

#include <stdint.h>

/* Botones: mismos bits que el control de PS1 (psxpad.h); cada plataforma traduce los suyos a estos */
enum {
	BTN_SELECT   = 1 << 0,
	BTN_L3       = 1 << 1,
	BTN_R3       = 1 << 2,
	BTN_START    = 1 << 3,
	BTN_UP       = 1 << 4,
	BTN_RIGHT    = 1 << 5,
	BTN_DOWN     = 1 << 6,
	BTN_LEFT     = 1 << 7,
	BTN_L2       = 1 << 8,
	BTN_R2       = 1 << 9,
	BTN_L1       = 1 << 10,
	BTN_R1       = 1 << 11,
	BTN_TRIANGLE = 1 << 12,
	BTN_CIRCLE   = 1 << 13,
	BTN_CROSS    = 1 << 14,
	BTN_SQUARE   = 1 << 15
};

void input_init(int with_card);
void input_update(void);
int input_down(uint16_t mask);       /* esta presionado ahora */
int input_pressed(uint16_t mask);    /* se acaba de presionar en este cuadro */
int input_raw_down(uint16_t mask);   /* presionado, leido directo del control (antes del bucle principal) */
/* Stick analogico: valores en [-127,127], 0 si la consola no tiene (el chat del telefono lo usa para desplazar) */
int input_stick_x(void);
int input_stick_y(void);
/* Pantalla tactil, en coordenadas del guion (320x240, las mismas en las que se dibuja); 0 si no hay tactil.
 * input_touch_down: 1 mientras el dedo esta apoyado (arrastrar el chat, modules/strips.c).
 * input_touch: 1 solo en el cuadro en que se toca (tocar una tecla, modules/keyboard.c). */
int input_touch_down(int *x, int *y);
int input_touch(int *x, int *y);
/* Prueba automatica: botones fijos por cuadro (AUTOTEST.BIN del disco, ver tools/engine/emutest.py) */
void input_set_script(const uint8_t *entries, int count);
/* Siguiente decision que fija el guion para el recorrido (en orden), o -1 si no quedan: autoplay sigue una ruta */
int input_autotest_choice(void);
/* El guion puede activar el modo autoplay (prueba de recorrido) y pedir salir del emulador */
extern volatile uint32_t autotest_autoplay, autotest_quit;
