/*
 * Contrato C2: lo que el nucleo pide a cada plataforma. El nucleo no incluye nada de una consola; solo llama a
 * estas operaciones a traves de vn_ops. Una operacion en NULL significa que el backend aun no la ofrece.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Capacidades declaradas por cada plataforma. */
typedef struct VnPlatformCaps {
	int screen_w;
	int screen_h;
	int max_sprites;
	int vram_bytes;
	int ram_bytes;
	int audio_channels;
	int has_video;   /* 1 si soporta video */
	int has_touch;   /* 1 si tiene tactil */
	int color_depth; /* 4, 8, 16, 32 */
} VnPlatformCaps;

#define VN_FONT_ATLAS_W 256      /* atlas de la fuente: 256x256 a 4 bits, indice 0 transparente */
#define VN_FONT_ATLAS_H 256

typedef struct VnPlatformOps {
	/* archivos: `name` es el nombre corto del disco ("C01.VNB"); el backend sabe en que carpeta estan */
	int (*file_size)(const char *name);                          /* bytes, -1 si no existe */
	int (*file_read)(const char *name, void *buf, int buf_bytes); /* bytes leidos, -1 si no existe o no cabe */
	/* fuente: el atlas viene del FILE leido por font_load (FNT1 son 256x256; FNT2 trae sus dimensiones, que
	 * cambian con el cuerpo del perfil de la consola). `w`/`h` van en pixeles. */
	void (*font_upload)(const uint8_t *atlas_4bpp, int w, int h);
	/* glifo (u, v, w, h) del atlas en (x, y) de pantalla; color es un tinte (128 = blanco), mezcla aditiva */
	void (*draw_glyph)(int layer, int x, int y, int u, int v, int w, int h, int r, int g, int b);
	void (*glyphs_end)(int layer);                               /* fin de un texto en la capa */
	/* graficos */
	void (*clear)(void);
	void (*draw_sprite)(int id, int x, int y, int w, int h, const void *tex);
	void (*present)(void);
	/* audio */
	void (*play_music)(int track_id, int loop);
	void (*play_sfx)(int sfx_id);
	/* entrada */
	int (*poll_input)(void);
	/* almacenamiento */
	int (*save)(const void *data, size_t size, int slot);
	int (*load)(void *data, size_t size, int slot);
	/* debug/log */
	void (*log)(const char *msg);
	/* tiempo */
	unsigned int (*get_time_ms)(void);
	void (*sleep_ms)(unsigned int ms);
} VnPlatformOps;

/* Backend activo; se fija una vez al arrancar, antes de llamar al nucleo. */
extern const VnPlatformOps *vn_ops;
void vn_platform_set(const VnPlatformOps *ops);
/* Operaciones del backend con el que se enlaza este ejecutable (cada plataforma define la suya) */
const VnPlatformOps *vn_platform_default_ops(void);
