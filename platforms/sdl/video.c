#include "video.h"

#include "sdl_target.h"
#include <stdlib.h>
#include <string.h>
#if defined(__PSP__)
#include <psppower.h> /* scePowerTick / PSP_POWER_TICK_ALL, definidos por el SDK de pspdev */
#include <pspdisplay.h> /* sceDisplayGetFrameBuf y PspDisplayPixelFormats, para el snapshot de las transiciones */
#if defined(VN_TRACE)
#include <pspsysmem.h>
#include <vramalloc.h>
#endif
/* El SDK no declara esta llamada (existe en el firm y la exporta PPSSPP): es el bloqueo que evita que la consola
 * se suspenda mientras el juego esta corriendo. scePowerTick solo rearma el reloj de reposo, no impide el SUSPEND,
 * y con el atlas del perfil (mas grande) ruta-4 volvia a morir de SELF-POWERDOWN a media ruta. */
int sceKernelPowerLock(unsigned int type, void *argp);
#define PSP_POWER_LOCK_DEFAULT 0u
#endif
#if defined(__vita__)
#include <psp2/display.h> /* sceDisplayGetFrameBuf y SceDisplayFrameBuf, para lo mismo */
#endif

#define COMMANDS 8192
typedef struct {
	int type, layer, x, y, w, h, r, g, b, blend, alpha;
	SDL_Texture *texture;
	int slot; /* puesto de img.c de esa textura; 0 = fuera de su tabla (la fuente) */
	SDL_Rect src;
	double angle;
	int flip;
} Draw;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Surface *headless_surface;
static Draw draws[COMMANDS];
static int draw_count, frames, vblanks, clip;
#if defined(VN_TRACE) || defined(VN_PSP_PROGRESS)
static unsigned long flip_entrada;   /* para el emparejamiento entrada/salida del flip (vease video_flip) */
#endif
static uint32_t started;
static SDL_Texture *snapshot, *font;
static uint8_t *last_pixels;
/* Snapshot por re-emision (T-633). Cuando la consola no puede leer su propia pantalla -es el caso de PSP, donde
 * SDL_RenderReadPixels responde "not supported", y de Vita, donde sale negro-, la "copia del cuadro anterior" se
 * reconstruye volviendo a emitir la lista de draws de ese cuadro. Cuesta RAM estatica, no VRAM: 1024 Draw. Con las
 * escenas de Snoot (fondo + hasta 16 sprites + caja + el texto por glifo) se queda muy por debajo del tope; si alguna
 * vez se recorta, snap_dropped lo cuenta y se ve con VN_TRACE. */
#define SNAP_MAX 1024
static Draw snap_draws[SNAP_MAX];
static int snap_count, snap_frozen, snap_replay, snap_dropped;
extern int texture_slot_live(int slot, SDL_Texture *t);   /* img.c: la textura de un draw guardado sigue viva? */
extern void res_snapshot_pin(int texture_slot, int pinned);
static int shot_requested, shot_number;
static int output_w = SCREEN_W, output_h = SCREEN_H;
void sdl_snapshot_to(const char *path);

/* PS1 usa 128 como tinte neutro (1.0); SDL usa 255. La GPU de PS1 hace texel * tinte/128,
 * SDL hace texel * mod/255. Para aclarar por encima de 1.0 (tinte >128) SDL no puede
 * pasar de 255 con ColorMod, asi que se recorta. En Snoot Wani ningun SHOW/anim usa
 * valores >128 (grep de las claves ANIM en los VNB de build/ctl/snoot-ps1-data da max 128),
 * por eso recortar es correcto; si apareciera un brillo >1.0 habria que hacer una
 * segunda pasada aditiva con SDL_BLENDMODE_ADD. */
static int ps1_to_sdl(int v)
{
	if (v <= 0)
		return 0;
	if (v >= 255)
		return 255;
	{
		int s = v * 255 / 128;
		return s > 255 ? 255 : s;
	}
}

/* Mapeo de coordenadas del motor (320x240) a la ventana de la consola. */
static void map_pos(int *mx, int *my, int x, int y)
{
#if SDL_TARGET_MANUAL_SCALE
	*mx = SDL_TARGET_VIEW_X + x * SDL_TARGET_VIEW_W / SCREEN_W;
	*my = SDL_TARGET_VIEW_Y + y * SDL_TARGET_VIEW_H / SCREEN_H;
#else
	*mx = x;
	*my = y;
#endif
}

