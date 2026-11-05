#include "special.h"

/* Snoot Game: 1 = chat del capitulo 4 (`call screen textscroll`), 2 = creditos (`label lending`), 3 = teclado del 13D */
const SpecialEntry game_specials[] = {
	{ 1, &special_strips },
	{ 2, &special_credits },
	{ 3, &special_keyboard },
};
const int game_special_count = sizeof(game_specials) / sizeof(game_specials[0]);
