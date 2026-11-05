#include "vn_platform.h"
#include "cd.h"
#include "sdl_target.h"
#include "video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern SDL_Renderer *sdl_renderer(void);
extern SDL_Texture *sdl_font_texture(void);
extern void sdl_set_font(SDL_Texture *);
extern void sdl_draw_texture(SDL_Texture *, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int,
                             int, int, int); /* los ultimos enteros son slot de textura y alfa */
static uint32_t started;
#if SDL_TARGET_PSP_MODULE_INFO
/* Sin SDL2main (el main es el del motor): la info de modulo y la pila de user_main van aqui (crt0 de pspsdk). */
PSP_MODULE_INFO("snoot", 0, 1, 0);
int sce_newlib_stack_kb_size = 1024;
#endif
static FILE *open_file(const char *n)
{
	/* cd_root() es la raiz que resolvio cd_init: en PSP puede venir de ef0: (PSP Go) y no del macro de
	 * compilacion. Se declara aqui porque cd.h es contrato y no describe rutas. */
	extern const char *cd_root(void);
	char p[600];
	snprintf(p, sizeof(p), "%s/%s", cd_root(), n);
	return fopen(p, "rb");
}
static int file_size(const char *n)
{
	FILE *f = open_file(n);
	long z;
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	z = ftell(f);
	fclose(f);
	return (int)z;
}
static int file_read(const char *n, void *b, int cap)
{
	FILE *f = open_file(n);
	long z;
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	z = ftell(f);
	rewind(f);
	if (z > cap || fread(b, 1, (size_t)z, f) != (size_t)z) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return (int)z;
}
static void font_upload(const uint8_t *src, int w, int h)
{
#ifdef VN_PSP_NATIVE_MENU
	/* El atlas ya tiene alfa de 4 bits: RGBA4444 conserva toda su precisión.
	 * Evita expandir 512x256 a medio MiB RGBA32 en PSP. */
	uint16_t *pixels;
	SDL_Texture *t;
	int i;
	if (w <= 0 || h <= 0)
		return;
	pixels = malloc((size_t)w * (size_t)h * sizeof(*pixels));
	if (!pixels)
		return;
	for (i = 0; i < w * h; i++) {
		int byte = src[i / 2], a = (i & 1) ? byte >> 4 : byte & 15;
		pixels[i] = (uint16_t)((a << 12) | 0x0fff);
	}
	t = SDL_CreateTexture(sdl_renderer(), SDL_PIXELFORMAT_ABGR4444, SDL_TEXTUREACCESS_STATIC, w, h);
	if (t && SDL_UpdateTexture(t, NULL, pixels, w * (int)sizeof(*pixels)) != 0) {
		SDL_DestroyTexture(t);
		t = NULL;
	}
	free(pixels);
	sdl_set_font(t);
	printf("font: PSP RGBA4444 %dx%d raw=%lu bytes %s\n", w, h, (unsigned long)w * h * 2,
	       t ? "OK" : SDL_GetError());
#else
	uint32_t *pixels;
	SDL_Surface *s;
	SDL_Texture *t;
	int x, y;

	if (w <= 0 || h <= 0)
		return;
	pixels = malloc((size_t)w * (size_t)h * 4);
	if (!pixels)
		return;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int i = y * w + x, byte = src[i / 2], a = (i & 1) ? byte >> 4 : byte & 15;
			pixels[i] = ((uint32_t)(a * 17) << 24) | 0x00ffffffu;
		}
	s = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
	t = s ? SDL_CreateTextureFromSurface(sdl_renderer(), s) : NULL;
	if (s)
		SDL_FreeSurface(s);
	free(pixels);
	sdl_set_font(t);
#endif
}
static void draw_glyph(int l, int x, int y, int u, int v, int w, int h, int r, int g, int b)
{
	SDL_Texture *t = sdl_font_texture();
	if (t)
		sdl_draw_texture(t, l, x, y, w, h, u, v, w, h, r * 2, g * 2, b * 2, 1, 0, 64, 0, 0, 255);
	else
		video_rect(l, x, y, w, h, r, g, b, 0);
}
static void glyphs_end(int l)
{
	(void)l;
}
static void log_line(const char *s)
{
#ifdef VN_TRACE_FILE
	static FILE *trace_file;
	static unsigned int trace_lines;
	if (!trace_file) {
		char path[600];
		extern const char *cd_root(void);
		snprintf(path, sizeof(path), "%s/TRACE.TXT", cd_root());
		trace_file = fopen(path, "wb");
	}
	if (trace_file) {
		fprintf(trace_file, "%s\n", s);
		if (!(++trace_lines & 31) || strstr(s, "|END"))
			fflush(trace_file);
	} else {
		puts(s);
		SDL_TARGET_FLUSH_STDOUT();
	}
#else
	puts(s);
	SDL_TARGET_FLUSH_STDOUT();
#endif
}
static unsigned int time_ms(void)
{
	return SDL_GetTicks() - started;
}
static void sleep_ms(unsigned int n)
{
	if (getenv("VN_FAST"))
		return;
	SDL_Delay(n);
}
static const VnPlatformOps ops = {.file_size = file_size,
                                  .file_read = file_read,
                                  .font_upload = font_upload,
                                  .draw_glyph = draw_glyph,
                                  .glyphs_end = glyphs_end,
                                  .log = log_line,
                                  .get_time_ms = time_ms,
                                  .sleep_ms = sleep_ms};
const VnPlatformOps *vn_platform_default_ops(void)
{
	SDL_TARGET_FAT_INIT();
	started = SDL_GetTicks();
	return &ops;
}
void fmv_select(int file)
{
	(void)file;
}