static void map_rect(SDL_Rect *r, int x, int y, int w, int h)
{
	int mx, my, ex, ey;
	map_pos(&mx, &my, x, y);
	map_pos(&ex, &ey, x + w, y + h);
	r->x = mx;
	r->y = my;
	r->w = ex - mx;
	r->h = ey - my;
}

static void add(Draw d)
{
	if (draw_count < COMMANDS)
		draws[draw_count++] = d;
}
/* El recorte se aplica solo cuando cambia: SDL_RenderSetClipRect vuelca la cola del renderer, y en el backend PSP
 * de SDL2 cada vuelco espera al GE (17 llamadas por cuadro costaban ~6 s en PPSSPP, ver T-629). */
static SDL_Rect clip_applied;
static int clip_applied_valid;
static void apply_clip(const SDL_Rect *c)
{
	if (c) {
		if (clip_applied_valid && clip_applied.x == c->x && clip_applied.y == c->y && clip_applied.w == c->w &&
		    clip_applied.h == c->h)
			return;
		clip_applied = *c;
		clip_applied_valid = 1;
	} else {
		if (!clip_applied_valid)
			return;
		clip_applied_valid = 0;
	}
	SDL_RenderSetClipRect(renderer, c);
}

/* Lee lo que hay en pantalla desde el framebuffer de la consola. Hace falta porque SDL_RenderReadPixels NO esta
 * soportado en el driver PSP (medido: "That operation is not supported") y en Vita responde negro, y este buffer no
 * es solo para las capturas del Test Lab: es el que rellena video_snapshot(), la copia de la escena anterior con la
 * que se cruzan los 4465 disolvimientos y fundidos del juego. Sin ella un TR_DISSOLVE no cruza nada (se ve corte
 * seco) y un TR_FADE empieza ya en negro.

 * VA APAGADA POR DETRAS (-DVN_PSP_SNAPSHOT_FROM_VRAM) y esto es lo medido el 2026-10-31 en PPSSPP 1.20.4:
 * la llamada responde bien - la direccion alterna 0x4000000/0x4044000 con cada flip, bufferwidth=512 (el pitch en
 * pixeles, no el ancho de 480) y pixelformat=0 (565) - y trajo pixeles reales, pero CONGELADOS: las cinco capturas
 * pedidas en los cuadros 616, 686, 692, 700 y 760 son byte a byte la misma imagen, la pantalla de arranque. Con un
 * backend de GPU (OpenGL/Vulkan/D3D) PPSSPP compone el framebuffer en una textura y no lo mantiene en la RAM
 * emulada, asi que ahi no hay nada que leer. En una PSP real el GE escribe en VRAM y esta es la via normal de las
 * capturas de homebrew, pero no se puede verificar en emulador, y dejarla activa sin verificar seria peor de lo que
 * hay hoy: un snapshot todo ceros con alfa 255 se pinta OPACO, o sea los 30 cuadros del disolvimento se verian negros
 * en vez de ver un corte seco. Queda para prueba en hardware. */
