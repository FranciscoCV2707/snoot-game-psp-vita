#include "img.h"
#include "sdl_target.h"
#include "video.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lz4.h"

extern void sdl_draw_texture(SDL_Texture *, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int,
                             int, int, int);
extern SDL_Renderer *sdl_renderer(void);
static SDL_Texture *textures[4096];
int img_brightness = 256;

/* Sigue viva la textura que esta en ese slot? El snapshot por re-emision de video.c (T-633) guarda los dibujos del
 * cuadro anterior con su textura, y si la caché desalojo esa imagen mientras dura la transicion el puntero cuelga.
 * Slot 0 = fuera de la tabla (la fuente), que vive en video.c y no se destruye aqui. */
int texture_slot_live(int slot, SDL_Texture *t)
{
	if (slot <= 0 || slot >= (int)(sizeof(textures) / sizeof(textures[0])))
		return t != 0;
	return textures[slot] == t;
}

/* Como esta empaquetado el pixel segun el perfil de la consola (docs/formato-img.md): los bits 3-4 de flags dicen el
 * formato cuando no hay paleta de 256 (`ncolors` = 0). En PSP el fondo va en 565 y el sprite en 4444 porque 565 no
 * tiene alfa; en las consolas con VRAM de sobra todo va en 32 bits. */
#define FMT_SHIFT   3
#define FMT_MASK    3
#define FMT_PAL     0
#define FMT_565     1
#define FMT_4444    2
#define FMT_8888    3
#if defined(VN_PSP_NATIVE_MENU)
/* El renderer GE puede reorganizar una textura despues del primer dibujo. Los fondos grandes que se actualizan
 * mientras corre una panoramica deben dividirse en texturas estaticas, igual que los sprites grandes. */
#define IMG_TILE_LARGE_OPAQUE 1
#else
#define IMG_TILE_LARGE_OPAQUE 0
#endif
#define PAL_TAIL    (24 + 512) /* donde estan las dos u32 de tamano cuando la paleta va detras del header */
#define NO_PAL_TAIL 24         /* y donde estan cuando no hay paleta */

