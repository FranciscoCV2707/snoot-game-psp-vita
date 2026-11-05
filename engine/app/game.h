#ifndef GAME_H
#define GAME_H

#include <stdint.h>

/* Reglas propias de cada juego que no son datos simples. Cada juego trae su games/<juego>/game.c;
 * games/default/game.c sirve a los juegos que no necesitan reglas propias. */

/* Si el capitulo extra numero `index` (orden del menu) esta disponible, segun los finales vistos (bit n = final n+1)
 * y los extras terminados (bit n = extra n). */
int game_extra_unlocked(int index, uint32_t endings_mask, uint32_t bonus_mask);

#endif