static int console_framebuffer(void)
{
#if defined(__PSP__) && defined(VN_PSP_SNAPSHOT_FROM_VRAM)
	void *fb = 0;
	int bw = 0, pf = -1, x, y, bpp;
	const uint8_t *bytes;

	if (sceDisplayGetFrameBuf(&fb, &bw, &pf, 0) < 0 || !fb)
		return 0;
#ifdef VN_TRACE
	/* Diagnostico de T-633: PPSSPP devolvia la pantalla de arranque congelada. Se imprime la direccion que
	 * responde cada variante del ultimo argumento (0 = NEXTHSYNC/INMEDIATO, 1 = NEXTVSYNC) y cuanto brillo tiene lo
	 * que hay ahi, para saber si el emulador modela el framebuffer displayed o solo el buffer 0. */
	{
		void *fb1 = 0;
		int bw1 = 0, pf1 = -1, i;
		unsigned long lum = 0;
		const uint8_t *b0 = (const uint8_t *)fb;
		for (i = 0; i < 4096; i++)
			lum += b0[i];
		sceDisplayGetFrameBuf(&fb1, &bw1, &pf1, 1);
		printf("video: getframebuf(0)=%p bw=%d pf=%d suma4KB=%lu | getframebuf(1)=%p bw=%d pf=%d cuadro=%d\n",
		       fb, bw, pf, lum, fb1, bw1, pf1, frames);
		fflush(stdout);
	}
#endif
	bpp = pf == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
	/* bufferwidth viene en PIXELES y es potencia de dos (512 para una vista de 480): es el pitch, no el ancho */
	if (bw < SCREEN_W)
		return 0;
	bytes = (const uint8_t *)fb;
	for (y = 0; y < SCREEN_H; y++)
		for (x = 0; x < SCREEN_W; x++) {
			uint8_t *dst = last_pixels + ((size_t)y * SCREEN_W + x) * 4;
			const uint8_t *src = bytes + ((size_t)y * bw + x) * bpp;
			unsigned v = bpp == 2 ? (unsigned)src[0] | ((unsigned)src[1] << 8)
			                       : (unsigned)src[0] | ((unsigned)src[1] << 8)
			                         | ((unsigned)src[2] << 16) | ((unsigned)src[3] << 24);
			switch (pf) {
			case PSP_DISPLAY_PIXEL_FORMAT_565:
				dst[0] = (uint8_t)((v & 31) * 8 + 4);
				dst[1] = (uint8_t)(((v >> 5) & 63) * 4 + 2);
				dst[2] = (uint8_t)(((v >> 11) & 31) * 8 + 4);
				break;
			case PSP_DISPLAY_PIXEL_FORMAT_5551:
				dst[0] = (uint8_t)((v & 31) * 8 + 4);
				dst[1] = (uint8_t)(((v >> 5) & 31) * 8 + 4);
				dst[2] = (uint8_t)(((v >> 10) & 31) * 8 + 4);
				dst[3] = (v >> 15) ? 255 : 0;
				continue;
			case PSP_DISPLAY_PIXEL_FORMAT_4444:
				dst[0] = (uint8_t)((v & 15) * 17);
				dst[1] = (uint8_t)(((v >> 4) & 15) * 17);
				dst[2] = (uint8_t)(((v >> 8) & 15) * 17);
				dst[3] = (uint8_t)(((v >> 12) & 15) * 17);
				continue;
			default: /* 8888: u32 0xAABBGGRR; en memoria little-endian R,G,B,A */
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
				dst[3] = src[3];
				continue;
			}
			dst[3] = 255;
		}
	return 1;
#elif defined(__vita__) && defined(VN_PSP_SNAPSHOT_FROM_VRAM)
	SceDisplayFrameBuf f;
	int x, y;

	memset(&f, 0, sizeof f);
	f.size = sizeof f;
	if (sceDisplayGetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !f.base)
		return 0;
	if (f.pitch < (unsigned)SCREEN_W || f.width < (unsigned)SCREEN_W)
		return 0;
	for (y = 0; y < SCREEN_H; y++) {
		const uint8_t *row = (const uint8_t *)f.base + (size_t)y * f.pitch * 4;
		uint8_t *dst = last_pixels + (size_t)y * SCREEN_W * 4;
		if (f.pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) {
			/* A8B8G8R8 en little-endian se guarda R,G,B,A: es el mismo orden de bytes que SDL_PIXELFORMAT_RGBA32 */
			memcpy(dst, row, (size_t)SCREEN_W * 4);
			continue;
		}
		for (x = 0; x < SCREEN_W; x++) {
			unsigned v = (unsigned)row[x * 4] | ((unsigned)row[x * 4 + 1] << 8)
			           | ((unsigned)row[x * 4 + 2] << 16) | ((unsigned)row[x * 4 + 3] << 24);
			/* A2B10G10R10: R en los bits 0-9, G 10-19, B 20-29, A 30-31 */
			dst[x * 4] = (uint8_t)((v & 0x3ff) >> 2);
			dst[x * 4 + 1] = (uint8_t)(((v >> 10) & 0x3ff) >> 2);
			dst[x * 4 + 2] = (uint8_t)(((v >> 20) & 0x3ff) >> 2);
			dst[x * 4 + 3] = (uint8_t)(((v >> 30) & 3) * 85);
		}
	}
	return 1;
#else
	return 0;
#endif
}

/* 1 si last_pixels tiene lo que habia en pantalla; 0 si ni la consola ni SDL pudieron leerla.
 * `for_shot` distingue quien pregunta: la palanca VN_SNAPSHOT_REPLAY solo afecta al snapshot de las transiciones,
 * porque si no, la captura con la que se mira el resultado estaria leydose del mismo sitio roto. */