static unsigned rd16(const uint8_t *p)
{
	return p[0] | ((unsigned)p[1] << 8);
}
static unsigned rd32(const uint8_t *p)
{
	return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int img_fmt(const uint8_t *f)
{
	return (f[5] >> FMT_SHIFT) & FMT_MASK;
}
static int img_tail(const uint8_t *f)
{
	return rd16(f + 22) ? PAL_TAIL : NO_PAL_TAIL;
}

/* Paleta IMG1: 256 palabras RGB555 en f+24. El indice 0 es transparente si flags trae el bit 1 y un color con el
 * bit 0x8000 se dibuja al 50 % cuando IMG_TRANSLUCENT esta activo. */
static uint32_t pal_pixel(const uint8_t *pal, unsigned ix, uint8_t flags)
{
	unsigned c = rd16(pal + ix * 2);
	unsigned r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
	unsigned a = (ix == 0 && (flags & 1)) ? 0 : 255;
	if ((flags & IMG_TRANSLUCENT) && (c & 0x8000))
		a = 128;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

/* Tabla de 256 entradas por imagen: expandir la paleta con las tres divisiones de pal_pixel costaba decenas de
 * milisegundos por carga en PSP (57 600 pixeles en cada subida de la ventana grande tambien), y en un MIPS sin
 * divisor es lo unico que se paga por pixel. */
static void pal_lut(const uint8_t *pal, uint8_t flags, uint32_t *lut)
{
	int i;

	for (i = 0; i < 256; i++)
		lut[i] = pal_pixel(pal, i, flags);
}

/* Pixel de un perfil en color verdadero expandido a RGBA32, para la ventana de las panoramicas. Multiplicar por 8
 * y sumar los bits altos: es lo mismo que *255/31 y *255/15 sin divisor (D28). */
static uint32_t true_pixel(const uint8_t *p, int fmt)
{
	unsigned c, r, g, b, a = 255;

	if (fmt == FMT_8888)
		return rd32(p); /* ya viene en R,G,B,A = SDL_PIXELFORMAT_RGBA32 */
	c = rd16(p);
	if (fmt == FMT_4444) {
		r = ((c >> 12) & 15) * 17;
		g = ((c >> 8) & 15) * 17;
		b = ((c >> 4) & 15) * 17;
		a = (c & 15) * 17;
	} else {
		r = ((c >> 11) & 31) * 8 + 4;
		g = ((c >> 5) & 63) * 4 + 2;
		b = (c & 31) * 8 + 4;
	}
	return (a << 24) | (b << 16) | (g << 8) | r;
}

static int read_header(Image *img, const uint8_t *f, int n)
{
	int tail;

	img->loaded = 0;
	img->big = 0;
	if (n < 24 || memcmp(f, "IMG1", 4))
		return -1;
	/* Con perfil en color verdadero no va paleta: `ncolors` vale 0 y el bloque de 512 bytes desaparece. */
	tail = rd16(f + 22) ? PAL_TAIL : NO_PAL_TAIL;
	if (n < tail + 8)
		return -1;
	img->kind = f[4];
	img->flags = f[5];
	img->full_w = rd16(f + 6);
	img->full_h = rd16(f + 8);
	img->x = rd16(f + 10);
	img->y = rd16(f + 12);
	img->w = rd16(f + 14);
	img->h = rd16(f + 16);
	memcpy(img->solid, f + 18, 4);
	return 0;
}

/* Lado mas largo de la textura que admite el renderer; 0 = sin limite (software y dummy no lo reportan).
 * VN_MAX_TEX lo fija en las pruebas de PC para dibujar una consola chica sin encender su emulador. */
static int max_texture_dim(void)
{
	static int cached = -1;
	const char *env = getenv("VN_MAX_TEX");
	SDL_RendererInfo info;
	int w, h;

	if (cached > 0)
		return cached;
	if (env && *env) {
		cached = atoi(env);
		return cached;
	}
#ifdef VN_PSP_NATIVE_MENU
	/* También en el banco de pruebas: ejercitar el mismo camino de panorámicas que PSP. */
	return 512;
#endif
	if (!sdl_renderer() || SDL_GetRendererInfo(sdl_renderer(), &info) != 0)
		return 0;
	w = info.max_texture_width;
	h = info.max_texture_height;
	if (w <= 0 || h <= 0)
		return 0;
	cached = w < h ? w : h;
	return cached;
}

/* Cota conservadora de memoria de una entrada PSP: SDL redondea a potencias de
 * dos y puede convertir el formato. Cuenta 4 B/texel hasta medir el driver fijado.
 * Cada imagen grande guarda su bitmap y una ventana de textura independiente. */
uint32_t img_memory_bound(const uint8_t *file, int size)
{
	Image image = {0};
	unsigned w = 2, h = 2, bpp;
	if (read_header(&image, file, size) != 0)
		return 0;
	if (image.kind > 1 || !image.w || !image.h)
		return 0;
	if (image.w > 512 || image.h > 512) {
		if (image.kind == 1 || IMG_TILE_LARGE_OPAQUE) {
			/* Transparencias grandes se parten en texturas <=512, sin cargar una copia RGBA entera. */
			unsigned x, y, total = 0,
			               tile_bpp = img_fmt(file) == FMT_PAL || img_fmt(file) == FMT_8888 ? 4 : 2;
			for (y = 0; y < image.h; y += 512)
				for (x = 0; x < image.w; x += 512) {
					unsigned tw = image.w - x < 512 ? image.w - x : 512;
					unsigned th = image.h - y < 512 ? image.h - y : 512;
					unsigned pw = 2, ph = 2;
					while (pw < tw)
						pw <<= 1;
					while (ph < th)
						ph <<= 1;
					total += pw * ph * tile_bpp;
				}
			return total;
		}
		while (w < BIG_WIN_W)
			w <<= 1;
		while (h < BIG_WIN_H)
			h <<= 1;
		bpp = 4;
#if defined(VN_PSP_NATIVE_MENU)
		if (img_fmt(file) == FMT_565)
			bpp = 2;
#endif
		return rd32(file + img_tail(file)) + w * h * bpp;
	}
	bpp = 4;
	if (image.w <= 512 && image.h <= 512) {
		/* Native PSP assets are 16-bit; SDL may wrap them in a 32-bit host texture. */
		bpp = 2;
	}
	while (w < image.w)
		w <<= 1;
	while (h < image.h)
		h <<= 1;
	return w * h * bpp;
}

/* ---------------------------------------------------------------- imagenes grandes (img_load_big)
 * En PSP y con transparencia, los bitmaps se dividen en texturas <=512. En los otros perfiles el bitmap completo
 * queda en RAM y se actualiza solo la ventana visible. Son los fondos que se recorren con Pan (stairs 1328x383 y
 * fanglookback 320x746), que no caben enteros en una textura PSP. Cada imagen mantiene su propio registro: el guion
 * precarga dos fondos grandes a la vez (ruta-1 carga stairs y fanglookback juntos). */
#define BIG_BYTES (4 * 1024 * 1024) /* en 16 bits, la panoramica de PSP (1992x574) ocupa 2,3 MB */
#define BIG_RECS  4                 /* registros simultaneos: acota la RAM que pueden ocupar las grandes */
#define BIG_SLOT  (4096 - BIG_RECS) /* registro por imagen grande; img_load no reparte estas ranuras */

typedef struct {
	const Image *owner; /* Image del cache dueña de estos datos; 0 = hueco libre */
	uint8_t *idx;       /* todo el bitmap: indices (fmt = FMT_PAL) o pixeles del perfil */
	uint8_t fmt;
	uint32_t lut[256]; /* paleta expandida una vez: la PSP no tiene divisor y se paga por pixel */
	SDL_Texture *window;
	Uint32 window_format;
	int window_bpp;
	int win_x, win_y, win_w, win_h;
	Image *tiles;
	int tile_count;
} BigRec;

static BigRec bigs[BIG_RECS];
static union {
	uint32_t rgba32[BIG_WIN_W * BIG_WIN_H];
	uint16_t native16[BIG_WIN_W * BIG_WIN_H];
} big_win;

static BigRec *big_of(const Image *img)
{
	int i;

	for (i = 0; i < BIG_RECS; i++)
		if (bigs[i].owner == img)
			return &bigs[i];
	return 0;
}

static BigRec *big_free_slot(void)
{
	int i;

	for (i = 0; i < BIG_RECS; i++)
		if (!bigs[i].owner)
			return &bigs[i];
	return 0;
}

static void big_drop(BigRec *rec)
{
	if (rec->window) {
#ifdef VN_PSP_TEXTURE_DIAG
		printf("img: destroy big-window rec=%d ptr=%p\n", (int)(rec - bigs), (void *)rec->window);
#endif
		SDL_DestroyTexture(rec->window);
		textures[BIG_SLOT + (rec - bigs)] = NULL;
	}
	free(rec->idx);
	if (rec->tiles) {
		int i;
		for (i = 0; i < rec->tile_count; i++) {
			int slot = rec->tiles[i].vram_x;
			if (slot > 0 && slot < BIG_SLOT && textures[slot]) {
#ifdef VN_PSP_TEXTURE_DIAG
				printf("img: destroy big-tile rec=%d tile=%d slot=%d ptr=%p\n", (int)(rec - bigs), i,
				       slot, (void *)textures[slot]);
#endif
				SDL_DestroyTexture(textures[slot]);
				textures[slot] = NULL;
			}
		}
		free(rec->tiles);
	}
	memset(rec, 0, sizeof(*rec));
}

/* Suelta lo que la entrada tuviera subido antes de cargar otra imagen en ella. Sin esto el registro de la ventana
 * quedaria apuntando a una ranura de la cache que ya es de otra imagen, y el descompresor escribiria
 * img->w*img->h bytes en el buffer de la anterior (acceso fuera de memoria que PPSSPP reporta en ruta-1). */
static void img_forget(Image *img)
{
	BigRec *rec = big_of(img);
#ifdef VN_TRACE
	if (img->vram_x)
		printf("img: forget slot=%u big=%u loaded=%u owner=%u\n", (unsigned)img->vram_x,
		       (unsigned)img->big, (unsigned)img->loaded, rec != NULL);
	#endif

	if (rec)
		big_drop(rec);
	if (img->vram_x > 0 && img->vram_x < BIG_SLOT && textures[img->vram_x]) {
#ifdef VN_PSP_TEXTURE_DIAG
		printf("img: destroy slot=%u ptr=%p big=%u loaded=%u owner=%u\n", (unsigned)img->vram_x,
		       (void *)textures[img->vram_x], (unsigned)img->big, (unsigned)img->loaded, rec != NULL);
#endif
		SDL_DestroyTexture(textures[img->vram_x]);
		textures[img->vram_x] = NULL;
	}
	img->vram_x = 0;
	img->big = 0;
}

static SDL_Texture *win_texture(BigRec *rec)
{
	if (!rec->window && sdl_renderer()) {
		/* PSP swizzles static textures on first draw; later linear updates corrupt them. */
		rec->window_format = SDL_PIXELFORMAT_RGBA32;
		rec->window_bpp = 4;
#if defined(VN_PSP_NATIVE_MENU)
		if (rec->fmt == FMT_565) {
			/* El renderer PSP solo reconoce BGR565; RGB565 usa la ruta RGBA8888
			 * aunque el buffer siga siendo de 16 bits. */
			rec->window_format = SDL_PIXELFORMAT_BGR565;
			rec->window_bpp = 2;
		}
#endif
		rec->window = SDL_CreateTexture(sdl_renderer(), rec->window_format, SDL_TEXTUREACCESS_STREAMING,
		                                BIG_WIN_W, BIG_WIN_H);
		if (rec->window)
			textures[BIG_SLOT + (rec - bigs)] = rec->window;
	}
	return rec->window;
}

/* Perfil en color verdadero: el bitmap ya son pixeles. Se sube en el formato nativo de la GPU (mitad de VRAM en las
 * consolas de 16 bits); si el driver de la consola no lo acepta, se expande a RGBA32 y se prueba con eso. */
static int texture_slot(SDL_Texture *t)
{
	int slot;

	for (slot = 1; slot < BIG_SLOT && textures[slot]; slot++)
		;
	if (slot >= BIG_SLOT)
		return -1;
	textures[slot] = t;
	return slot;
}

/* SDL's PSP renderer swizzles static textures in 8-row blocks and 16-byte
 * columns. Textures padded below either minimum can overrun its allocation
 * (for example pocky: 41x2 RGBA4444); streaming textures skip that swizzler. */
static SDL_TextureAccess true_texture_access(int w, int h, int bpp)
{
#if defined(__PSP__)
	int tw = 2, th = 2;
	while (tw < w)
		tw <<= 1;
	while (th < h)
		th <<= 1;
	if (tw * bpp < 16 || th < 8)
		return SDL_TEXTUREACCESS_STREAMING;
#else
	(void)w;
	(void)h;
	(void)bpp;
#endif
	return SDL_TEXTUREACCESS_STATIC;
}

static int img_upload_true(Image *img, uint8_t *px, int fmt);

static int img_load_true(Image *img, const uint8_t *src, int comp, uint32_t raw, int fmt)
{
	int bpp = fmt == FMT_8888 ? 4 : 2;
	uint8_t *px;
	if (raw != (uint32_t)img->w * img->h * bpp)
		return -1;
	px = malloc(raw);
	if (!px)
		return -1;
	if (lz4_decode(src, comp, px, (int)raw) != (int)raw) {
		free(px);
		return -1;
	}
	return img_upload_true(img, px, fmt);
}
static int img_upload_true(Image *img, uint8_t *px, int fmt)
{
	int bpp = fmt == FMT_8888 ? 4 : 2;
	/* IMG1 guarda RGB565 canónico. El renderer PSP solo admite BGR565, así
	 * que abajo se intercambian los extremos antes de subir la textura; el
	 * renderer SDL de escritorio debe recibir el formato RGB original. */
	Uint32 sf = fmt == FMT_565    ? SDL_PIXELFORMAT_RGB565
	            : fmt == FMT_4444 ? SDL_PIXELFORMAT_RGBA4444
	                              : SDL_PIXELFORMAT_RGBA32;
	const uint8_t *upload = px;
	uint8_t *native = NULL;
	uint32_t *rgba = 0;
	SDL_Texture *t = NULL;
	SDL_Surface *surface;
	int slot = -1, i;
	#if defined(__PSP__)
	if (fmt == FMT_565)
		sf = SDL_PIXELFORMAT_BGR565;
	else if (fmt == FMT_4444)
		sf = SDL_PIXELFORMAT_ABGR4444;
	if (fmt == FMT_565 || fmt == FMT_4444) {
		native = malloc((size_t)img->w * img->h * 2);
		if (native) {
			for (i = 0; i < (int)img->w * img->h; i++) {
				unsigned c = rd16(px + (size_t)i * 2), converted;
				if (fmt == FMT_565)
					converted = ((c & 31) << 11) | (c & 0x7e0) | (c >> 11);
				else
					converted = ((c & 15) << 12) | ((c & 0x00f0) << 4) |
					            ((c & 0x0f00) >> 4) | (c >> 12);
				native[i * 2] = (uint8_t)converted;
				native[i * 2 + 1] = (uint8_t)(converted >> 8);
			}
			upload = native;
		}
	}
	#endif
	t = SDL_CreateTexture(sdl_renderer(), sf, true_texture_access(img->w, img->h, bpp), img->w, img->h);
	if (t && SDL_UpdateTexture(t, NULL, upload, img->w * bpp) != 0) {
		SDL_DestroyTexture(t);
		t = NULL;
	}
	if (!t) {
		rgba = malloc((size_t)img->w * img->h * 4);
		if (rgba) {
			for (i = 0; i < (int)img->w * img->h; i++)
				rgba[i] = true_pixel(px + (size_t)i * bpp, fmt);
			surface = SDL_CreateRGBSurfaceWithFormatFrom(rgba, img->w, img->h, 32, img->w * 4,
			                                             SDL_PIXELFORMAT_RGBA32);
			if (surface) {
				t = SDL_CreateTextureFromSurface(sdl_renderer(), surface);
				SDL_FreeSurface(surface);
			}
		}
	}
	free(px);
	free(native);
	free(rgba);
	if (!t)
		return -1;
	slot = texture_slot(t);
	if (slot < 0) {
		SDL_DestroyTexture(t);
		return -1;
	}
	img->vram_x = (uint16_t)slot;
	img->loaded = 1;
	return 0;
}

int img_load(Image *img, const uint8_t *f, int n, int vx, int vy, int cy)
{
	uint32_t raw, comp;
	uint8_t *indices;
	uint32_t *pixels;
	uint32_t lut[256];
	SDL_Surface *surface;
	SDL_Renderer *r = sdl_renderer();
	int i, max, tail, fmt;

	(void)vx;
	(void)vy;
	(void)cy;
	img_forget(img);
	if (read_header(img, f, n) != 0)
		return -1;
	tail = img_tail(f);
	fmt = img_fmt(f);
	raw = rd32(f + tail);
	comp = rd32(f + tail + 4);
	if (img->kind > 1 || !img->w) {
		img->loaded = 1;
		return 0;
	}
	max = max_texture_dim();
	if (max > 0 && (img->w > max || img->h > max))
		return img_load_big(img, f, n, vx, vy, cy);
	if (fmt != FMT_PAL)
		return img_load_true(img, (const uint8_t *)f + tail + 8, (int)comp, raw, fmt);
	if (raw != (uint32_t)img->w * img->h || comp > (uint32_t)(n - tail - 8))
		return -1;
	indices = malloc(raw);
	pixels = malloc((size_t)img->w * img->h * 4);
	if (!indices || !pixels) {
		free(indices);
		free(pixels);
		return -1;
	}
	if (lz4_decode(f + tail + 8, (int)comp, indices, (int)raw) != (int)raw) {
		free(indices);
		free(pixels);
		return -1;
	}
	pal_lut(f + 24, img->flags, lut);
	for (i = 0; i < (int)raw; i++)
		pixels[i] = lut[indices[i]];
	surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, img->w, img->h, 32, img->w * 4, SDL_PIXELFORMAT_RGBA32);
	if (surface) {
		int slot;
		for (slot = 1; slot < BIG_SLOT && textures[slot]; slot++)
			;
		if (slot < BIG_SLOT) {
			textures[slot] = SDL_CreateTextureFromSurface(r, surface);
			/* si la subida falla (fuera de VRAM) la imagen no esta cargada: darla por buena dibujaria una textura nula */
			if (textures[slot])
				img->vram_x = (uint16_t)slot;
			else
				textures[slot] = NULL;
		}
		SDL_FreeSurface(surface);
	}
	free(indices);
	free(pixels);
	img->loaded = img->vram_x > 0;
	return img->loaded ? 0 : -1;
}
void img_release(Image *img)
{
	img_forget(img);
	img->loaded = 0;
}
static SDL_Texture *tex(const Image *i)
{
	return i->vram_x < 4096 ? textures[i->vram_x] : NULL;
}
static void draw_ex(const Image *i, int l, int x, int y, int flip, int zoom, int r, int g, int b, int angle, int alpha)
{
	int fw = i->full_w, scale = (i->flags & IMG_HALF) ? 2 : 1;
	BigRec *big = big_of(i);
	if (!i->loaded)
		return;
	if (big && big->tiles) {
		int n;
		for (n = 0; n < big->tile_count; n++) {
			Image *tile = &big->tiles[n];
			Image part = *tile;
			int dx = (tile->x - i->x) * zoom / 64;
			int dy = (tile->y - i->y) * zoom / 64;
			if (flip)
				dx = -dx - tile->w * zoom / 64;
			if (angle) {
				double a = angle * 3.14159265358979323846 / 180.0;
				int cx = dx + tile->w * zoom / 128, cy = dy + tile->h * zoom / 128;
				int ox = i->w * zoom / 128, oy = i->h * zoom / 128;
				dx = ox + (int)(cx * cos(a) - cy * sin(a)) - tile->w * zoom / 128;
				dy = oy + (int)(cx * sin(a) + cy * cos(a)) - tile->h * zoom / 128;
			}
			/* dx/dy ya colocan el mosaico dentro de la imagen completa; sus offsets de atlas no se deben
			 * volver a sumar al emitir la textura individual. */
			part.x = part.y = 0;
			part.full_w = part.w;
			part.full_h = part.h;
			draw_ex(&part, l, x + dx, y + dy, flip, zoom, r, g, b, angle, alpha);
		}
		return;
	}
	if (big && big->idx) {
		img_draw_big(i, l, x, y);
		return;
	}
	if (i->kind == 2) {
		video_rect(l, x, y, i->full_w * zoom / 64, i->full_h * zoom / 64, i->solid[0], i->solid[1], i->solid[2],
		           0);
		return;
	}
	if (i->kind == 3)
		return;
	if (scale == 2) {
		zoom *= 2;
		fw = (i->full_w + 1) / 2;
	}
	sdl_draw_texture(tex(i), l, x + (flip ? (fw - i->x - i->w) * zoom / 64 : i->x * zoom / 64),
	                 y + i->y * zoom / 64, i->w, i->h, 0, 0, i->w, i->h, r * img_brightness / 256,
	                 g * img_brightness / 256, b * img_brightness / 256, (i->flags & IMG_TRANSLUCENT) != 0, flip,
	                 zoom, angle, i->vram_x, alpha);
}
void img_draw_ex(const Image *i, int l, int x, int y, int flip, int zoom, int r, int g, int b)
{
	draw_ex(i, l, x, y, flip, zoom, r, g, b, 0, 255);
}
void img_draw(const Image *i, int l, int x, int y)
{
	img_draw_ex(i, l, x, y, 0, 64, 128, 128, 128);
}
void img_draw_flip(const Image *i, int l, int x, int y, int f)
{
	img_draw_ex(i, l, x, y, f, 64, 128, 128, 128);
}
void img_draw_ex_rot(const Image *i, int l, int x, int y, int f, int z, int r, int g, int b, int a)
{
	draw_ex(i, l, x, y, f, z, r, g, b, a, 255);
}
void img_draw_ex_alpha_rot(const Image *i, int l, int x, int y, int f, int z, int r, int g, int b, int angle,
                           int alpha)
{
	draw_ex(i, l, x, y, f, z, r, g, b, angle, alpha);
}
int img_load_big(Image *img, const uint8_t *f, int n, int x, int y, int c)
{
	BigRec *rec;
	uint32_t raw, comp;
	int tail = img_tail(f), fmt = img_fmt(f), bpp = fmt == FMT_PAL ? 1 : fmt == FMT_8888 ? 4 : 2;

	(void)x;
	(void)y;
	(void)c;
	if (read_header(img, f, n) != 0)
		return -1;
	if (img->kind > 1 || !img->w)
		return -1;
	raw = rd32(f + tail);
	comp = rd32(f + tail + 4);
	if (raw != (uint32_t)img->w * img->h * bpp || raw > BIG_BYTES || comp > (uint32_t)(n - tail - 8))
		return -1;
	if (img->kind == 1 || IMG_TILE_LARGE_OPAQUE) {
		uint8_t *pixels;
		uint32_t lut[256];
		int tile_fmt = fmt == FMT_PAL ? FMT_8888 : fmt;
		int tile_bpp = fmt == FMT_PAL ? 4 : bpp;
		int cols = (img->w + 511) / 512, rows = (img->h + 511) / 512;
		int count = cols * rows, tx, ty, k = 0;
		pixels = malloc(raw);
		if (!pixels || lz4_decode(f + tail + 8, (int)comp, pixels, (int)raw) != (int)raw) {
			free(pixels);
			return -1;
		}
		rec = big_free_slot();
		if (!rec) {
			free(pixels);
			return -1;
		}
		memset(rec, 0, sizeof(*rec));
		rec->owner = img;
		rec->tiles = calloc((size_t)count, sizeof(*rec->tiles));
		if (!rec->tiles) {
			free(pixels);
			memset(rec, 0, sizeof(*rec));
			return -1;
		}
		rec->tile_count = count;
		if (fmt == FMT_PAL)
			pal_lut(f + 24, img->flags, lut);
		for (ty = 0; ty < img->h; ty += 512)
			for (tx = 0; tx < img->w; tx += 512) {
				Image *tile = &rec->tiles[k++];
				int tw = img->w - tx < 512 ? img->w - tx : 512;
				int th = img->h - ty < 512 ? img->h - ty : 512;
				uint8_t *part = malloc((size_t)tw * th * tile_bpp);
				int row, col;
				if (!part) {
					free(pixels);
					big_drop(rec);
					return -1;
				}
				for (row = 0; row < th; row++) {
					const uint8_t *srcrow = pixels + ((size_t)(ty + row) * img->w + tx) * bpp;
					uint8_t *dstrow = part + (size_t)row * tw * tile_bpp;
					if (fmt == FMT_PAL) {
						for (col = 0; col < tw; col++)
							((uint32_t *)dstrow)[col] = lut[srcrow[col]];
					} else {
						memcpy(dstrow, srcrow, (size_t)tw * bpp);
					}
				}
				*tile = *img;
				tile->x = (uint16_t)(img->x + tx);
				tile->y = (uint16_t)(img->y + ty);
				tile->w = (uint16_t)tw;
				tile->h = (uint16_t)th;
				tile->loaded = tile->big = 0;
				tile->vram_x = 0;
				if (img_upload_true(tile, part, tile_fmt) != 0) {
					free(pixels);
					big_drop(rec);
					return -1;
				}
			}
		free(pixels);
		img->vram_x = (uint16_t)(BIG_SLOT + (rec - bigs));
		img->big = 1;
		img->loaded = 1;
		return 0;
	}
	rec = big_free_slot();
	if (!rec)
		return -1; /* sin hueco: mejor no dibujarla que robar los datos de otra que sigue en pantalla */
	rec->idx = malloc(raw);
	if (!rec->idx)
		return -1;
	if (lz4_decode(f + tail + 8, (int)comp, rec->idx, (int)raw) != (int)raw) {
		free(rec->idx);
		memset(rec, 0, sizeof(*rec));
		return -1;
	}
	rec->owner = img;
	rec->fmt = (uint8_t)fmt;
	if (fmt == FMT_PAL)
		pal_lut(f + 24, img->flags, rec->lut);
	img->vram_x = (uint16_t)(BIG_SLOT + (rec - bigs));
	img->big = 1;
	img->loaded = 1;
#ifdef VN_TRACE
	printf("img: big %ux%u (ventana %dx%d, textura max %d)\n", (unsigned)img->w, (unsigned)img->h, BIG_WIN_W,
	       BIG_WIN_H, max_texture_dim());
#endif
	return 0;
}
void img_draw_big(const Image *img, int l, int x, int y)
{
	uint32_t *dst;
	uint16_t *dst16;
	const uint8_t *src;
	SDL_Rect area;
	BigRec *rec;
	Image win;
	int ox, oy, cw, ch, row, col;

	rec = big_of(img);
	if (!img->loaded || !rec) {
#ifdef VN_TRACE
		/* una vez: sin indices la panoramica no se dibuja y el log se llenaria cada cuadro */
		static int warned;
		if (!warned) {
			warned = 1;
			printf("img: big %ux%u sin indices (frame %d)\n", (unsigned)img->w, (unsigned)img->h,
			       video_frames());
		}
#endif
		return;
	}
	if (rec->tiles) {
		draw_ex(img, l, x, y, 0, 64, 128, 128, 128, 0, 255);
		return;
	}
	if (!win_texture(rec))
		return;
	cw = img->w < BIG_WIN_W ? img->w : BIG_WIN_W;
	ch = img->h < BIG_WIN_H ? img->h : BIG_WIN_H;
	/* esquina de la ventana dentro del bitmap: la parte de la imagen que cae en pantalla */
	ox = -x - img->x;
	oy = -y - img->y + VIEW_Y;
	if (ox > img->w - cw)
		ox = img->w - cw;
	if (oy > img->h - ch)
		oy = img->h - ch;
	if (ox < 0)
		ox = 0;
	if (oy < 0)
		oy = 0;

	if (ox != rec->win_x || oy != rec->win_y || cw != rec->win_w || ch != rec->win_h) {
		int bpp = rec->fmt == FMT_PAL ? 1 : rec->fmt == FMT_8888 ? 4 : 2;
		for (row = 0; row < ch; row++) {
			src = rec->idx + ((size_t)(oy + row) * img->w + ox) * bpp;
			if (rec->window_bpp == 2 && rec->fmt != FMT_PAL && rec->fmt != FMT_8888) {
				dst16 = big_win.native16 + (size_t)row * BIG_WIN_W;
				if (rec->fmt == FMT_565) {
					for (col = 0; col < cw; col++) {
						unsigned c = rd16(src + (size_t)col * 2);
						dst16[col] = (uint16_t)(((c & 31) << 11) | (c & 0x7e0) | (c >> 11));
					}
				} else {
					memcpy(dst16, src, (size_t)cw * sizeof(*dst16));
				}
			} else {
				dst = big_win.rgba32 + (size_t)row * BIG_WIN_W;
				if (rec->fmt == FMT_PAL) {
					for (col = 0; col < cw; col++)
						dst[col] = rec->lut[src[col]];
				} else {
					for (col = 0; col < cw; col++)
						dst[col] = true_pixel(src + (size_t)col * bpp, rec->fmt);
				}
			}
		}
		area = (SDL_Rect){0, 0, cw, ch};
		if (SDL_UpdateTexture(rec->window, &area,
		                      rec->window_bpp == 2 ? (const void *)big_win.native16
		                                           : (const void *)big_win.rgba32,
		                      BIG_WIN_W * rec->window_bpp) != 0)
			return;
		rec->win_x = ox;
		rec->win_y = oy;
		rec->win_w = cw;
		rec->win_h = ch;
	}
	win = *img;
	win.big = 0;
	win.x = 0;
	win.y = 0;
	win.w = win.full_w = (uint16_t)cw;
	win.h = win.full_h = (uint16_t)ch;
	img_draw(&win, l, x + img->x + ox, y + img->y + oy);
}
