#include "res.h"
#include "cd.h"
#include "sdl_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__PSP__) && defined(VN_TRACE)
#include <malloc.h>
#include <pspsysmem.h>
#include <vramalloc.h>
#endif

extern void img_release(Image *img);

/* Techo de la lectura en heap para un perfil: Vita (960x544 en RGBA8888) son 2 MB crudos y el container llega
 * comprimido; 8 MB deja margen sin aceptar basura. Por encima de esto la imagen se da por inexistente. */
#ifndef RES_HEAP_MAX
#define RES_HEAP_MAX (8 * 1024 * 1024)
#endif

/* Cache de imagenes acotado (SDL_TARGET_RES_CACHE) con desalojo por escena, como el res.c de platforms/ps1: lo que
 * se toco en la escena actual (res_touch) no se desaloja; si hace falta sitio para dibujar tambien sirve lo que no
 * se dibujo en este cuadro ni en el anterior (res_tick cuenta los cuadros). Los ids que no existen quedan marcados
 * en `missing` (1 bit por id de 16 bits) para no reabrir el archivo cada vez que se piden. */
#define RES_IDS 0x10000
#if defined(VN_PSP_NATIVE_MENU) && !defined(SDL_TARGET_RES_BYTES)
#define SDL_TARGET_RES_BYTES (3932160u) /* 3,75 MiB: deja espacio para cargar las capas finales y sus temporales */
#endif

typedef struct {
	int id; /* 0 = libre; si no, id de la imagen + 1 */
	Image image;
	uint32_t last_use;  /* ultima escena en que se pidio (res_get o res_touch) */
	uint32_t last_draw; /* ultimo cuadro de res_get; 0 si solo se precargo */
	uint8_t snapshot_pinned;
	uint32_t bytes;     /* cota de almacenamiento de textura o bitmap grande; no solo píxeles visibles */
} Entry;

static Entry cache[SDL_TARGET_RES_CACHE];
static uint8_t missing[RES_IDS / 8];
static uint32_t scene_now = 1, draw_clock = 2;
#ifdef SDL_TARGET_RES_BYTES
static uint32_t resident_bytes;
extern uint32_t img_memory_bound(const uint8_t *file, int size);
#endif
volatile uint32_t res_trace_tag;

static Entry *find(int id)
{
	int i;

	for (i = 0; i < SDL_TARGET_RES_CACHE; i++)
		if (cache[i].id == id + 1)
			return &cache[i];
	return 0;
}

static int missing_get(int id)
{
	return id >= 0 && id < RES_IDS && (missing[id >> 3] >> (id & 7)) & 1;
}

static void missing_set(int id)
{
	if (id >= 0 && id < RES_IDS)
		missing[id >> 3] |= (uint8_t)(1 << (id & 7));
}

/* Desaloja la entrada menos usada que no pertenece a la escena actual. Con for_draw tambien puede desalojar una de
 * la escena actual que no se dibujo en este cuadro ni en el anterior (cuadros de animacion ya pasados o precargados).
 * Devuelve 0 si no hay ninguna. */
static int evict_one(int for_draw)
{
	int i, best = -1;

	for (i = 0; i < SDL_TARGET_RES_CACHE; i++)
		if (cache[i].id && !cache[i].snapshot_pinned && cache[i].last_use < scene_now &&
		    (best < 0 || cache[i].last_use < cache[best].last_use))
			best = i;
	if (best < 0 && for_draw)
		for (i = 0; i < SDL_TARGET_RES_CACHE; i++)
			if (cache[i].id && !cache[i].snapshot_pinned && cache[i].last_draw + 1 < draw_clock &&
			    (best < 0 || cache[i].last_draw < cache[best].last_draw))
				best = i;
	if (best < 0)
		return 0;
#if defined(VN_TRACE) || defined(VN_PSP_TEXTURE_DIAG)
	printf("res: evict id=%d scene=%lu slot=%u big=%u loaded=%u\n", cache[best].id - 1,
	       (unsigned long)scene_now, (unsigned)cache[best].image.vram_x, (unsigned)cache[best].image.big,
	       (unsigned)cache[best].image.loaded);
#endif
	img_release(&cache[best].image);
#ifdef SDL_TARGET_RES_BYTES
	resident_bytes -= cache[best].bytes;
#endif
	cache[best].id = 0;
	return 1;
}