static int capture_pixels(int for_shot)
{
	uint8_t *raw;
	int x, y;
	static int avisado;
	if (!last_pixels)
		return 0;
	if (!for_shot && getenv("VN_SNAPSHOT_REPLAY"))
		return 0; /* fuerza el camino de re-emision en PC, donde leer la pantalla si funciona */
	if (for_shot && console_framebuffer())
		return 1;
#if defined(__vita__)
	/* En Vita SDL_RenderReadPixels NO falla: responde bien y con negro (medido otra vez el 2026-11-01 sobre la
	 * captura del Test Lab: 960x544, brillo medio 0,00 y un solo color). Un snapshot de negro cruzaria contra negro,
	 * o sea el mismo defecto de siempre con aspecto de haber funcionado. Por eso para el snapshot no se le pregunta
	 * aqui y se va por re-emision; para la captura del paso `shot` si se le pregunta, porque lo que se quiere es la
	 * pantalla tal cual y su negro ya esta reportado como pendiente en T-633. */
	if (!for_shot)
		return 0;
#endif
	if (!renderer)
		return 0;
	SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
	raw = malloc((size_t)output_w * output_h * 4);
	if (!raw)
		return 0;
	if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32, raw, output_w * 4) == 0) {
		for (y = 0; y < SCREEN_H; y++)
			for (x = 0; x < SCREEN_W; x++) {
				int sx = x * output_w / SCREEN_W, sy = y * output_h / SCREEN_H;
				uint8_t *pixel = last_pixels + ((size_t)y * SCREEN_W + x) * 4;
				memcpy(pixel, raw + ((size_t)sy * output_w + sx) * 4, 4);
				pixel[3] = 255;
			}
		free(raw);
		return 1;
	}
	free(raw);
	if (!avisado) {
		/* Antes esto se tragaba en silencio y el resultado era "el disolvimento no se ve", sin una linea en ningun
		 * log. Un fallo de lectura de pantalla es un defecto del port, no del escenario. */
		avisado = 1;
		printf("video: no se pudo leer la pantalla (%s); las transiciones con copia se veran cortadas\n",
		       SDL_GetError());
		fflush(stdout);
	}
	return 0;
}
static void rect(int layer, int x, int y, int w, int h, int r, int g, int b, int blend)
{
	Draw d = {0};
	d.type = 1;
	d.layer = layer;
	d.x = x;
	d.y = y;
	d.w = w;
	d.h = h;
	d.r = r;
	d.g = g;
	d.b = b;
	d.blend = blend;
	d.alpha = 255;
	add(d);
}
void video_init(void)
{
	SDL_TARGET_PREPARE_IOP();
	SDL_TARGET_CALL_SET_MAIN_READY();
#if defined(__PSP__)
	sceKernelPowerLock(PSP_POWER_LOCK_DEFAULT, NULL); /* una sola vez: la consola no se suspende con el juego abierto */
	scePowerSetClockFrequency(333, 333, 166);
#endif
	if (SDL_InitSubSystem(SDL_TARGET_INIT_FLAGS) != 0)
		return;
	if (getenv("SDL_VIDEODRIVER") && strcmp(getenv("SDL_VIDEODRIVER"), "dummy") == 0) {
		headless_surface = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_RGBA32);
		if (headless_surface)
			renderer = SDL_CreateSoftwareRenderer(headless_surface);
	} else {
		window = SDL_CreateWindow("Snoot Game", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		                          SDL_TARGET_WINDOW_W, SDL_TARGET_WINDOW_H, SDL_WINDOW_SHOWN);
		if (window)
			renderer = SDL_CreateRenderer(window, -1, SDL_TARGET_RENDERER_FLAGS);
		if (!renderer && window)
			renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	}
	if (renderer) {
#if SDL_TARGET_USE_LOGICAL_SIZE
		SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);
#endif
		SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
#if defined(__PSP__) && defined(VN_TRACE)
		printf("mem: event=renderer vram_free=%lu vram_largest=%lu sys_free=%lu sys_largest=%lu\n",
		       (unsigned long)vmemavail(), (unsigned long)vlargestblock(), (unsigned long)sceKernelTotalFreeMemSize(),
		       (unsigned long)sceKernelMaxFreeMemSize());
#endif
#ifdef VN_TRACE
		{
			/* el limite de textura de la consola: de el depende que una imagen vaya por img_load_big.
			 * Se imprime tambien el rectangulo de presentacion: es lo que aplasta la imagen si el perfil
			 * no lo sobrescribio (SDL_TARGET_VIEW_* lleva guarda #ifndef por eso). */
			SDL_RendererInfo info;
			if (SDL_GetRendererInfo(renderer, &info) == 0)
				printf("video: renderer=%s maxtex=%dx%d output=%dx%d vista=%d,%dx%dx%d escala=%d\n", info.name,
				       info.max_texture_width, info.max_texture_height, output_w, output_h,
				       SDL_TARGET_VIEW_X, SDL_TARGET_VIEW_Y, SDL_TARGET_VIEW_W, SDL_TARGET_VIEW_H,
				       SDL_TARGET_MANUAL_SCALE);
		}
