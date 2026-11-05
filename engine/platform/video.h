/* Video: lo que la app y el nucleo dibujan por capas. Cada plataforma lo implementa (API por enlace, PLAN.md D15);
 * lo propio de la GPU de PS1 (primitivas, VRAM, CLUT) esta en platforms/ps1/video_ps1.h. */
#pragma once

#include <stdint.h>

/* La vista la fija el perfil de la consola (docs/perfiles.md, D29). Sin perfil puesto por el build quedan los
 * numeros de PS1, que son los que producen el disco de v1.0: pantalla 4:3 de 320x240 con la imagen 16:9 de
 * 320x180 centrada. Una consola nativa los sobreescribe desde su build (-DSCREEN_W=480 -DSCREEN_H=272
 * -DVIEW_Y=0 -DVIEW_H=272 en PSP, por ejemplo). */
#ifndef SCREEN_W
#define SCREEN_W 320
#endif
#ifndef SCREEN_H
#define SCREEN_H 240
#endif
#ifndef VIEW_Y
#define VIEW_Y 30
#endif
#ifndef VIEW_H
#define VIEW_H 180
#endif

#define VIEW_W SCREEN_W /* el letterbox de PS1 es vertical: la vista ocupa todo el ancho */

/* Coordenadas de las pantallas de sistema diseñadas sobre el lienzo PS1 de 320x240.
 * Mantienen ese resultado en PS1 y escalan a la resolucion fisica de cada perfil. */
#define SYS_X(px) ((px) * (int)SCREEN_W / 320)
#define SYS_Y(px) ((px) * (int)SCREEN_H / 240)

/* El guion habla en pixeles del lienzo de 1920x1080 con el que compila Ren'Py
 * (snoot-ps1/tools/script_compiler/opcodes.py: "tx, ty en pixeles de 1920x1080"). Para la vista de cada consola hay
 * que convertir con SU ancho. Con VIEW_W=320 esto es exactamente dividir entre 6, el numero de siempre de PS1
 * (1920 = 320*6, y la division entera coincide digito a digito incluso con negativos), pero dejar el /6 fijo dejaba
 * la camara 1,5x corta en PSP y 3x en Vita: T-645. */
#define VN_SRC_W   1920
#define SRC_PX(px) ((px) * (int)VIEW_W / VN_SRC_W)

/* Todo lo que estaba escrito en pixel de PS1 se expresa ahora como fraction de la pantalla, de forma que con el
 * perfil de PS1 el resultado sea el mismo numero de siempre: la caja ocupa el 30 % del alto (72 px -> fila 168). */
#if defined(VN_PSP_NATIVE_MENU) || defined(VN_VITA_NATIVE_MENU)
#define BOX_H (SCREEN_H * 278 / 1080) /* gui.textbox_height del original */
#else
#define BOX_H (SCREEN_H * 3 / 10)
#endif
#define BOX_Y (SCREEN_H - BOX_H)

/* Capas: 0 se dibuja al final (al frente). Dentro de una capa, lo agregado despues se dibuja antes. */
#define LAYER_DEBUG   0
#define LAYER_TEXT    1
#define LAYER_BOX     2
#define LAYER_FX      3 /* transiciones: sobre la escena, debajo de la caja de texto */
#define LAYER_SPRITE0 4 /* 4..19: un sprite por capa (16 como VM_MAX_SPRITES); el menor numero queda al frente */
#define LAYER_BG      20
#define LAYER_COUNT   21

void video_init(void);
void video_rect(int layer, int x, int y, int w, int h, int r, int g, int b, int blend);
void video_triangle_down(int layer, int x, int y, int w, int h, int r, int g, int b);
void video_flip(void);
/* Copia de la pantalla para las transiciones, en el lugar que dio res_reserve_snapshot */
void video_snapshot(int x, int y);
/* Fin de la transicion: deja de congelar la copia. Obligatoria con el snapshot por re-emision (T-633), porque si no
 * el cuadro guardado no se vuelve a actualizar nunca y la siguiente transicion cruza contra una escena vieja. */
void video_release_snapshot(void);
/* bright: modulacion de color (128 = normal, 0 = negro); desde la fila band_y se usa band_bright */
void video_snapshot_draw(int layer, int x, int y, int blend, int bright, int band_y, int band_bright);
int video_frames(void);
void video_clip_scene(void); /* llamar despues de agregar la escena (se dibuja primero) */
void video_unclip(void);     /* llamar despues de agregar la caja de texto y la interfaz */
/* Retrazado vertical: esperar el siguiente y contador desde el arranque (60 por segundo en NTSC) */
void video_wait_vblank(void);
int video_vblank_count(void);