static Entry *new_entry(int for_draw)
{
	int i;

	for (;;) {
		for (i = 0; i < SDL_TARGET_RES_CACHE; i++)
			if (!cache[i].id)
				return &cache[i];
		if (!evict_one(for_draw))
			return 0;
	}
}

static const Image *load_image(int id, int for_draw)
{
	/* Estatico: 180 KB en el stack desbordan el del hilo principal de consolas como Wii (128 KB)
	 * y corrompen la memoria que sigue (crash en el primer SCENE). Cubre el disco de PS1 (su imagen mas grande
	 * mide 167 KB); un perfil con mas pixeles tiene imagenes mas grandes y se lee en el heap con el tamano real. */
	static uint8_t file[180000];
	uint8_t *buf = file;
	int cap = (int)sizeof(file);
	char path[32];
	Entry *e;
	int n, size;
#ifdef SDL_TARGET_RES_BYTES
	uint32_t required;
#endif

	e = find(id);
	if (e) {
		e->last_use = scene_now;
		if (for_draw)
			e->last_draw = draw_clock;
		return &e->image;
	}
	if (missing_get(id))
		return 0;
	snprintf(path, sizeof(path), "\\DATA\\I%04d.IMG", id);
	/* "No existe" y "no entra en mi buffer" no son lo mismo: marcar como ausente lo que solo era grande dejaba la
	 * imagen negra para siempre (paso con las 11 imagenes del perfil psp que pesaban mas de 180 KB). */
	if (cd_find(path, 0, &size) < 0) {
#ifdef VN_TRACE
		if (for_draw)
			printf("res: DRAW_MISS id=%d cause=file\n", id);
#endif
		missing_set(id);
		return 0;
	}
	if (size > cap) {
		if (size > RES_HEAP_MAX) {
			missing_set(id);
			return 0;
		}
		buf = malloc((size_t)size);
		if (!buf)
			return 0;
		cap = size;
	}
	n = cd_read_file(path, buf, cap);
	if (n < 0) {
		if (buf != file)
			free(buf);
		return 0;
	}
#ifdef SDL_TARGET_RES_BYTES
	required = img_memory_bound(buf, n);
	if (required > SDL_TARGET_RES_BYTES) {
#ifdef VN_TRACE
		if (for_draw)
			printf("res: DRAW_MISS id=%d cause=image-budget\n", id);
#endif
		if (buf != file)
			free(buf);
		return 0;
	}
	while (resident_bytes > SDL_TARGET_RES_BYTES - required) {
		if (!evict_one(for_draw)) {
#ifdef VN_TRACE
			if (for_draw)
				printf("res: DRAW_MISS id=%d cause=bytes\n", id);
#endif
			if (buf != file)
				free(buf);
			return 0;
		}
	}
#endif
	e = new_entry(for_draw);
	if (!e) {
#ifdef VN_TRACE
		if (for_draw)
			printf("res: DRAW_MISS id=%d cause=slots\n", id);
#endif
		if (buf != file)
			free(buf);
		return 0;
	}
	memset(e, 0, sizeof(*e));
	e->id = id + 1;
	e->last_use = scene_now;
	e->last_draw = for_draw ? draw_clock : 0;
	if (img_load(&e->image, buf, n, 0, 0, 0)) {
#ifdef VN_TRACE
		if (for_draw)
			printf("res: DRAW_MISS id=%d cause=decode\n", id);
#endif
		e->id = 0;
		if (buf != file)
			free(buf);
		return 0;
	}
#ifdef SDL_TARGET_RES_BYTES
	e->bytes = required;
	resident_bytes += required;
#if defined(__PSP__) && defined(VN_TRACE)
	{
		static size_t peak;
		static int invalid_reported;
		/* PPSSPP can return mallinfo values larger than the arena; reject those instead of claiming a false peak. */
		struct mallinfo memory = mallinfo();
		size_t vram_free = vmemavail(), vram_largest = vlargestblock();
		size_t sys_free = sceKernelTotalFreeMemSize(), sys_largest = sceKernelMaxFreeMemSize();
		if (memory.uordblks > memory.arena) {
			if (!invalid_reported) {
				invalid_reported = 1;
				printf("heap: mallinfo_invalid allocated=%lu arena=%lu\n",
				       (unsigned long)memory.uordblks, (unsigned long)memory.arena);
			}
		} else if (memory.uordblks > peak) {
			peak = memory.uordblks;
			printf("heap: peak_allocated=%lu arena=%lu free_chunks=%lu\n", (unsigned long)peak,
			       (unsigned long)memory.arena, (unsigned long)memory.fordblks);
		}
		printf("mem: event=image scene=%lu id=%d vram_free=%lu vram_largest=%lu sys_free=%lu sys_largest=%lu\n",
		       (unsigned long)scene_now, id, (unsigned long)vram_free, (unsigned long)vram_largest,
		       (unsigned long)sys_free, (unsigned long)sys_largest);
	}
#endif
#ifdef VN_TRACE
	printf("res: image=%d bound=%lu resident=%lu budget=%lu\n", id, (unsigned long)required,
	       (unsigned long)resident_bytes, (unsigned long)SDL_TARGET_RES_BYTES);
#endif
#endif
	if (buf != file)
		free(buf);
	return &e->image;
}