#endif
	}
	last_pixels = malloc(SCREEN_W * SCREEN_H * 4);
	started = SDL_GetTicks();
}
void video_rect(int l, int x, int y, int w, int h, int r, int g, int b, int blend)
{
	rect(l, x, y, w, h, r, g, b, blend);
}
void video_triangle_down(int l, int x, int y, int w, int h, int r, int g, int b)
{
	Draw d = {0};
	d.type = 2;
	d.layer = l;
	d.x = x;
	d.y = y;
	d.w = w;
	d.h = h;
	d.r = r;
	d.g = g;
	d.b = b;
	d.alpha = 255;
	add(d);
}
void video_snapshot(int x, int y)
{
	(void)x;
	(void)y;
	if (!renderer || !last_pixels)
		return;
	if (!capture_pixels(0)) {
		/* La consola no pudo leer su pantalla. Si tenemos la lista de draws del ultimo cuadro completo, la
		 * re-emitimos: es el unico camino que queda en PSP y Vita, y freeze la copia para que los cuadros de la
		 * propia transicion no se vayan cruzando contra si mismos. */
		if (snap_count) {
			snap_replay = 1;
			snap_frozen = 1;
			/* scene_sync carga la escena nueva antes de pintar el snapshot. Fija las texturas viejas para que
			 * la cache no las expulse y el fundido no deje huecos ni restos. */
			{
				int i;
				for (i = 0; i < snap_count; i++)
					if (snap_draws[i].type == 4 && snap_draws[i].texture &&
					    texture_slot_live(snap_draws[i].slot, snap_draws[i].texture))
						res_snapshot_pin(snap_draws[i].slot, 1);
			}
#ifdef VN_TRACE
			/* Sonda de T-633 (temporal, quitar cuando el caso este cerrado): histograma de lo guardado, para saber si
			 * la caja de la escena anterior esta o NO en la lista. Solo las cinco primeras veces: un snapshot por
			 * transicion son ~2600 lineas por corrida y eso inunda el log del Test Lab. */
			{
				static int sondas;
				if (sondas < 5) {
					int i, nrect = 0, ntex = 0, nother = 0, nbox = 0, ntext = 0;
					sondas++;
					for (i = 0; i < snap_count; i++) {
						if (snap_draws[i].type == 1)
							nrect++;
						else if (snap_draws[i].type == 4)
							ntex++;
						else
							nother++;
						if (snap_draws[i].layer == LAYER_BOX)
							nbox++;
						if (snap_draws[i].layer == LAYER_TEXT)
							ntext++;
					}
					printf("video: snapshot por re-emision de %d dibujos (recortados %d): rect=%d tex=%d otro=%d"
					       " caja=%d texto=%d cuadro=%d\n",
					       snap_count, snap_dropped, nrect, ntex, nother, nbox, ntext, frames);
					fflush(stdout);
				}
			}
#endif
		}
		return;
	}
	snap_replay = 0;
	if (!snapshot)
		snapshot =
		    SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, SCREEN_W, SCREEN_H);
	SDL_UpdateTexture(snapshot, NULL, last_pixels, SCREEN_W * 4);
}
void video_release_snapshot(void)
{
	int i;
	if (snap_replay)
		for (i = 0; i < snap_count; i++)
			if (snap_draws[i].type == 4)
				res_snapshot_pin(snap_draws[i].slot, 0);
	snap_replay = 0;
	snap_frozen = 0;
}
void video_snapshot_draw(int l, int x, int y, int blend, int bright, int band_y, int band_bright)
{
	(void)x;
	(void)y;
	(void)band_y;
	(void)band_bright;
	if (snap_replay && snap_count) {
		int i;
		/* Orden: el bucle de video_flip recorre la lista de atras hacia adelante, o sea lo ultimo agregado se dibuja
		 * primero (queda detras). Recorrer la copia hacia adelante reproduce el orden de pintado original. */
		for (i = 0; i < snap_count && draw_count < COMMANDS - 1; i++) {
			Draw d = snap_draws[i];
			if (d.type == 3)
				continue; /* la copia de si misma, si viniera del cuadro guardado */
			if (d.texture && !texture_slot_live(d.slot, d.texture))
				continue; /* la imagen ya salio de la caché: mejor un dibujo de menos que un punter colgando */
			d.layer = l;  /* todo el cuadro anterior se repone por la capa de la transicion, encima de la escena nueva */
			if (blend == 2) {
				/* Dissolve: la escena nueva ya se dibuja con brillo p. La anterior debe
				 * conservar su color y perder opacidad (1-p), no oscurecerse encima. */
				d.alpha = d.alpha * bright / 128;
				d.blend = 1;
			} else {
				d.r = d.r * bright / 128;
				d.g = d.g * bright / 128;
				d.b = d.b * bright / 128;
			}
			if (!blend)
				d.blend = 0; /* el FADE quiere la copia opaca: se anula el alfa propio de cada dibujo */
			add(d);
		}
		return;
	}
	if (snapshot) {
		Draw d = {0};
		d.type = 3;
		d.layer = l;
		d.x = 0;
		d.y = VIEW_Y;
		d.w = SCREEN_W;
		d.h = VIEW_H;
		d.r = blend == 2 ? 128 : bright;
		d.g = blend == 2 ? 128 : bright;
		d.b = blend == 2 ? 128 : bright;
		d.blend = blend;
		d.alpha = blend == 2 ? bright * 2 : 255;
		d.texture = snapshot;
		d.src = (SDL_Rect){0, VIEW_Y, SCREEN_W, VIEW_H};
		add(d);
	}
}
void video_clip_scene(void)
{
	clip = 1;
}
void video_unclip(void)
{
	clip = 0;
}
void video_flip(void)
{
	int layer, i;
	if (getenv("VN_FAST") && getenv("SDL_VIDEODRIVER") && strcmp(getenv("SDL_VIDEODRIVER"), "dummy") == 0 &&
	    (!getenv("VN_SHOT_DIR") || !shot_requested)) {
		draw_count = 0;
		frames++;
		vblanks++;
		return;
	}
	if (renderer) {
		SDL_Rect view_clip;
		int clipped = 0;
#if defined(VN_TRACE) || defined(VN_PSP_PROGRESS)
		/* Maridaje entrada/salida del vuelco: si el log termina con una ENTRADA sin su SALIDA, el cuadro se queda
		 * dentro del dibujo o del SDL_RenderPresent (el GE), no en el bucle del juego. Es la pieza que faltaba para
		 * saber donde esta el atasco de T-644, que desde fuera se ve igual que un cuelgue del VM. */
		if (!(++flip_entrada % (
#ifdef VN_TRACE
					300
#else
					30
#endif
					))) {
			printf("hb2: flip_entrada it=%lu cuadros=%d\n", flip_entrada, frames);
			fflush(stdout); /* el par que importa es el ultimo antes de la parada: sin vaciar se pierde con el proceso */
		}
#endif
		SDL_SetRenderTarget(renderer, NULL);
		/* SDL_SetRenderTarget deja el recorte del renderer en "sin recorte": el cache hay que invalidarlo igual. */
		clip_applied_valid = 0;
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		if (clip) {
			map_rect(&view_clip, 0, VIEW_Y, SCREEN_W, VIEW_H);
			clipped = 1;
		}
		for (layer = LAYER_BG; layer >= LAYER_DEBUG; layer--) {
			apply_clip(clipped && layer >= LAYER_SPRITE0 ? &view_clip : NULL);
			for (i = draw_count - 1; i >= 0; i--) {
				Draw *d = &draws[i];
				SDL_Rect dst;
				if (d->layer != layer)
					continue;
				map_rect(&dst, d->x, d->y, d->w, d->h);
				if (d->type == 1) {
					SDL_SetRenderDrawBlendMode(renderer,
					                           d->blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
					SDL_SetRenderDrawColor(renderer, d->r, d->g, d->b,
					                       (d->blend ? 128 : 255) * d->alpha / 255);
					SDL_RenderFillRect(renderer, &dst);
				} else if (d->type == 2) {
#if SDL_TARGET_HAS_RENDER_GEOMETRY
					int ax, ay, bx, by, cx, cy;
					SDL_Vertex v[3];
					map_pos(&ax, &ay, d->x, d->y);
					map_pos(&bx, &by, d->x + d->w, d->y);
					map_pos(&cx, &cy, d->x + d->w / 2, d->y + d->h);
					v[0] = (SDL_Vertex){{(float)ax, (float)ay},
					                    {(Uint8)d->r, (Uint8)d->g, (Uint8)d->b, (Uint8)d->alpha},
					                    {0, 0}};
					v[1] = (SDL_Vertex){{(float)bx, (float)by},
					                    {(Uint8)d->r, (Uint8)d->g, (Uint8)d->b, (Uint8)d->alpha},
					                    {0, 0}};
					v[2] = (SDL_Vertex){{(float)cx, (float)cy},
					                    {(Uint8)d->r, (Uint8)d->g, (Uint8)d->b, (Uint8)d->alpha},
					                    {0, 0}};
					SDL_RenderGeometry(renderer, NULL, v, 3, NULL, 0);
#else
					SDL_Rect tr = {d->x, d->y, d->w > 0 ? d->w : 1, d->h > 0 ? d->h : 1};
					SDL_SetRenderDrawColor(renderer, d->r, d->g, d->b, (Uint8)d->alpha);
					SDL_RenderFillRect(renderer, &tr);
#endif
				} else if (d->type == 3) {
					int sr = ps1_to_sdl(d->r);
					int sg = ps1_to_sdl(d->g);
					int sb = ps1_to_sdl(d->b);
					SDL_SetTextureColorMod(d->texture, sr, sg, sb);
					SDL_SetTextureAlphaMod(d->texture, (Uint8)d->alpha);
					SDL_SetTextureBlendMode(d->texture,
					                        d->blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
					SDL_RenderCopy(renderer, d->texture, &d->src, &dst);
				} else if (d->texture) {
					int sr, sg, sb;
					if (d->texture == font) {
						sr = d->r > 255 ? 255 : d->r;
						sg = d->g > 255 ? 255 : d->g;
						sb = d->b > 255 ? 255 : d->b;
					} else {
						sr = ps1_to_sdl(d->r);
						sg = ps1_to_sdl(d->g);
						sb = ps1_to_sdl(d->b);
					}
					SDL_SetTextureColorMod(d->texture, sr, sg, sb);
					SDL_SetTextureAlphaMod(d->texture, (Uint8)d->alpha);
					SDL_SetTextureBlendMode(d->texture,
					                        d->blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_BLEND);
					SDL_RenderCopyEx(renderer, d->texture, &d->src, &dst, d->angle, NULL,
					                 d->flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
				}
			}
		}
		if (!snap_frozen && draw_count) {
			/* El cuadro que acabamos de volcar es el "anterior" para la proxima transicion. Mientras hay una en
			 * curso NO se re-copia: si no, la transicion se cruzaria contra si misma. */
			int n = draw_count > SNAP_MAX ? SNAP_MAX : draw_count;
			if (draw_count > SNAP_MAX)
				snap_dropped++;
			memcpy(snap_draws, draws, sizeof(Draw) * n);
			snap_count = n;
		}
		apply_clip(NULL);
		if (shot_requested) {
			SDL_RenderFlush(renderer);
			capture_pixels(1);
		}
		SDL_RenderPresent(renderer);
#if defined(VN_TRACE) || defined(VN_PSP_PROGRESS)
		if (flip_entrada && !(flip_entrada % 300)) {
			printf("hb2: flip_salida it=%lu cuadros=%d\n", flip_entrada, frames);
			fflush(stdout);
		}
#endif
	}
	if (shot_requested) {
		const char *dir = getenv("VN_SHOT_DIR"), *names = getenv("VN_SHOT_NAMES");
#if defined(__PSP__)
		if (!dir || !*dir) {
			/* En PSP no se puede fijar la variable antes de resolver la unidad (los datos estan en ms0: o, en la
			 * Go, en ef0:), asi que el destino de las capturas es la raiz que ya resolvio cd_init(), igual que
			 * ux0: en Vita y sdmc: en 3DS. */
			extern const char *cd_root(void);
			dir = cd_root();
		}
#endif
		if (dir && *dir) {
			char p[800], name[128] = "shot";
			int i = 0, j = 0, n = shot_number++;
			if (names) {
				const char *s = names;
				while (n && *s) {
					if (*s == ',')
						n--;
					s++;
				}
				if (*s) {
					while (*s && *s != ',' && j + 1 < (int)sizeof(name))
						name[j++] = *s++;
					name[j] = 0;
				}
			}
			for (i = 0; dir[i] && i < 400; i++)
				p[i] = dir[i];
			p[i++] = '/';
			snprintf(p + i, sizeof(p) - (size_t)i, "%s.bmp", name);
			sdl_snapshot_to(p);
#ifdef VN_TRACE
			/* Permite que el harness solicite el buffer GPU de PPSSPP para la misma toma; el BMP PSP puede estar
			 * leyendo una copia del framebuffer que el renderer acelerado del emulador no mantiene actualizada. */
			printf("shot: name=%s frame=%d\n", name, frames);
			fflush(stdout);
#endif
		}
		shot_requested = 0;
	}
	draw_count = 0;
	/* La consola tiene reloj de reposo: si no entra boton, una PSP real y PPSSPP apagan el equipo. En los creditos
	 * el autotest pasa minutos sin pulsar nada y la corrida se cortaba ahi ("SELF-POWERDOWN!" en el log de PPSSPP,
	 * ruta-4 parada en el cuadro 12238 con 26 minutos corridos). scePowerTick cada cuadro es lo que hace cualquier
	 * juego comercial: sin esto, cualquier ruta larga es una ruleta segun cuanto tarde el episodio. */
#if defined(__PSP__)
	scePowerTick(PSP_POWER_TICK_ALL);
	/* SDL present-vsync wedges in PPSSPP on long routes; pace the completed swap to GE VBlank. */
	sceDisplayWaitVblankStart();
#endif
	frames++;
	vblanks++;
}
int video_frames(void)
{
	return frames;
}
void video_wait_vblank(void)
{
	if (getenv("VN_FAST"))
		return;
	uint32_t due = started + (uint32_t)((frames + 1) * 1000 / 60), now = SDL_GetTicks();
	if (due > now)
		SDL_Delay(due - now);
	vblanks++;
}
int video_vblank_count(void)
{
	return vblanks;
}

void sdl_draw_texture(SDL_Texture *t, int layer, int x, int y, int w, int h, int sx, int sy, int sw, int sh, int r,
                      int g, int b, int blend, int flip, int zoom, int angle, int slot, int alpha)
{
	Draw d = {0};
	d.type = 4;
	d.layer = layer;
	d.x = x;
	d.y = y;
	d.w = w * zoom / 64;
	d.h = h * zoom / 64;
	d.texture = t;
	d.slot = slot;
	d.src = (SDL_Rect){sx, sy, sw, sh};
	/* r/g/b vienen en escala PS1 (128 = neutro); font ya viene en escala SDL (r*2) y se guarda tal cual */
	if (t == font) {
		d.r = r > 255 ? 255 : r < 0 ? 0 : r;
		d.g = g > 255 ? 255 : g < 0 ? 0 : g;
		d.b = b > 255 ? 255 : b < 0 ? 0 : b;
	} else {
		d.r = r < 0 ? 0 : r > 255 ? 255 : r;
		d.g = g < 0 ? 0 : g > 255 ? 255 : g;
		d.b = b < 0 ? 0 : b > 255 ? 255 : b;
	}
	d.blend = blend;
	d.alpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
	d.flip = flip;
	d.angle = (double)angle * 360.0 / 4096.0;
	add(d);
}
SDL_Renderer *sdl_renderer(void)
{
	return renderer;
}
SDL_Texture *sdl_font_texture(void)
{
	return font;
}
void sdl_set_font(SDL_Texture *t)
{
	if (font && font != t)
		SDL_DestroyTexture(font); /* una recarga del atlas (otro perfil, otra corrida) no debe dejar la anterior */
	font = t;
}
static void put16(FILE *f, unsigned v)
{
	fputc((int)(v & 255), f);
	fputc((int)((v >> 8) & 255), f);
}
static void put32(FILE *f, unsigned v)
{
	put16(f, v & 65535);
	put16(f, v >> 16);
}
void sdl_snapshot_to(const char *path)
{
	FILE *f;
	int y, x;
	if (!last_pixels || !path || !*path)
		return;
	f = fopen(path, "wb");
	if (!f)
		return;
	fputc('B', f);
	fputc('M', f);
	put32(f, 54 + SCREEN_W * SCREEN_H * 3);
	put32(f, 0);
	put32(f, 54);
	put32(f, 40);
	put32(f, SCREEN_W);
	put32(f, SCREEN_H);
	put16(f, 1);
	put16(f, 24);
	put32(f, 0);
	put32(f, SCREEN_W * SCREEN_H * 3);
	put32(f, 2835);
	put32(f, 2835);
	put32(f, 0);
	put32(f, 0);
	for (y = SCREEN_H - 1; y >= 0; y--)
		for (x = 0; x < SCREEN_W; x++) {
			const uint8_t *p = last_pixels + ((size_t)y * SCREEN_W + x) * 4;
			fputc(p[2], f);
			fputc(p[1], f);
			fputc(p[0], f);
		}
	fclose(f);
}
void sdl_shot_request(void)
{
	shot_requested = 1;
}
