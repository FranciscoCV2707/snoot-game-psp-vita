#ifndef SPECIAL_H
#define SPECIAL_H

#include <stdint.h>

/* Pantallas especiales: lo que la instruccion SPECIAL id del guion abre (un chat, los creditos, un teclado...).
 * Cada una es un modulo de modules/; cada juego dice cuales usa y con que id en games/<juego>/specials.c. */

enum {
	SP_IN_GAME = 1,      /* cuenta como parte del juego (Select muestra la depuracion, R1 salta, suenan las animaciones) */
	SP_OWN_R1 = 2,       /* usa R1 para si misma: no activa el salto de texto */
	SP_OWN_SCENE = 4,    /* dibuja la pantalla entera: no se dibuja la escena detras */
};

typedef struct {
	unsigned flags;
	int (*begin)(void);        /* 0 si no tiene que mostrar (la instruccion se ignora) */
	int (*update)(void);       /* 1 al terminar: vuelve el guion */
	void (*draw)(void);
	int (*autoplay)(void);     /* prueba automatica: 1 cuando ya se puede seguir; NULL = seguir enseguida */
} Special;

/* Tabla de cada juego: que modulo abre cada id. El orden da el estado del motor (la primera es ST_SPECIAL). */
typedef struct {
	int id;
	const Special *special;
} SpecialEntry;

extern const SpecialEntry game_specials[];
extern const int game_special_count;

/* Tiras de imagenes que se desplazan (el chat del capitulo 4 de Snoot): `count` imagenes de 320x180 desde `first` */
extern const Special special_strips;
void strips_setup(int first, int count);

/* Creditos que suben solos */
extern const Special special_credits;
void credits_clear(void);
void credits_add(int style, const char *text, int len);   /* estilo 0 nombre, 1 encabezado, 2 titulo */

/* Teclado en pantalla (renpy.input) */
extern const Special special_keyboard;

/* Chat de telefono con burbujas (Wani): OP_PHONE llama a phone_clear y phone_add; luego la instruccion SPECIAL
 * abre el chat. side 0 = el otro personaje, 1 = la protagonista; img 0 = sin foto */
extern const Special special_phone;
void phone_clear(void);
void phone_add(int side, const char *text, int img);

/* Video STR (Wani): reproduce \VIDEO.STR del CD como pantalla especial; X sale (ver modules/fmv.c) */
extern const Special special_fmv;
void fmv_select(int file);     /* la siguiente vez que se abre special_fmv reproduce V<file>.STR (-1: VIDEO.STR) */

/* Lienzo de dibujo (Wani, cap. 3): la cruceta mueve el cursor y X traza. canvas_bits saca el dibujo en 1 bit
 * por pixel (128x72, 1152 bytes) para la memory card; devuelve los bytes escritos */
extern const Special special_canvas;
int canvas_bits(uint8_t *out);
void canvas_load(const uint8_t *in);

#endif
