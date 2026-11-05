#include "game.h"

/* Snoot Game, decision D1 de snoot-ps1: con 0 finales no hay extras; con el primero, x1-x3. Cada final visto
 * muestra uno mas de la serie de Trish (x4-x6) y otro de la de Naomi (x7-x9). x10 pide los 4 finales y los 9 extras. */
int game_extra_unlocked(int i, uint32_t endings_mask, uint32_t bonus_mask)
{
	int e = endings_mask & 15, seen_count = 0, w, k;

	if (e == 0)
		return 0;
	if (e == 15)
		return i < 9 || (bonus_mask & 0x1ff) == 0x1ff;
	for (k = 0; k < 4; k++)
		seen_count += (e >> k) & 1;
	w = 4 - seen_count;                              /* se esconden w de Trish (x4-x6) y w de Naomi (x7-x9) desde el final */
	if (i == 9 || (i >= 6 - w && i <= 5) || (i >= 9 - w && i <= 8))
		return 0;
	return 1;
}