void res_new_scene(void)
{
	scene_now++;
#if defined(__PSP__) && defined(VN_TRACE)
	printf("mem: event=scene scene=%lu vram_free=%lu vram_largest=%lu sys_free=%lu sys_largest=%lu\n",
	       (unsigned long)scene_now, (unsigned long)vmemavail(), (unsigned long)vlargestblock(),
	       (unsigned long)sceKernelTotalFreeMemSize(), (unsigned long)sceKernelMaxFreeMemSize());
#endif
}

void res_tick(void)
{
	draw_clock++;
}

const Image *res_get(int id)
{
	return load_image(id, 1);
}

void res_touch(int id)
{
	Entry *e = find(id);

	if (e)
		e->last_use = scene_now;
}

void res_preload(int id)
{
	load_image(id, 0);
}

const Image *res_peek(int id)
{
	Entry *e = find(id);

	return e ? &e->image : 0;
}

int res_reserve_snapshot(int *x, int *y)
{
	if (x)
		*x = 0;
	if (y)
		*y = 0;
	return 1;
}

void res_release_snapshot(void)
{
}

void res_snapshot_pin(int texture_slot, int pinned)
{
	int i;
	if (texture_slot <= 0)
		return;
	for (i = 0; i < SDL_TARGET_RES_CACHE; i++)
		if (cache[i].id && cache[i].image.loaded && cache[i].image.vram_x == texture_slot)
			cache[i].snapshot_pinned = (uint8_t)(pinned != 0);
}

void res_prefetch(const uint16_t *ids, int n)
{
	int i;
	/* RES_SOUND: look_ahead mezcla sonidos ambientales con las imagenes (en PS1 van a la cache de RAM); aqui el
	 * audio los lee solo al sonar. Tratarlos como imagen buscaba I32770.IMG y demas en cada vistazo. */
	for (i = 0; i < n; i++)
		if (!(ids[i] & RES_SOUND))
			res_preload(ids[i]);
}

int res_prefetch_busy(void)
{
	return 0;
}

int res_have(int id)
{
	/* un sonido no espera a esta cache: con 0 el motor volvia a pedir toda la precarga en cada linea */
	return (id & RES_SOUND) || res_peek(id) != NULL;
}

const uint8_t *res_cached_file(int id, int *n)
{
	(void)id;
	if (n)
		*n = 0;
	return NULL;
}

uint8_t *res_borrow_cache(int *n)
{
	if (n)
		*n = 0;
	return NULL;
}
