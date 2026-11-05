/*
 * Snoot Game PS1 - motor del juego: VM, escenas, dialogo, decisiones, modo saltar, titulo, pausa, guardado, opciones,
 * galeria y capitulos extra.
 *
 * Controles: X avanza y confirma, O vuelve, R1 salta lineas ya leidas, Start abre la pausa, d-pad elige en menus,
 * Cuadrado oculta la caja, Select muestra la depuracion. En el titulo, L1 + R1 + Select abre el menu de capitulos de
 * desarrollo (ahi tambien se salta lo no leido).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "vn_platform.h"
#include "cd.h"
#include "chapter.h"
#include "app.h"
#include "game.h"
#include "game_config.h"
#if defined(VN_PSP_NATIVE_MENU) || defined(VN_VITA_NATIVE_MENU)
#include "psp_menu.h"
#endif
#ifdef VN_NATIVE_AUDIO
#include "audio_native.h"
#endif
#include "ui_text.h"
#include "img.h"
#include "input.h"
#include "opcodes.h"
#include "res.h"
#include "save.h"
#include "scene.h"
#include "special.h"
#include "text.h"
#include "video.h"
#include "vm.h"

/* BOX_Y viene de engine/platform/video.h con el perfil de la consola (D29). */
#define MAX_SPEAKERS   48
#define MAX_POSITIONS  48
#define SEEN_BYTES     VN_SEEN_BYTES
#define MENU_TOP       (SCREEN_H * 14 / 240) /* 14 en PS1 */
#define MENU_BOTTOM    (SCREEN_H - SCREEN_H * 14 / 240)
#define MAX_GALLERY    256
#define SKIP_PER_FRAME 8 /* lineas ya leidas que pasan por cuadro al saltar */
#define MAX_DEV        48
#define MAX_CHAPTERS   48
#define MAX_EXTRAS     10
#define MAX_ENDINGS    8
#define SLOTS          3
#ifdef VN_VITA_NATIVE_MENU
#define VITA_SLOTS_X     (SCREEN_W * 24 / 320)
#define VITA_SLOTS_TOP  (SCREEN_H * 62 / 240)
#define VITA_SLOTS_STEP (SCREEN_H * 45 / 240)
#define VITA_SLOTS_H    (SCREEN_H * 43 / 240)
#endif

typedef struct {
	char name[64];
	uint8_t r, g, b;
} Speaker;

typedef enum {
	ST_RUN,
	ST_SAY,
	ST_MENU,
	ST_PAUSE,
	ST_END,
	ST_ERROR,
	ST_DEV,
	ST_TRANS,
	ST_TITLE,
	ST_PMENU,
	ST_SLOTS,
	ST_OPTIONS,
	ST_GALLERY,
	ST_EXTRAS,
	ST_CONFIRM,
	ST_NOTICE,
	ST_SPECIAL /* pantalla especial numero n de game_specials: ST_SPECIAL + n (siempre al final) */
} State;

#define IS_SPECIAL(s)     ((s) >= ST_SPECIAL)
#define SPECIAL_HAS(flag) (IS_SPECIAL(state) && (special->flags & (flag)))

static uint8_t file_buf[16 * 1024] __attribute__((aligned(4)));
static Speaker speakers[MAX_SPEAKERS];
static int speaker_count;
static int16_t pos_x[MAX_POSITIONS], pos_y[MAX_POSITIONS];
static int pos_count;
static int start_chapter;
static uint32_t start_offset;

static Vm vm;
static State state;
static TextRun run;
static int scene_dirty, show_box = 1, debug;
/* Caja de texto como config.window = "auto" de Ren'Py: aparece con cada linea, se queda (vacia) en las pausas y se
 * oculta antes de `scene` y de las decisiones; `window show/hide` la fijan. Entra y sale con un fundido de 0.2 s. */
#define BOX_FADE        12
#define TEXTBOX_INSTANT 4
static int win_auto = 1, win_on, box_level;
static int snap_box; /* la copia de la transicion lleva la caja completa */
static int pause_left, say_seen_before;
static int bg_start; /* cuadro en que aparecio el fondo (inicio de su Pan) */
static uint8_t seen[SEEN_BYTES];
static uint32_t endings_mask, bonus_mask;
static uint16_t gallery_image[MAX_GALLERY]; /* imagen -> indice de galeria, en pares */
static uint8_t gallery_index[MAX_GALLERY];
static int gallery_pairs, gallery_count;
static uint8_t gallery_seen[(MAX_GALLERY + 7) / 8];

/* Videos de la galeria (VID1 de GAME.BIN, solo los juegos que los tienen): entrada de galeria, archivo V<nn>.STR y la
 * imagen que lo desbloquea (0xFFFF = nunca). Se reproducen con special_fmv; X en la galeria. */
#define MAX_VIDEOS    16
#define SPECIAL_VIDEO 250 /* id de la pantalla de video en games/<juego>/specials.c */
static int video_count;
static uint8_t video_slot[MAX_VIDEOS], video_file[MAX_VIDEOS];
static uint16_t video_unlock[MAX_VIDEOS];
static State special_return = ST_RUN; /* adonde vuelve el juego al terminar una pantalla especial */

/* Menu de capitulos (desarrollo): entradas de STORY y la variable de ruta que cada una necesita (255 = ninguna) */
static int dev_count, dev_chapter;
static uint32_t dev_offset[MAX_DEV];
static uint8_t dev_var[MAX_DEV], dev_value[MAX_DEV];
static char dev_name[MAX_DEV][32];
#ifdef VN_VITA_NATIVE_MENU
static State dev_back = ST_TITLE;
static int pref_skip_unseen, pref_skip_after_choices = 1, pref_transitions = 2;
static int pref_chapter_select = 1;
#endif

/* Modo saltar (R1): pasa lo ya leido sin cargar nada del CD; se detiene en texto nuevo, finales y cambios de capitulo */
static int skip_mode, eat_cross;
static int dev_session; /* partida empezada desde el menu de desarrollo: salta lo no leido */

/* Datos de la interfaz (seccion UI01 de GAME.BIN) */
static int logo_image, lock_image;
static uint16_t ending_image[MAX_ENDINGS]; /* imagen que marca cada final al verse (0 = ninguna) */
static int ending_count;                   /* UI01 trae 4 */
static char chapter_name[MAX_CHAPTERS][42];
static int extra_count;
static uint16_t extra_chapter[MAX_EXTRAS];
static uint32_t extra_offset[MAX_EXTRAS];
static char extra_name[MAX_EXTRAS][42];
static const Special *special; /* la pantalla especial abierta */

/* Partida y progreso */
#ifdef VN_VITA_NATIVE_MENU
static int music_level = 6, sfx_level = 8;
static int text_speed_cps = TEXT_DEFAULT_CPS, auto_forward_seconds = 15;
#else
static int music_level = 10, sfx_level = 10;
static int text_speed_cps = TEXT_DEFAULT_CPS, auto_forward_seconds;
#endif
static uint32_t say_auto_elapsed_frames;
static uint32_t play_frames; /* tiempo de la partida actual */
static int playing_extra;    /* la partida es un capitulo extra: al terminar vuelve al titulo */
static int autosave_pending;

static int menu_cursor, menu_top, menu_order[VM_MENU_MAX];
static uint32_t rng_state = 12345;

/* Bitacora de etapas del bucle principal, solo para diagnosticos con --trace (T-644): sin traza el macro no cuesta
 * nada. Se imprime CADA etapa (no la mascara al final del cuerpo): lo que interesa es la ultima linea del log, y con
 * la mascara impresa arriba de la iteracion se perderia justo la iteracion colgada.
 *   cuerpo:  1=input 2=audio 4=autoplay 8=maquina de estados 16=hud/dibujo 32=flip
 *   dentro de run_vm: 64=vm_run 128=scene_sync 256=transicion preparada 512=prefetch/lookahead        */
#ifdef VN_TRACE
static int vn_etapa(int n)
{
	if (video_frames() >= 11500) {
		printf("etapa: cuadro=%d n=%d\n", video_frames(), n);
		fflush(stdout);
	}
	return 0;
}
#define VN_ETAPA(n) vn_etapa(n)
#else
#define VN_ETAPA(n) ((void)0)
#endif

static void fatal(const char *msg)
{
	int i;

	printf("ERROR: %s\n", msg);
	fflush(stdout); /* se queda en video_wait_vblank para siempre: sin esto el mensaje muere en el buffer y un
	                 * disco mal armado (falta un capitulo) parece un cuelgue del motor sin causa */
	for (i = 0; i < 2; i++) { /* sin fuente: un bloque rojo indica el error */
		video_rect(LAYER_TEXT, 140, 100, 40, 40, 220, 0, 0, 0);
		video_flip();
	}
	for (;;)
		video_wait_vblank();
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int random_below(int n)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (int)((rng_state >> 16) % (uint32_t)n);
}

/* ---------------------------------------------------------------- progreso */
static int seen_get(int gid)
{
	return gid < SEEN_BYTES * 8 && (seen[gid >> 3] & (1 << (gid & 7)));
}

static void seen_set(int gid)
{
	if (gid < SEEN_BYTES * 8)
		seen[gid >> 3] |= (uint8_t)(1 << (gid & 7));
}

/* ---------------------------------------------------------------- datos */
static void load_game_tables(void)
{
	const uint8_t *p = file_buf;
	int size = cd_read_file("\\DATA\\GAME.BIN", file_buf, sizeof(file_buf)), i, n;

	if (size < 12 || memcmp(p, "GAM1", 4) != 0)
		fatal("GAME.BIN");
	start_chapter = rd16(p + 4);
	start_offset = rd32(p + 6);
	speaker_count = rd16(p + 10);
	p += 12;
	if (speaker_count > MAX_SPEAKERS)
		fatal("demasiados hablantes");
	for (i = 0; i < speaker_count; i++) {
		int n;

		if (p + 4 > file_buf + size)
			fatal("tabla de hablantes truncada");
		n = p[3];
		if (n >= (int)sizeof(speakers[i].name) || p + 4 + n > file_buf + size)
			fatal("nombre de hablante invalido");

		speakers[i].r = p[0];
		speakers[i].g = p[1];
		speakers[i].b = p[2];
		memcpy(speakers[i].name, p + 4, n);
		speakers[i].name[n] = 0;
		p += 4 + n;
	}
	pos_count = rd16(p);
	p += 2;
	if (pos_count > MAX_POSITIONS)
		fatal("demasiadas posiciones");
	for (i = 0; i < pos_count; i++) {
		pos_x[i] = (int16_t)rd16(p);
		pos_y[i] = (int16_t)rd16(p + 2);
		p += 4;
	}
	gallery_count = rd16(p);
	gallery_pairs = rd16(p + 2);
	p += 4;
	if (gallery_count > MAX_GALLERY || gallery_pairs > MAX_GALLERY)
		fatal("galeria demasiado grande");
	for (i = 0; i < gallery_pairs; i++) {
		gallery_image[i] = rd16(p);
		gallery_index[i] = p[2];
		p += 3;
	}
	if (p + 4 > file_buf + size)
		return;
	dev_chapter = rd16(p);
	n = rd16(p + 2);
	p += 4;
	for (i = 0; i < n; i++) {
		if (i < MAX_DEV) {
			int len = p[6] < 31 ? p[6] : 31;

			dev_offset[i] = rd32(p);
			dev_var[i] = p[4];
			dev_value[i] = p[5];
			memcpy(dev_name[i], p + 7, len);
			dev_name[i][len] = 0;
			dev_count = i + 1;
		}
		p += 7 + p[6];
	}
	/* VID1: videos de la galeria (Snoot no la tiene: sigue UI01) */
	if (p + 5 <= file_buf + size && memcmp(p, "VID1", 4) == 0) {
		n = p[4];
		p += 5;
		for (i = 0; i < n; i++, p += 4) {
			if (i < MAX_VIDEOS) {
				video_slot[i] = p[0];
				video_file[i] = p[1];
				video_unlock[i] = rd16(p + 2);
				video_count = i + 1;
			}
		}
	}
	/* UI01: logo, candado, imagenes de los finales, nombres de capitulo y capitulos extra */
	if (p + 4 > file_buf + size || memcmp(p, "UI01", 4) != 0)
		return;
	logo_image = rd16(p + 4);
	lock_image = rd16(p + 6);
	ending_count = 4;
	for (i = 0; i < ending_count; i++)
		ending_image[i] = rd16(p + 8 + 2 * i);
	n = rd16(p + 16);
	p += 18;
	for (i = 0; i < n; i++) {
		if (i < MAX_CHAPTERS) {
			memcpy(chapter_name[i], p + 1, p[0] < 41 ? p[0] : 41);
			chapter_name[i][p[0] < 41 ? p[0] : 41] = 0;
		}
		p += 1 + p[0];
	}
	n = *p++;
	for (i = 0; i < n; i++) {
		if (i < MAX_EXTRAS) {
			extra_chapter[i] = rd16(p);
			extra_offset[i] = rd32(p + 2);
			memcpy(extra_name[i], p + 7, p[6] < 41 ? p[6] : 41);
			extra_name[i][p[6] < 41 ? p[6] : 41] = 0;
			extra_count = i + 1;
		}
		p += 7 + p[6];
	}
	strips_setup(rd16(p), p[2]);
	n = rd16(p + 3);
	p += 5;
	credits_clear();
	for (i = 0; i < n; i++) {
		credits_add(p[0], (const char *)p + 2, p[1]);
		p += 2 + p[1];
	}
}

static void gallery_mark(int image)
{
	int i;

	for (i = 0; i < gallery_pairs; i++)
		if (gallery_image[i] == image)
			gallery_seen[gallery_index[i] >> 3] |= (uint8_t)(1 << (gallery_index[i] & 7));
	for (i = 0; i < video_count; i++)
		if (video_unlock[i] == image)
			gallery_seen[video_slot[i] >> 3] |= (uint8_t)(1 << (video_slot[i] & 7));
	/* un final cuenta al ver su imagen (Snoot: las cartas e1of4 ... e4of4, como ending_image() del original) */
	for (i = 0; i < ending_count; i++)
		if (ending_image[i] && ending_image[i] == image && !(endings_mask & (1u << i))) {
			endings_mask |= 1u << i;
			autosave_pending = 1;
		}
}

static int chapter_loader(void *user, int chapter, VmChapter *out)
{
	(void)user;
	return chapter_open(chapter, out);
}

static const char *text_string(int local)
{
	return chapter_text(local);
}

/* ---------------------------------------------------------------- camara (CAMERA) */
/* Zoom y desplazamiento de toda la escena: pantalla = zoom x punto - (tx, ty), con (tx, ty) en pixeles de
 * 1920x1080 (la vista de 320x180 es 1/6 de eso). cam_z va en 1/256; cam_x y cam_y en pixeles de la vista. El
 * cambio se anima de los valores de inicio a los de destino en cam_len cuadros. */
static int ease256(int curve, int u);
static int cam_z = 256, cam_x, cam_y;
static int cam_z0, cam_x0, cam_y0, cam_z1 = 256, cam_x1, cam_y1, cam_t0, cam_len, cam_curve;

static int camera_on(void)
{
	return cam_z != 256 || cam_x || cam_y;
}

static void camera_update(void)
{
	int e, t = (int)(video_frames() - cam_t0);

	if (cam_len <= 0 || t >= cam_len) {
		cam_z = cam_z1;
		cam_x = cam_x1;
		cam_y = cam_y1;
		return;
	}
	e = ease256(cam_curve, t * 256 / cam_len);
	cam_z = cam_z0 + (cam_z1 - cam_z0) * e / 256;
	cam_x = cam_x0 + (cam_x1 - cam_x0) * e / 256;
	cam_y = cam_y0 + (cam_y1 - cam_y0) * e / 256;
}

static int cam_sx(int x)
{
	return x * cam_z / 256 - cam_x;
}

static int cam_sy(int y)
{
	return VIEW_Y + (y - VIEW_Y) * cam_z / 256 - cam_y;
}

/* La camara no va en el guardado: al cargar o volver al titulo arranca sin zoom ni desplazamiento */
static void camera_reset(void)
{
	cam_z = cam_z1 = 256;
	cam_x = cam_x1 = cam_y = cam_y1 = cam_len = 0;
}

static void notify(void *user, int opcode, const int32_t *args, int nargs)
{
	(void)user;
	(void)nargs;
	switch (opcode) {
	case OP_PHONE: {
		int i;

		phone_clear();
		for (i = 0; i < vm.phone_n; i++)
			phone_add(vm.phone_side[i], text_string(vm.phone_text[i]), vm.phone_img[i]);
		break;
	}
	case OP_CAMERA:
		camera_update();
		cam_z0 = cam_z;
		cam_x0 = cam_x;
		cam_y0 = cam_y;
		cam_z1 = args[0] ? args[0] : 256;
		cam_x1 = SRC_PX(args[1]); /* el guion da tx/ty en pixeles de 1920x1080: SRC_PX los pasa a la vista */
		cam_y1 = SRC_PX(args[2]);
		cam_len = skip_mode ? 0 : args[3];
		cam_curve = args[4];
		cam_t0 = (int)video_frames();
		break;
	case OP_SCENE:
		bg_start = video_frames();
		gallery_mark(args[0]);
		scene_dirty = 1;
		if (win_auto)
			win_on = 0;
		break;
	case OP_MENU:
		if (win_auto)
			win_on = 0;
		break;
	case OP_SAY:
	case OP_SAYX:
		if (win_auto)
			win_on = 1;
		break;
	case OP_TEXTBOX:
		if ((args[0] & 3) == 1) {
			win_auto = 1;
		} else {
			win_auto = 0;
			win_on = (args[0] & 3) == 2;
		}
		if (args[0] & TEXTBOX_INSTANT)
			box_level = win_on ? BOX_FADE : 0;
		break;
	case OP_SHOW:
		gallery_mark(args[1]);
		scene_dirty = 1;
		break;
	case OP_SHOW_RANDOM:
		scene_dirty = 1;
		break;
	case OP_BACKGROUND:
		gallery_mark(args[0]);
		scene_dirty = 1;
		break;
	case OP_HIDE:
	case OP_ORDER:
		scene_dirty = 1;
		break;
	case OP_MARK_SEEN:
		gallery_mark(args[0]);
		break;
	case OP_ENDING:
		endings_mask |= 1u << ((args[0] - 1) & 31);
		skip_mode = 0;
		break;
	case OP_LOAD_CHAPTER:
		skip_mode = 0;
		break;
	case OP_UNLOCK_BONUS:
		bonus_mask |= 1u << (args[0] & 31);
		break;
	case OP_MUSIC_PLAY:
		audio_music_play(args[0], args[1], args[2]);
		break;
	case OP_MUSIC_QUEUE:
		audio_music_queue(args[0]);
		break;
	case OP_MUSIC_STOP:
		audio_music_stop(args[0]);
		break;
	case OP_AMBIENT_PLAY:
		audio_ambient_play(args[0], args[1], args[2]);
		break;
	case OP_AMBIENT_STOP:
		audio_ambient_stop(args[0], args[1]);
		break;
	case OP_SFX_PLAY:
		if (!skip_mode) /* al saltar no suenan los efectos (como en Ren'Py) */
			audio_sfx_play(args[0]);
		break;
	case OP_SFX_STOP:
		audio_sfx_stop_fade(args[0]);
		break;
	case OP_VOLUME:
		audio_volume(args[0], args[1], args[2]);
		break;
	}
}

static int op_size(const OpInfo *info)
{
	const char *f;
	int n = 0;

	for (f = info->fmt; *f; f++)
		n += (*f == 'H' || *f == 'h') ? 2 : (*f == 'I') ? 4 : 1;
	return n;
}

/* Congelones: cuadros que tardaron mas de 0.1 s (lecturas del CD, descompresion). La prueba en emulador los lee:
 * cuantos hubo, el mayor y los 32 peores como (vblanks << 24) | (capitulo << 18) | pc */
#define STALL_VBLANKS 6
#define STALL_LOG     32
volatile uint32_t stall_count, stall_max, stall_total, stall_log[STALL_LOG];
volatile uint32_t decode_vblanks; /* preparar escenas sin contar el CD: descomprimir y subir a VRAM */
extern volatile uint32_t cd_sync_vblanks, cd_wait_vblanks, res_trace_tag;

static void log_stall(int vblanks)
{
	uint32_t entry;
	int i, low = 0;

	if (vblanks > 255)
		vblanks = 255;
	entry = ((uint32_t)vblanks << 24) | ((uint32_t)(vm.chapter & 63) << 18) | (vm.pc & 0x3FFFF);
	for (i = 1; i < STALL_LOG; i++)
		if (stall_log[i] < stall_log[low])
			low = i;
	if (entry > stall_log[low])
		stall_log[low] = entry;
	stall_count++;
	stall_total += vblanks;
	if ((uint32_t)vblanks > stall_max)
		stall_max = vblanks;
}

#define LOOKAHEAD_IMAGES 32

static int near_sfx_missing; /* look_ahead: un efecto de antes de la proxima linea no esta en la SPU */
static int say_prefetched;   /* ya se reviso lo que viene despues de la linea actual */

static void add_image(uint16_t *ids, int *n, int id)
{
	int i;

	if (!id || *n >= LOOKAHEAD_IMAGES)
		return;
	for (i = 0; i < *n; i++)
		if (ids[i] == id)
			return;
	ids[(*n)++] = (uint16_t)id;
}

/* Recorre el guion hacia adelante (sin entrar en decisiones) juntando las imagenes y ambientes que se van a usar; con
 * `queue_sfx` pone en cola los efectos que van a sonar. En `*near` (si no es 0) deja cuantos se usan antes de la
 * proxima linea. `steps` limita cuantas instrucciones mira. */
static int look_ahead(int steps, uint16_t *ids, int *near, int queue_sfx)
{
	uint32_t pc = vm.pc;
	int n = 0, k, e;

	near_sfx_missing = 0;
	if (near)
		*near = -1;
	for (k = 0; k < steps && pc < vm.ch.code_size && n < LOOKAHEAD_IMAGES; k++) {
		const uint8_t *p = vm.ch.code + pc;
		const OpInfo *info = op_info(p[0]);

		if (!info || p[0] == OP_MENU || p[0] == OP_CALL || p[0] == OP_RETURN || p[0] == OP_LOAD_CHAPTER ||
		    p[0] == OP_END || p[0] == OP_HALT)
			break;
		if (p[0] == OP_JUMP) {
			pc = rd32(p + 1); /* salto fijo: se sigue el camino */
			continue;
		}
		if ((p[0] == OP_SAY || p[0] == OP_SAYX) && near && *near < 0)
			*near = n;
		if (p[0] == OP_SFX_PLAY && queue_sfx)
			audio_preload(rd16(p + 1));
		if (p[0] == OP_SFX_PLAY && near && *near < 0 && audio_sfx_missing(rd16(p + 1)))
			near_sfx_missing = 1;
		if (p[0] == OP_AMBIENT_PLAY && audio_ambient_missing(p[1], rd16(p + 2)))
			add_image(ids, &n,
			          RES_SOUND | rd16(p + 2)); /* el bucle va a la cache de RAM con las imagenes */
		if (p[0] == OP_SCENE || p[0] == OP_BACKGROUND)
			add_image(ids, &n, rd16(p + 1));
		if (p[0] == OP_SHOW_RANDOM) /* showCG(): cualquiera de sus imagenes */
			for (e = 0; e < p[4]; e++)
				add_image(ids, &n, rd16(p + 2) + e);
		if (p[0] == OP_SHOW) {
			const uint8_t *anim = scene_anim(&vm, rd16(p + 7)), *ev;

			add_image(ids, &n, rd16(p + 2));
			if (anim && anim[0]) /* cuadros que la animacion muestra despues */
				for (e = 0, ev = anim + 3 + 15 * anim[0]; e < anim[2]; e++, ev += 7)
					if (!(ev[4] & 2))
						add_image(ids, &n, rd16(ev + 2));
		}
		pc += 1 + op_size(info);
	}
	if (near && *near < 0)
		*near = n;
	return n;
}

/* Mientras se lee una linea: si lo que se usa antes de la proxima linea no esta en memoria, se empieza a traer ya en
 * segundo plano (la musica se pausa una linea antes, pero al avanzar el juego no se congela). Si la musica ya esta
 * en pausa, se aprovecha para traer tambien lo que sigue. */
static void prefetch_next(void)
{
	uint16_t ids[LOOKAHEAD_IMAGES];
	int n, near, i;

	if (skip_mode || res_prefetch_busy())
		return;
	n = look_ahead(600, ids, &near, 0);
	for (i = 0; i < near; i++)
		res_touch(ids[i]); /* las que ya estan en VRAM no se desalojan antes de usarse */
	for (i = 0; i < near && res_have(ids[i]); i++)
		;
	if (i < near || near_sfx_missing || audio_cd_paused()) {
		res_prefetch(ids, n);
		look_ahead(600, ids, 0, 1);
	}
}

/* Sonidos dentro de las animaciones (`play sound` en un bloque ATL): suenan al pasar su tiempo */
static void anim_sounds(void)
{
	int i, e;

	for (i = 0; i < VM_MAX_SPRITES; i++) {
		VmSprite *s = &vm.sprites[i];
		const uint8_t *anim, *ev;
		int t;

		if (!s->used || !s->anim)
			continue;
		anim = scene_anim(&vm, s->anim);
		if (!anim || !anim[2])
			continue;
		t = video_frames() - (int)s->shown_at;
		ev = anim + 3 + 15 * anim[0];
		for (e = 0; e < anim[2]; e++, ev += 7) {
			int at = rd16(ev);

			if ((ev[4] & 2) && at > s->sound_t && at <= t && !skip_mode) {
				int variants = rd16(ev + 5);

				audio_sfx_play(rd16(ev + 2) + (variants > 1 ? random_below(variants) : 0));
			}
		}
		s->sound_t = t;
	}
}

/* ---------------------------------------------------------------- escena */
/* Alineacion del fondo en el cuadro actual (8.8 desplazado 8 bits mas): interpolacion lineal entre claves (Pan). */
static void bg_align(int *xa, int *ya)
{
	const uint8_t *anim = scene_anim(&vm, vm.bg_anim & 0x7FFF), *k0, *k1;
	int t = video_frames() - bg_start, n, i, t0, t1, x0, y0;

	*xa = 128 << 8; /* sin animacion: posicion por defecto de Ren'Py */
	*ya = 256 << 8;
	if (!anim || !anim[0])
		return;
	n = anim[0];
	for (i = 0; i < n - 1 && t >= rd16(anim + 3 + 15 * (i + 1)); i++)
		;
	k0 = anim + 3 + 15 * i;
	x0 = (int16_t)rd16(k0 + 2);
	y0 = (int16_t)rd16(k0 + 4);
	*xa = x0 << 8;
	*ya = y0 << 8;
	if (i == n - 1)
		return;
	k1 = k0 + 15;
	t0 = rd16(k0);
	t1 = rd16(k1);
	if (t <= t0 || t1 <= t0)
		return;
	*xa += (((int16_t)rd16(k1 + 2) - x0) * 256 * (t - t0)) / (t1 - t0);
	*ya += (((int16_t)rd16(k1 + 4) - y0) * 256 * (t - t0)) / (t1 - t0);
}

/* Imagen opaca que cubre toda la vista: lo que queda detras no se carga ni se dibuja */
static int covers_view(const Image *img, const VmSprite *s)
{
	return img->kind == 0 && !img->big && !s->anim && img->x == 0 && img->y == 0 && img->w >= SCREEN_W &&
	       img->h >= VIEW_H && img->full_w == img->w && img->full_h == img->h;
}

static void scene_sync(void)
{
	const VmSprite *order[VM_MAX_SPRITES];
	uint16_t extra[SCENE_MAX_EVENTS];
	int rank, i, n, count, first = 0, covered = 0;

	if (!scene_dirty || skip_mode)
		return; /* al saltar no se lee el CD: la escena se carga una vez al detenerse */
	scene_dirty = 0;
	res_new_scene();
	count = scene_sorted(&vm, order);
	/* lo que la escena ya tiene cargado queda protegido antes de cargar lo nuevo (si no, el fondo, que se pide al
	 * final, podia desalojarse para hacer lugar a los personajes) */
	if (vm.bg)
		res_touch(vm.bg);
	for (rank = 0; rank < count; rank++)
		res_touch(order[rank]->image);
	for (rank = count - 1; rank >= 0 && !covered; rank--) {
		const Image *img = res_get(order[rank]->image);

		first = rank;
		covered = img && covers_view(img, order[rank]);
	}
	if (vm.bg && !covered)
		res_get(vm.bg);
	/* las imagenes que las animaciones muestran despues se cargan si sobra VRAM, para no leer el CD a mitad */
	for (rank = count - 1; rank >= first; rank--) {
		n = scene_event_images(&vm, order[rank], extra, SCENE_MAX_EVENTS);
		for (i = 0; i < n; i++)
			res_preload(extra[i]);
	}
}

/* Imagen para dibujar: al saltar solo lo que ya esta en VRAM */
static const Image *scene_image(int id)
{
	return skip_mode ? res_peek(id) : res_get(id);
}

/* ---------------------------------------------------------------- transiciones */
#define TR_CUT        0
#define TR_DISSOLVE   1
#define TR_FADE       2
#define TR_VPUNCH     3
#define TR_HPUNCH     4
#define TR_MOVE       5
#define TR_FADE_WHITE 6

static int trans_type, trans_t, trans_len, trans_a, trans_b, trans_c, snap_x = -1, snap_y;
static int scene_dx, scene_dy; /* desplazamiento de toda la escena (sacudidas) */
static int16_t last_x[VM_MAX_SPRITES], last_y[VM_MAX_SPRITES];
static uint8_t last_tag[VM_MAX_SPRITES], last_valid[VM_MAX_SPRITES];
static int16_t from_x[VM_MAX_SPRITES], from_y[VM_MAX_SPRITES];
static uint8_t from_edge[VM_MAX_SPRITES]; /* 0 desde su posicion anterior; 1-4 entra por el borde */

static int ease256(int curve, int u)
{
	int v = 256 - u;

	switch (curve) {
	case 1:
		return (u * u * u) >> 16;
	case 2:
		return 256 - ((v * v * v) >> 16);
	case 3:
		return u < 128 ? (u * u * u) >> 14 : 256 - ((v * v * v) >> 14);
	}
	return u;
}

static void begin_transition(void)
{
	int i;

	trans_type = vm.ev_args[0];
	trans_a = vm.ev_args[1];
	trans_b = vm.ev_args[2];
	trans_c = vm.ev_args[3];
	trans_t = 0;
	switch (trans_type) {
	case TR_DISSOLVE:
	case TR_VPUNCH:
	case TR_HPUNCH:
	case TR_MOVE:
		trans_len = trans_a;
		break;
	case TR_FADE:
	case TR_FADE_WHITE:
		trans_len = trans_a + trans_b + trans_c;
		break;
	default:
		trans_len = 0;
	}
#ifdef VN_VITA_NATIVE_MENU
	if (pref_transitions == 0)
		trans_len = 0;
	else if (pref_transitions == 1 && trans_len > 1)
		trans_len = (trans_len + 1) / 2;
#endif
	if (skip_mode || trans_len <= 0)
		return;
	if (trans_type == TR_DISSOLVE || trans_type == TR_FADE || trans_type == TR_FADE_WHITE) {
		/* el cuadro anterior se guarda en VRAM antes de cargar lo nuevo */
		if (res_reserve_snapshot(&snap_x, &snap_y))
			video_snapshot(snap_x, snap_y);
		else
			snap_x = -1; /* VRAM llena con lo que esta en pantalla: corte (la duracion se mantiene) */
	}
	if (trans_type == TR_MOVE)
		for (i = 0; i < VM_MAX_SPRITES; i++) {
			const VmSprite *s = &vm.sprites[i];

			from_edge[i] = 0;
			if (!s->used)
				continue;
			if (last_valid[i] && last_tag[i] == s->tag) {
				from_x[i] = last_x[i];
				from_y[i] = last_y[i];
			} else {
				from_edge[i] = (uint8_t)(trans_b & 15);
			}
		}
	state = ST_TRANS;
}

static void end_transition(void)
{
	if (snap_x >= 0) {
		res_release_snapshot();
		video_release_snapshot(); /* con el snapshot por re-emision hay que descongelar la copia del cuadro */
		snap_x = -1;
	}
	scene_dx = scene_dy = 0;
	state = ST_RUN;
}

static void update_transition(void)
{
	if (++trans_t >= trans_len || skip_mode)
		end_transition();
}

/* Sacudidas: desplazamiento que alterna de lado cada 2 cuadros y se apaga hacia el final */
static void transition_offset(void)
{
	int amp;

	scene_dx = scene_dy = 0;
	if (state != ST_TRANS || (trans_type != TR_VPUNCH && trans_type != TR_HPUNCH))
		return;
	/* El golpe se mide en pixeles del lienzo de 1920, igual que la camara: con /6 fijo era 1 px en las tres
	 * consolas (PS1 1, PSP 1, Vita 1); con SRC_PX sale 1 / 2 / 5, que es la misma proporcion de pantalla. */
	amp = SRC_PX(trans_b) * (trans_len - trans_t) / trans_len;
	if ((trans_t >> 1) & 1)
		amp = -amp;
	if (trans_type == TR_VPUNCH)
		scene_dy = amp;
	else
		scene_dx = amp;
}

/* Posicion de un sprite durante MoveTransition (y registro de donde quedo cada uno) */
static void move_position(int slot, int tag, int w, int h, int *x, int *y)
{
	if (state == ST_TRANS && trans_type == TR_MOVE && vm.sprites[slot].used) {
		int e = ease256(trans_c, trans_t * 256 / trans_len), fx = from_x[slot], fy = from_y[slot];

		switch (from_edge[slot]) {
		case 1:
			fx = -w;
			fy = *y;
			break;
		case 2:
			fx = SCREEN_W;
			fy = *y;
			break;
		case 3:
			fx = *x;
			fy = VIEW_Y - h;
			break;
		case 4:
			fx = *x;
			fy = VIEW_Y + VIEW_H;
			break;
		}
		*x = fx + (*x - fx) * e / 256;
		*y = fy + (*y - fy) * e / 256;
	}
	last_x[slot] = (int16_t)*x;
	last_y[slot] = (int16_t)*y;
	last_tag[slot] = (uint8_t)tag;
	last_valid[slot] = 1;
}

/* Brillo de la escena nueva durante la transicion (256 = normal). Dissolve: la escena nueva se dibuja a brillo t y
 * encima se suma la copia de la anterior a brillo 1 - t: nueva * t + vieja * (1 - t), continuo y exacto, porque la
 * GPU multiplica cada textura por su color sin costo. Fundido a negro: la entrada sube el brillo de 0 a 1. */
static int transition_brightness(void)
{
	if (state != ST_TRANS || trans_len <= 0)
		return 256;
	/* En PSP el snapshot se compone encima de la escena nueva con alfa (1-p). La escena nueva debe quedar
	 * a brillo normal; oscurecerla tambien multiplicaba p dos veces y hacia parpadear la pantalla casi a negro. */
	if (trans_type == TR_DISSOLVE && snap_x >= 0)
		return 256;
	if (trans_type == TR_FADE && trans_t < trans_a && snap_x < 0) /* sin copia: la salida oscurece lo nuevo */
		return (trans_a - trans_t) * 256 / trans_a;
	if (trans_type == TR_FADE && trans_t >= trans_a + trans_b)
		return (trans_t - trans_a - trans_b) * 256 / (trans_c ? trans_c : 1);
	return 256;
}

static void draw_transition(void)
{
	int p, v, b, white = trans_type == TR_FADE_WHITE;
	/* si la caja sigue visible, la de la copia (oscurecida al 50%) se deshace al doble de brillo: si no, se
	 * oscureceria dos veces */
	int band = snap_box && box_level >= BOX_FADE ? BOX_Y : SCREEN_H;

	if (state != ST_TRANS)
		return;
	p = trans_t * 256 / trans_len;
	if (trans_type == TR_DISSOLVE && snap_x >= 0) {
		b = (256 - p) * 128 / 256;
		video_snapshot_draw(LAYER_FX, snap_x, snap_y, 2, b, band, 2 * b); /* suma */
	} else if (trans_type == TR_FADE) {
		if (trans_t < trans_a) {
			/* salida: la copia de la escena anterior se oscurece de verdad (multiplicada), no restada */
			b = (trans_a - trans_t) * 128 / (trans_a ? trans_a : 1);
			if (snap_x >= 0)
				video_snapshot_draw(LAYER_FX, snap_x, snap_y, 0, b, band, 2 * b);
		} else if (trans_t < trans_a + trans_b) {
			video_rect(LAYER_FX, 0, VIEW_Y, SCREEN_W, VIEW_H, 0, 0, 0, 0);
		}
		/* entrada: la escena nueva ya se dibujo con transition_brightness() */
	} else if (white) {
		if (trans_t < trans_a) {
			v = trans_t * 255 / (trans_a ? trans_a : 1);
			video_rect(LAYER_FX, 0, VIEW_Y, SCREEN_W, VIEW_H, v, v, v, 2); /* encima de la copia */
			if (snap_x >= 0)
				video_snapshot_draw(LAYER_FX, snap_x, snap_y, 0, 128, band, 255);
		} else if (trans_t < trans_a + trans_b) {
			video_rect(LAYER_FX, 0, VIEW_Y, SCREEN_W, VIEW_H, 255, 255, 255, 0);
		} else {
			v = (trans_len - trans_t) * 255 / (trans_c ? trans_c : 1);
			video_rect(LAYER_FX, 0, VIEW_Y, SCREEN_W, VIEW_H, v, v, v, 2);
		}
	}
}

static void draw_bg(void)
{
	if (!vm.bg)
		video_rect(LAYER_BG, 0, VIEW_Y, SCREEN_W, VIEW_H, 0, 0, 0,
		           0); /* negro dibujado: lleva el bit de mascara */
	if (vm.bg) {
		const Image *bg = scene_image(vm.bg);

		if (bg && bg->kind == 2) {
			video_rect(LAYER_BG, 0, VIEW_Y, SCREEN_W, VIEW_H, bg->solid[0] * img_brightness >> 8,
			           bg->solid[1] * img_brightness >> 8, bg->solid[2] * img_brightness >> 8, 0);
		} else if (bg) {
			int xa, ya, bx, by;

			bg_align(&xa, &ya);
			bx = (((SCREEN_W - bg->full_w) * xa) >> 16);
			by = VIEW_Y + (((VIEW_H - bg->full_h) * ya) >> 16);
			if (camera_on()) {
				bx = cam_sx(bx);
				by = cam_sy(by);
			}
			bx += scene_dx;
			by += scene_dy;
			if (bg->big) /* las imagenes grandes no tienen zoom: con camara solo se desplazan */
				img_draw_big(bg, LAYER_BG, bx, by);
			else if (camera_on())
				img_draw_ex(bg, LAYER_BG, bx, by, vm.bg_anim >> 15, 64 * cam_z / 256, 128, 128, 128);
			else
				img_draw_flip(bg, LAYER_BG, bx, by, vm.bg_anim >> 15);
		}
	}
}

static void draw_solid(int layer, const uint8_t *rgba, int alpha)
{
	SolidBlend sb;
	int i;

	scene_solid_blend(rgba, alpha, &sb);
	if (sb.mode != 3) { /* el brillo de la transicion tambien oscurece los colores (no las restas) */
		sb.r = sb.r * img_brightness >> 8;
		sb.g = sb.g * img_brightness >> 8;
		sb.b = sb.b * img_brightness >> 8;
	}
	for (i = 0; i < sb.passes; i++)
		video_rect(layer, 0, VIEW_Y, SCREEN_W, VIEW_H, sb.r, sb.g, sb.b, sb.mode);
}

static void draw_scene(void)
{
	const VmSprite *order[VM_MAX_SPRITES];
	int n = scene_sorted(&vm, order), rank;

	camera_update();
	/* de adelante hacia atras: al llegar a una imagen que tapa toda la vista no hace falta seguir */
	for (rank = n - 1; rank >= 0; rank--) {
		const VmSprite *s = order[rank];
		const Image *img;
		int layer = LAYER_BG - 1 - rank;
		SpriteLook lk;

		scene_look(&vm, s, skip_mode ? SCENE_SETTLED : (int)(video_frames() - s->shown_at), &lk);
		img = scene_image(lk.image);
		if (!img && lk.image != s->image)
			img = scene_image(
			    s->image); /* cuadro de animacion que no cupo en VRAM: se queda la imagen base */
		if (!img || img->kind == 3 || lk.alpha < SCENE_HIDDEN_ALPHA)
			continue;
		if (layer < LAYER_SPRITE0)
			layer = LAYER_SPRITE0;
		if (img->kind == 2) {
			draw_solid(layer, img->solid, lk.alpha);
		} else {
			int w = img->full_w * lk.zoom / 64, h = img->full_h * lk.zoom / 64;
			int x = ((SCREEN_W - w) * lk.xa) / 256, y = VIEW_Y + ((VIEW_H - h) * lk.ya) / 256;

			move_position((int)(s - vm.sprites), s->tag, w, h, &x, &y);
			if (vm.persp_on) {
				/* perspectiva de Ren'Py (reference.py:project): escala = z11 / (z11 - (z - zc)); con zzoom el tamano se compensa
				 * con (z11 - z) / z11 y la esquina se corre a la mitad de lo que se encoge. La camara traslada antes de escalar. */
				int32_t depth = vm.persp_z11 - (s->depth - vm.persp_zc), scale, comp = 256, left, top;

				if (depth <= 0)
					continue;
				scale = vm.persp_z11 * 256 / depth;
				if (s->zzoom)
					comp = (vm.persp_z11 - s->depth) * 256 / vm.persp_z11;
				if (comp <= 0 || scale * comp / 256 > 32 * 256)
					continue;
				if (s->zzoom)
					x += w * (256 - comp) / 512;
				y += h * (256 - comp) / 512;
				left = SCREEN_W / 2 + ((x - cam_x - SCREEN_W / 2) * scale) / 256;
				top = VIEW_Y + VIEW_H / 2 + ((y - cam_y - (VIEW_Y + VIEW_H / 2)) * scale) / 256;
				#if defined(VN_PSP_NATIVE_MENU) || defined(VN_VITA_NATIVE_MENU)
				img_draw_ex_alpha_rot(img, layer, left + scene_dx, top + scene_dy, lk.flip,
				                     (lk.zoom * cam_z / 256) * scale * comp / (256 * 256), lk.r, lk.g, lk.b,
				                     lk.angle, lk.alpha);
				#else
				img_draw_ex_rot(img, layer, left + scene_dx, top + scene_dy, lk.flip,
				                (lk.zoom * cam_z / 256) * scale * comp / (256 * 256), lk.r, lk.g, lk.b,
				                lk.angle);
				#endif
			} else {
				#if defined(VN_PSP_NATIVE_MENU) || defined(VN_VITA_NATIVE_MENU)
				img_draw_ex_alpha_rot(img, layer, cam_sx(x) + scene_dx, cam_sy(y) + scene_dy, lk.flip,
				                     lk.zoom * cam_z / 256, lk.r, lk.g, lk.b, lk.angle, lk.alpha);
				#else
				img_draw_ex_rot(img, layer, cam_sx(x) + scene_dx, cam_sy(y) + scene_dy, lk.flip,
				                lk.zoom * cam_z / 256, lk.r, lk.g, lk.b, lk.angle);
				#endif
			}
		}
		if (!vm.persp_on && covers_view(img, s)) {
			video_clip_scene();
			return;
		}
	}
	draw_bg();
	video_clip_scene();
}

/* ---------------------------------------------------------------- interfaz */
/* with_text = 0: caja vacia (pausa con la ventana visible) */
static void draw_box(int with_text)
{
	const Speaker *sp = vm.ev_speaker < speaker_count ? &speakers[vm.ev_speaker] : &speakers[0];
	int k = box_level;

#ifdef VN_NATIVE_SNOOT_MENU
	snoot_psp_dialog_draw(&run, sp->name, sp->r, sp->g, sp->b, k, BOX_FADE, with_text);
	return;
#endif
	if (k >= BOX_FADE)
		video_rect(LAYER_BOX, 0, BOX_Y, SCREEN_W, SCREEN_H - BOX_Y, 0, 0, 0, 1);
	else /* a medio fundido se resta un gris: en tonos medios coincide con oscurecer al 50% al llegar al final */
		video_rect(LAYER_BOX, 0, BOX_Y, SCREEN_W, SCREEN_H - BOX_Y, 64 * k / BOX_FADE, 64 * k / BOX_FADE,
		           64 * k / BOX_FADE, 3);
	if (!with_text)
		return;
	if (sp->name[0])
		font_draw(LAYER_TEXT, 8, BOX_Y + 2, sp->name, sp->r * k / BOX_FADE, sp->g * k / BOX_FADE,
		          sp->b * k / BOX_FADE);
	text_draw(&run, LAYER_TEXT, 8, BOX_Y + 15, 128 * k / BOX_FADE, 128 * k / BOX_FADE, 128 * k / BOX_FADE);
	if (k >= BOX_FADE && text_page_done(&run) && ((video_frames() / 20) & 1))
		video_triangle_down(LAYER_TEXT, 290, SCREEN_H - 24, 10, 7, 255, 255, 255);
}

/* Lleva la caja hacia visible u oculta, un paso por cuadro (al saltar, de golpe) */
static void update_box(void)
{
	int target =
	    show_box && (state == ST_SAY || (win_on && (state == ST_RUN || state == ST_PAUSE || state == ST_TRANS)));

	if (!show_box || skip_mode)
		box_level = target ? BOX_FADE : 0;
	else if (target && box_level < BOX_FADE)
		box_level++;
	else if (!target && box_level > 0)
		box_level--;
}

static int option_lines(const char *s)
{
	int n = 1;

	for (; *s; s++)
		if (*s == '\n')
			n++;
	return n;
}

static void menu_begin(void)
{
	int i;

	for (i = 0; i < vm.menu_n; i++)
		menu_order[i] = i;
	if (vm.menu_flags & 1)
		for (i = vm.menu_n - 1; i > 0; i--) {
			int j = random_below(i + 1), t = menu_order[i];

			menu_order[i] = menu_order[j];
			menu_order[j] = t;
		}
	menu_cursor = 0;
	menu_top = 0;
}

/* Lista las opciones desde menu_top; ajusta menu_top para que el cursor quede visible */
/* Menu de imagenes (OP_IMGMENU): fondo y botones en su sitio; un boton ya visto se ve apagado. */
static void draw_imgmenu(void)
{
	const Image *bg = scene_image(vm.menu_bg);
	int i;

	if (bg && bg->big)
		img_draw_big(bg, LAYER_BG, 0, VIEW_Y);
	else if (bg)
		img_draw(bg, LAYER_BG, 0, VIEW_Y);
	for (i = 0; i < vm.menu_n; i++) {
		const Image *img = scene_image(vm.menu_img_id[i]);
		int dim = vm.menu_seen[i] != 0xFFFF && seen_get(vm.menu_seen[i]);

		if (img)
			img_draw_ex(img, LAYER_SPRITE0, vm.menu_x[i], VIEW_Y + vm.menu_y[i], 0, 64, dim ? 64 : 128,
			            dim ? 64 : 128, dim ? 64 : 128);
	}
	if (menu_cursor < vm.menu_n) {
		const Image *c = scene_image(vm.menu_img_id[menu_cursor]);
		int x = vm.menu_x[menu_cursor], y = VIEW_Y + vm.menu_y[menu_cursor];

		if (c) {
			int w = c->full_w, h = c->full_h;

			video_rect(LAYER_TEXT, x, y, w, 1, 255, 255, 0, 0);
			video_rect(LAYER_TEXT, x, y + h - 1, w, 1, 255, 255, 0, 0);
			video_rect(LAYER_TEXT, x, y, 1, h, 255, 255, 0, 0);
			video_rect(LAYER_TEXT, x + w - 1, y, 1, h, 255, 255, 0, 0);
		}
	}
}

static void draw_menu(void)
{
	if (vm.menu_img) {
		draw_imgmenu();
		return;
	}
	#ifdef VN_VITA_NATIVE_MENU
	{
		const char *choices[VM_MENU_MAX];
		int i;

		for (i = 0; i < vm.menu_n; i++)
			choices[i] = text_string(vm.menu_text[menu_order[i]]);
		snoot_psp_choice_draw(choices, vm.menu_n, menu_cursor);
		return;
	}
	#endif
	int lh = font_line_height(), i, y, last;

	video_rect(LAYER_BOX, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 1);
	for (;;) {
		y = MENU_TOP;
		last = -1;
		for (i = menu_top; i < vm.menu_n; i++) {
			int h = option_lines(text_string(vm.menu_text[menu_order[i]])) * lh + 8;

			if (y + h > MENU_BOTTOM)
				break;
			y += h;
			last = i;
		}
		if (menu_cursor > last && menu_top < menu_cursor)
			menu_top++;
		else
			break;
	}
	y = MENU_TOP;
	for (i = menu_top; i <= last; i++) {
		const char *s = text_string(vm.menu_text[menu_order[i]]);
		int selected = i == menu_cursor;

		if (selected)
			video_triangle_down(LAYER_TEXT, 6, y + 4, 8, 6, 255, 220, 40);
		font_draw(LAYER_TEXT, 20, y, s, selected ? 255 : 128, selected ? 220 : 128, selected ? 40 : 128);
		y += option_lines(s) * lh + 8;
	}
}

/* ---------------------------------------------------------------- flujo */
static void begin_special(int id);

static void start_say(void)
{
	int gid = vm.ev_gid;

	say_seen_before = seen_get(gid);
	say_prefetched = 0;
	say_auto_elapsed_frames = 0;
	text_start(&run, text_string(vm.ev_text));
	state = ST_SAY;
}

static void run_vm(void)
{
	VmEvent ev;

	audio_batch_begin(); /* todas las lecturas de este paso comparten una sola pausa de la musica */
	res_trace_tag = (uint32_t)vm.chapter << 16;
	vm.now = (uint32_t)video_frames();
	ev = vm_run(&vm);
	VN_ETAPA(64); /* vm_run devolvio: los SHOW que se ven en el log salen de ahi dentro */

	{
		int changed = scene_dirty && !skip_mode;

		/* lo nuevo se carga antes de reservar la copia de la transicion (que sale del framebuffer, no de las
		 * texturas): asi la reserva sabe que espacio queda de verdad */
		int t0 = video_vblank_count(), cd0 = cd_sync_vblanks + cd_wait_vblanks;

		scene_sync();
		VN_ETAPA(128); /* scene_sync devolvio: aqui dentro estan los res_get y las subidas de textura */
		decode_vblanks += video_vblank_count() - t0 - (cd_sync_vblanks + cd_wait_vblanks - cd0);
		if (ev == VM_EV_TRANSITION) {
			snap_box = box_level >= BOX_FADE;
			begin_transition();
			if (!win_on)
				box_level = 0; /* la caja que se oculta ya va en la copia y se desvanece con ella */
		}
		VN_ETAPA(256); /* transicion preparada */
		/* si ya hubo que leer el disco, se aprovecha la pausa para traer las imagenes de las escenas que siguen */
		if (changed && audio_cd_paused()) {
			uint16_t ids[LOOKAHEAD_IMAGES];

			VN_ETAPA(
			    257); /* la rama se ejecuto de verdad (sin esto no se sabe si el test de PC probo algo) */
			res_prefetch(ids, look_ahead(600, ids, 0, 1));
		}
		VN_ETAPA(512); /* lookahead/prefetch devolvio; lo que sigue es el switch del evento */
	}
	switch (ev) {
	case VM_EV_SAY:
		start_say();
		break;
	case VM_EV_MENU:
		menu_begin();
		state = ST_MENU;
		break;
	case VM_EV_PAUSE:
		pause_left = vm.ev_frames;
		state = ST_PAUSE;
		break;
	case VM_EV_SPECIAL:
		begin_special(vm.ev_args[0]);
		break;
	case VM_EV_END:
		state = ST_END;
		break;
	case VM_EV_ERROR:
		printf("VM: %s\n", vm.error);
		state = ST_ERROR;
		break;
	default:
		break;
	}
	audio_batch_end();
}

static void clear_scene(void)
{
	memset(vm.sprites, 0, sizeof(vm.sprites));
	vm.bg = 0;
	vm.bg_anim = 0;
	scene_dirty = 1;
}

static void begin(int chapter, uint32_t offset, int var, int value)
{
	clear_scene();
	audio_stop_all();
	memset(vm.vars, 0, sizeof(vm.vars));
	if (var < VM_VARS)
		vm.vars[var] = (int16_t)value;
	skip_mode = 0;
	play_frames = 0;
	win_auto = 1;
	win_on = 0;
	box_level = 0;
	if (vm_start(&vm, chapter, offset) != 0)
		fatal("no se pudo iniciar el capitulo");
	state = ST_RUN;
}

static void go_title(void);

/* Fila 0: la historia completa; las demas, las entradas de la tabla */
static void update_dev(void)
{
	int i;

	if (input_pressed(BTN_UP) && menu_cursor > 0)
		menu_cursor--;
	if (input_pressed(BTN_DOWN) && menu_cursor < dev_count + extra_count)
		menu_cursor++;
	if (input_pressed(BTN_CIRCLE)) {
#ifdef VN_VITA_NATIVE_MENU
		if (dev_back != ST_TITLE) {
			state = dev_back;
			return;
		}
#endif
		go_title();
		return;
	}
	if (!input_pressed(BTN_CROSS))
		return;
	dev_session = 1;
	playing_extra = 0;
	if (menu_cursor == 0) {
		begin(start_chapter, start_offset, 255, 0);
		return;
	}
	i = menu_cursor - 1;
	if (i >= dev_count) { /* despues de la historia, los capitulos extra */
		i -= dev_count;
		begin(extra_chapter[i], extra_offset[i], 255, 0);
		playing_extra = 1;
		return;
	}
	begin(dev_chapter, dev_offset[i], dev_var[i], dev_value[i]);
}

static void draw_dev(void)
{
	int lh = font_line_height(), rows = (MENU_BOTTOM - MENU_TOP - lh) / lh, i, y;

	video_rect(LAYER_BOX, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 1);
	font_draw(LAYER_TEXT, 20, MENU_TOP - 8, "Menu de capitulos (desarrollo)", 255, 220, 40);
	if (menu_cursor < menu_top)
		menu_top = menu_cursor;
	if (menu_cursor >= menu_top + rows)
		menu_top = menu_cursor - rows + 1;
	y = MENU_TOP - 8 + lh + 4;
	for (i = menu_top; i <= dev_count + extra_count && i < menu_top + rows; i++) {
		int selected = i == menu_cursor;

		if (selected)
			video_triangle_down(LAYER_TEXT, 6, y + 4, 8, 6, 255, 220, 40);
		font_draw(LAYER_TEXT, 20, y,
		          i == 0           ? "Historia completa"
		          : i <= dev_count ? dev_name[i - 1]
		                           : extra_name[i - 1 - dev_count],
		          selected ? 255 : 128, selected ? 220 : 128, selected ? 40 : 128);
		y += lh;
	}
}

/* ---------------------------------------------------------------- guardado (D10, docs/presupuesto-memoria.md) */
/* Datos: 0 control (save.c), 16 volumenes, 20 finales u32, 24 extras terminados u32, 32 galeria vista, 64 texto
 * leido, despues las 3 ranuras. Ranura: 0 usada, 1 llamadas, 2 capitulo u16, 4 pc u32, 8 numero de guardado u32,
 * 12 tiempo u32, 16 fondo u16, 18 anim del fondo u16, 20 musica i16, 22 ambientes 4 x i16, 32 variables 64 x i16,
 * 160 llamadas 8 x (capitulo u16, pc u32), 208 contador de orden u32, 212 sprites (copia de VmSprite). */
#define OFS_OPTIONS 16
#ifdef VN_VITA_NATIVE_MENU
#define OFS_VITA_PREFS (SAVE_DATA_BYTES - 8)
#endif
#define OFS_ENDINGS 20
#define OFS_BONUS   24
#define OFS_GALLERY 32
#define OFS_SEEN    64
#define OFS_SLOTS   (OFS_SEEN + SEEN_BYTES)
#define SLOT_BYTES  1024
#define OFS_CANVAS  (OFS_SLOTS + SLOTS * SLOT_BYTES) /* dibujo del lienzo (1152 bytes, al final: no mueve nada) */
#define SL_VARS     32
#define SL_CALLS    160
#define SL_BORN     208
#define SL_SPRITES  212

typedef char SaveLayoutFits[(OFS_CANVAS + 1152 <= SAVE_DATA_BYTES) ? 1 : -1];
#ifdef VN_VITA_NATIVE_MENU
typedef char VitaPrefsFit[(OFS_CANVAS + 1152 <= OFS_VITA_PREFS) ? 1 : -1];
#endif

static uint8_t save_buf[SAVE_DATA_BYTES] __attribute__((aligned(4)));
static int card_state = SAVE_NO_FILE;
static uint32_t save_number;

static void wr16(uint8_t *p, int v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
	wr16(p, (int)(v & 0xffff));
	wr16(p + 2, (int)(v >> 16));
}

static uint8_t *slot_ptr(int i)
{
	return save_buf + OFS_SLOTS + i * SLOT_BYTES;
}

static void progress_to_buf(void)
{
	save_buf[OFS_OPTIONS] = (uint8_t)music_level;
	save_buf[OFS_OPTIONS + 1] = (uint8_t)sfx_level;
	save_buf[OFS_OPTIONS + 2] = text_speed_cps == 0 ? 255 : (uint8_t)text_speed_cps;
	save_buf[OFS_OPTIONS + 3] = (uint8_t)auto_forward_seconds;
#ifdef VN_VITA_NATIVE_MENU
	save_buf[OFS_VITA_PREFS] = (uint8_t)pref_transitions;
	save_buf[OFS_VITA_PREFS + 1] = (uint8_t)pref_skip_unseen;
	save_buf[OFS_VITA_PREFS + 2] = (uint8_t)pref_skip_after_choices;
	save_buf[OFS_VITA_PREFS + 3] = (uint8_t)pref_chapter_select;
	save_buf[OFS_VITA_PREFS + 4] = 0xa5; /* marca las preferencias extendidas Vita */
#endif
	wr32(save_buf + OFS_ENDINGS, endings_mask);
	wr32(save_buf + OFS_BONUS, bonus_mask);
	memcpy(save_buf + OFS_GALLERY, gallery_seen, sizeof(gallery_seen));
	memcpy(save_buf + OFS_SEEN, seen, SEEN_BYTES);
	canvas_bits(save_buf + OFS_CANVAS);
}

static void progress_from_buf(void)
{
	int i;

	music_level = save_buf[OFS_OPTIONS] <= 10 ? save_buf[OFS_OPTIONS] : 10;
	sfx_level = save_buf[OFS_OPTIONS + 1] <= 10 ? save_buf[OFS_OPTIONS + 1] : 10;
	text_speed_cps = save_buf[OFS_OPTIONS + 2] == 255 ? 0
	                 : save_buf[OFS_OPTIONS + 2] >= 20 && save_buf[OFS_OPTIONS + 2] <= 100
	                     ? save_buf[OFS_OPTIONS + 2]
	                     : TEXT_DEFAULT_CPS;
	switch (save_buf[OFS_OPTIONS + 3]) {
	case 5:
	case 10:
	case 15:
	case 20:
	case 30:
		auto_forward_seconds = save_buf[OFS_OPTIONS + 3];
		break;
	default:
		auto_forward_seconds = 0;
		break;
	}
#ifdef VN_VITA_NATIVE_MENU
	if (save_buf[OFS_VITA_PREFS + 4] == 0xa5) {
		pref_transitions = save_buf[OFS_VITA_PREFS] <= 2 ? save_buf[OFS_VITA_PREFS] : 2;
		pref_skip_unseen = save_buf[OFS_VITA_PREFS + 1] == 1;
		pref_skip_after_choices = save_buf[OFS_VITA_PREFS + 2] == 1;
		pref_chapter_select = save_buf[OFS_VITA_PREFS + 3] == 1;
	} else {
		pref_transitions = 2;
		pref_skip_unseen = 0;
		pref_skip_after_choices = 1;
		pref_chapter_select = 1;
		auto_forward_seconds = 15;
	}
#endif
	text_set_default_cps(text_speed_cps);
#ifdef VN_TRACE
	printf("settings: loaded music=%d sfx=%d text_cps=%d auto=%d\n", music_level, sfx_level, text_speed_cps,
	       auto_forward_seconds);
#endif
	endings_mask = rd32(save_buf + OFS_ENDINGS);
	bonus_mask = rd32(save_buf + OFS_BONUS);
	memcpy(gallery_seen, save_buf + OFS_GALLERY, sizeof(gallery_seen));
	memcpy(seen, save_buf + OFS_SEEN, SEEN_BYTES);
	canvas_load(save_buf + OFS_CANVAS);
	for (i = 0; i < SLOTS; i++)
		if (rd32(slot_ptr(i) + 8) > save_number)
			save_number = rd32(slot_ptr(i) + 8);
	audio_set_levels(music_level, sfx_level);
}

static int newest_slot(void)
{
	int i, best = -1;

	for (i = 0; i < SLOTS; i++)
		if (slot_ptr(i)[0] && (best < 0 || rd32(slot_ptr(i) + 8) > rd32(slot_ptr(best) + 8)))
			best = i;
	return best;
}

/* Pantalla de un solo mensaje dibujada ya (las operaciones de la memory card bloquean) */
static void show_now(const char *msg)
{
	int i;

	for (i = 0; i < 2; i++) {
		video_rect(LAYER_BOX, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0);
		draw_center(SCREEN_H / 2 - 8, msg, 128, 128, 128);
		video_flip();
	}
}

static const char *card_error(int result)
{
	switch (result) {
	case SAVE_NO_CARD:
		return TX(T_NO_CARD);
	case SAVE_FULL:
		return TX(T_CARD_FULL);
	case SAVE_WRITE_ERROR:
		return TX(T_CARD_WRITE_FAILED);
	}
	return 0;
}

static int card_disabled; /* Select al arrancar: se juega sin memory card */

static int write_card(const char *msg)
{
	if (card_disabled)
		return card_state = SAVE_NO_CARD;
	show_now(msg);
	progress_to_buf();
	card_state = save_write(save_buf);
	return card_state;
}

static void save_slot(int i)
{
	uint8_t *s = slot_ptr(i);
	int k;

	memset(s, 0, SLOT_BYTES);
	s[0] = 1;
	s[1] = (uint8_t)vm.ncalls;
	wr16(s + 2, vm.chapter);
	wr32(s + 4, vm.ev_pc);
	wr32(s + 8, ++save_number);
	wr32(s + 12, play_frames);
	wr16(s + 16, vm.bg);
	wr16(s + 18, vm.bg_anim);
	wr16(s + 20, audio_music_current());
	for (k = 0; k < 4; k++)
		wr16(s + 22 + 2 * k, audio_ambient_current(k));
	for (k = 0; k < VM_VARS; k++)
		wr16(s + SL_VARS + 2 * k, vm.vars[k]);
	for (k = 0; k < vm.ncalls && k < VM_CALLS; k++) {
		wr16(s + SL_CALLS + 6 * k, vm.calls[k].chapter);
		wr32(s + SL_CALLS + 6 * k + 2, vm.calls[k].pc);
	}
	wr32(s + SL_BORN, vm.born_counter);
	memcpy(s + SL_SPRITES, vm.sprites, sizeof(vm.sprites));
}

static void load_slot(int i)
{
	const uint8_t *s = slot_ptr(i);
	int k, music;

	clear_scene();
	audio_stop_all();
	skip_mode = 0;
	win_auto = 1;
	win_on = 0;
	box_level = 0;
	if (vm_start(&vm, rd16(s + 2), rd32(s + 4)) != 0)
		fatal("no se pudo cargar la partida");
	for (k = 0; k < VM_VARS; k++)
		vm.vars[k] = (int16_t)rd16(s + SL_VARS + 2 * k);
	vm.ncalls = s[1] < VM_CALLS ? s[1] : VM_CALLS;
	for (k = 0; k < vm.ncalls; k++) {
		vm.calls[k].chapter = rd16(s + SL_CALLS + 6 * k);
		vm.calls[k].pc = rd32(s + SL_CALLS + 6 * k + 2);
	}
	vm.born_counter = rd32(s + SL_BORN);
	memcpy(vm.sprites, s + SL_SPRITES, sizeof(vm.sprites));
	memset(vm.from, 0, sizeof(vm.from));
	for (k = 0; k < VM_MAX_SPRITES; k++) {
		vm.sprites[k].shown_at = 0;         /* las animaciones quedan asentadas */
		vm.sprites[k].sound_t = 0x7fffffff; /* y sus sonidos no se repiten */
	}
	vm.bg = rd16(s + 16);
	vm.bg_anim = rd16(s + 18);
	bg_start = -100000;
	play_frames = rd32(s + 12);
	music = (int16_t)rd16(s + 20);
	if (music >= 0)
		audio_music_play(music, 10, 0);
	for (k = 0; k < 4; k++) {
		int amb = (int16_t)rd16(s + 22 + 2 * k);

		if (amb >= 0)
			audio_ambient_play(k, amb, 10);
	}
	playing_extra = 0;
	dev_session = 0;
	camera_reset();
	scene_dirty = 1;
	state = ST_RUN;
#ifdef VN_TRACE
	printf("load: slot=%d chapter=%u pc=%lu\n", i + 1, (unsigned)rd16(s + 2), (unsigned long)rd32(s + 4));
#endif
}

/* ---------------------------------------------------------------- pantallas */
enum { UI_SAVE, UI_LOAD };
enum { CONF_OVERWRITE, CONF_QUIT };

static int ui_cursor, ui_mode, ui_slot, ui_changed, gallery_pos;
static State ui_back;    /* a donde vuelve la pantalla actual con O */
static State pause_from; /* estado del juego al abrir la pausa */
static const char *notice_text;
static State notice_next;
static const char *confirm_text;
static int confirm_action, confirm_yes;

static void notice(const char *text, State next)
{
	notice_text = text;
	notice_next = next;
	state = ST_NOTICE;
}

/* Texto centrado; cada linea (separada por \n) se centra por separado */
void draw_center(int y, const char *s, int r, int g, int b)
{
	char line[96];

	while (*s) {
		int n = 0;

		while (s[n] && s[n] != '\n' && n < (int)sizeof(line) - 1)
			n++;
		memcpy(line, s, n);
		line[n] = 0;
		font_draw(LAYER_TEXT, (SCREEN_W - font_width(line)) / 2, y, line, r, g, b);
		y += font_line_height();
		s += n;
		if (*s == '\n')
			s++;
	}
}

/* Lista centrada; `disabled` marca en gris las opciones que no se pueden usar */
static void draw_list(int y, const char *const *items, int n, int cursor, uint32_t disabled)
{
	int lh = font_line_height() + font_line_height() * 2 / 7, i;
	y = SYS_Y(y);

	for (i = 0; i < n; i++, y += lh) {
		int gray = (disabled >> i) & 1, sel = i == cursor;

		if (sel)
			video_triangle_down(LAYER_TEXT, (SCREEN_W - font_width(items[i])) / 2 - SYS_X(14), y + SYS_Y(4),
			                    SYS_X(8), SYS_Y(6), 255, 220, 40);
		draw_center(y, items[i],
		            gray  ? 64
		            : sel ? 255
		                  : 128,
		            gray  ? 64
		            : sel ? 220
		                  : 128,
		            gray  ? 64
		            : sel ? 40
		                  : 128);
	}
}

/* Arriba/abajo con vuelta; devuelve 1 con X, -1 con O */
static int list_input(int *cursor, int n)
{
	if (input_pressed(BTN_UP))
		*cursor = (*cursor + n - 1) % n;
	if (input_pressed(BTN_DOWN))
		*cursor = (*cursor + 1) % n;
	if (input_pressed(BTN_CROSS)) {
#ifdef VN_NATIVE_AUDIO
		audio_native_ui(0);
#endif
		return 1;
	}
	if (input_pressed(BTN_CIRCLE)) {
#ifdef VN_NATIVE_AUDIO
		audio_native_ui(1);
#endif
		return -1;
	}
	return 0;
}

#ifdef VN_VITA_NATIVE_MENU
/* Tocar una fila selecciona y activa la misma opción que X. Coordenadas de pantalla del renderer. */
static int touch_list_row(int top, int step, int height, int n, int left, int right, int *row)
{
	int x, y, i;
	if (!input_touch(&x, &y) || x < left || x >= right)
		return 0;
	for (i = 0; i < n; i++)
		if (y >= top + i * step && y < top + i * step + height) {
			*row = i;
			return 1;
		}
	return 0;
}
#endif

void panel(void)
{
	video_rect(LAYER_BOX, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 1);
	video_rect(LAYER_BOX, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 1);
}

static void draw_system_panel(void)
{
#ifdef VN_VITA_NATIVE_MENU
	if (snoot_psp_menu_draw("SYSTEM_MENU"))
		return;
#endif
	panel();
}

/* Capitulos extra visibles: la regla es de cada juego (games/<juego>/game.c) */
static int extras_list(int *out)
{
	int i, n = 0;

	for (i = 0; i < extra_count; i++)
		if (game_extra_unlocked(i, endings_mask, bonus_mask))
			out[n++] = i;
	return n;
}

static void go_title(void)
{
	clear_scene();
	camera_reset();
	audio_stop_all();
	skip_mode = 0;
	dev_session = 0;
	playing_extra = 0;
#ifdef VN_NATIVE_SNOOT_MENU
	ui_cursor = 0;
#else
	ui_cursor = newest_slot() >= 0 ? 1 : 0;
#endif
	state = ST_TITLE;
#ifdef VN_NATIVE_AUDIO
	audio_native_title();
	audio_set_levels(music_level, sfx_level);
#endif
}

#define title_items         (game_text + T_TITLE_START) /* 5 entradas seguidas en ui_text.h */
#define BONUS_MENU_EPILOGUE 0x80000000u

static void open_slots(int mode, State back)
{
	ui_mode = mode;
	ui_back = back;
	ui_cursor = newest_slot() >= 0 ? newest_slot() : 0;
	state = ST_SLOTS;
}

static void open_options(State back)
{
	ui_back = back;
	ui_cursor = 0;
	ui_changed = 0;
	state = ST_OPTIONS;
}

/* Filas del titulo: 0 comenzar, 1 continuar, 2 capitulos extra (solo si el juego tiene), 3 galeria, 4 opciones */
static int title_rows(int *row)
{
#ifdef VN_NATIVE_SNOOT_MENU
	/* Conserva las cinco opciones del menú portátil basado en el arte original. */
	static const int original[] = {0, 1, 2, 3, 4};
	memcpy(row, original, sizeof(original));
	return 5;
#else
	int n = 0, i;

	for (i = 0; i < 5; i++)
		if (i != 2 || extra_count)
			row[n++] = i;
	return n;
#endif
}

static void update_title(void)
{
	int extras[MAX_EXTRAS], rows[6], n = title_rows(rows);
#ifdef VN_VITA_NATIVE_MENU
	int touched_row;
#endif

#ifdef VN_VITA_NATIVE_MENU
	if (input_down(BTN_L1) && input_down(BTN_R1) && input_pressed(BTN_SELECT) && dev_count && pref_chapter_select) {
		menu_cursor = menu_top = 0;
		dev_back = ST_TITLE;
		state = ST_DEV;
		return;
	}
#endif
	if (ui_cursor >= n)
		ui_cursor = n - 1;
	{
		int activated = 0;
#ifdef VN_VITA_NATIVE_MENU
		if (touch_list_row(SCREEN_W * 82 / 480, SCREEN_W * 33 / 480, SCREEN_W * 32 / 480, n,
		                   SCREEN_W * 340 / 480, SCREEN_W * 465 / 480, &touched_row)) {
			ui_cursor = touched_row;
			activated = 1;
		}
#endif
		if (!activated && list_input(&ui_cursor, n) != 1)
			return;
	}
	switch (rows[ui_cursor]) {
	case 0:
		dev_session = 0;
		playing_extra = 0;
		begin(start_chapter, start_offset, 255, 0);
		break;
	case 1:
		if (newest_slot() < 0)
			notice(card_state == SAVE_NO_CARD ? TX(T_NO_CARD) : TX(T_NO_SAVES), ST_TITLE);
		else
			open_slots(UI_LOAD, ST_TITLE);
		break;
	case 2:
		if (extras_list(extras) == 0)
			notice(TX(T_EXTRAS_LOCKED), ST_TITLE);
		else {
			ui_cursor = 0;
			state = ST_EXTRAS;
		}
		break;
	case 3:
#ifdef VN_NATIVE_SNOOT_MENU
		snoot_psp_gallery_open();
#endif
		gallery_pos = 0;
		state = ST_GALLERY;
		break;
	case 4:
		open_options(ST_TITLE);
		break;
	}
}

static void draw_title(void)
{
#ifdef VN_NATIVE_SNOOT_MENU
	char page[16];
	snprintf(page, sizeof(page), "MENU%d%s", ui_cursor, bonus_mask & BONUS_MENU_EPILOGUE ? "E" : "");
	if (!snoot_psp_menu_draw(page)) {
		draw_center(SCREEN_H / 3, "Faltan los recursos del menú", 255, 128, 128);
		draw_center(SCREEN_H / 2, "Reinstala la carpeta DATA del port", 128, 128, 128);
	}
#else
	const Image *logo = logo_image ? res_get(logo_image) : 0;
	uint32_t disabled = newest_slot() < 0 ? 2 : 0;
	int extras[MAX_EXTRAS], rows[5], items_n = title_rows(rows), k;
	const char *items[5];
	int title_y = 130;
#if SCREEN_W > 320
	int line_height = font_line_height() + font_line_height() * 2 / 7;
	int list_top = (SCREEN_H - items_n * line_height) / 2;
	title_y = list_top * 240 / SCREEN_H;
#endif

	for (k = 0; k < items_n; k++)
		items[k] = title_items[rows[k]];
	if (extra_count && !extras_list(extras))
		disabled |= 4;
	if (logo && logo->kind <= 1)
		img_draw(logo, LAYER_TEXT, (SCREEN_W - logo->full_w) / 2, 12);
	draw_list(title_y, items, items_n, ui_cursor, disabled);
	if (disabled & 4) {
		const Image *lock = lock_image ? res_get(lock_image) : 0;

		if (lock && lock->kind <= 1)
			img_draw(lock, LAYER_TEXT, (SCREEN_W + font_width(items[2])) / 2 + SYS_X(6),
			         SYS_Y(title_y) + 2 * (font_line_height() + font_line_height() * 2 / 7) + SYS_Y(2));
	}
#endif
}

#define pause_items (game_text + T_PAUSE_RESUME) /* 6 entradas seguidas en ui_text.h */

static int pause_opened; /* cuadro en que se abrio (el mismo Start no la cierra) */

static void update_pause_menu(void)
{
	int r = list_input(&ui_cursor, 6);
#ifdef VN_VITA_NATIVE_MENU
	int row;
	if (touch_list_row(SYS_Y(70), font_line_height() + font_line_height() * 2 / 7, font_line_height() + 2, 6,
	                   SYS_X(20), SCREEN_W - SYS_X(20), &row)) {
		ui_cursor = row;
		r = 1;
	}
#endif

	if (r < 0 || (input_pressed(BTN_START) && video_frames() != pause_opened)) {
		state = pause_from;
		return;
	}
	if (r != 1)
		return;
	switch (ui_cursor) {
	case 0:
		state = pause_from;
		break;
	case 1:
		state = pause_from;
		skip_mode = 1;
		break;
	case 2:
		open_slots(UI_SAVE, ST_PMENU);
		break;
	case 3:
		open_slots(UI_LOAD, ST_PMENU);
		break;
	case 4:
		open_options(ST_PMENU);
		break;
	case 5:
		confirm_text = TX(T_CONFIRM_QUIT);
		confirm_action = CONF_QUIT;
		confirm_yes = 0;
		ui_back = ST_PMENU;
		state = ST_CONFIRM;
		break;
	}
}

static void do_save(int slot)
{
	const char *err;

	save_slot(slot);
	err = card_error(write_card(TX(T_SAVING)));
	if (err) {
		slot_ptr(slot)[0] = 0;
		notice(err, ST_PMENU);
	} else {
	#ifdef VN_TRACE
		printf("save: slot=%d chapter=%u pc=%lu result=0\n", slot + 1, (unsigned)rd16(slot_ptr(slot) + 2),
		       (unsigned long)rd32(slot_ptr(slot) + 4));
	#endif
		notice(TX(T_SAVED), pause_from);
	}
}

static void update_slots(void)
{
	int r = list_input(&ui_cursor, SLOTS);
#ifdef VN_VITA_NATIVE_MENU
	int row;
	if (touch_list_row(VITA_SLOTS_TOP, VITA_SLOTS_STEP, VITA_SLOTS_H, SLOTS,
	                   VITA_SLOTS_X, SCREEN_W - VITA_SLOTS_X, &row)) {
		ui_cursor = row;
		r = 1;
	}
#endif

	if (r < 0) {
		ui_cursor = 0;
		state = ui_back;
		return;
	}
	if (r != 1)
		return;
	if (ui_mode == UI_LOAD) {
		if (slot_ptr(ui_cursor)[0])
			load_slot(ui_cursor);
		return;
	}
	if (slot_ptr(ui_cursor)[0]) {
		confirm_text = TX(T_CONFIRM_OVERWRITE);
		confirm_action = CONF_OVERWRITE;
		confirm_yes = 0;
		ui_slot = ui_cursor;
		state = ST_CONFIRM;
		return;
	}
	do_save(ui_cursor);
}

static void draw_slots(void)
{
	int i, newest = newest_slot();
	char line[96];
#ifndef VN_VITA_NATIVE_MENU
	int lh = font_line_height() + 10;
#endif

	draw_system_panel();
#ifdef VN_VITA_NATIVE_MENU
	font_draw(LAYER_DEBUG, VITA_SLOTS_X, SYS_Y(16), ui_mode == UI_SAVE ? "GUARDAR PARTIDA" : "CARGAR PARTIDA",
	          224, 244, 255);
	font_draw(LAYER_DEBUG, VITA_SLOTS_X, SYS_Y(36), "SELECCIONA UN ESPACIO", 120, 190, 208);
	for (i = 0; i < SLOTS; i++) {
		const uint8_t *s = slot_ptr(i);
		int x = VITA_SLOTS_X, y = VITA_SLOTS_TOP + i * VITA_SLOTS_STEP;
		int w = SCREEN_W - VITA_SLOTS_X * 2, h = VITA_SLOTS_H, sel = i == ui_cursor;
		uint32_t secs = s[0] ? rd32(s + 12) / 60 : 0;
		int ch = s[0] ? rd16(s + 2) : -1;

		/* Tarjeta oscura con acento turquesa y borde de seleccion en el estilo del menu Vita. */
		video_rect(LAYER_TEXT, x, y, w, h, 12, 24, 49, 1);
		video_rect(LAYER_TEXT, x, y, w, SYS_Y(1) + (sel ? SYS_Y(1) : 0), sel ? 92 : 48,
		           sel ? 225 : 112, sel ? 245 : 156, 0);
		video_rect(LAYER_TEXT, x, y, sel ? SYS_X(2) : SYS_X(1), h,
		           sel ? 92 : 48, sel ? 225 : 112, sel ? 245 : 156, 0);
		video_rect(LAYER_TEXT, x + SYS_X(12), y + SYS_Y(9), SYS_X(28), SYS_Y(22),
		           sel ? 24 : 30, sel ? 92 : 48, sel ? 118 : 72, 1);
		snprintf(line, sizeof(line), "%02d", i + 1);
		font_draw(LAYER_DEBUG, x + SYS_X(17), y + SYS_Y(11), line, 160, 235, 250);
		font_draw(LAYER_DEBUG, x + SYS_X(50), y + SYS_Y(8),
		          !s[0] ? "ESPACIO DISPONIBLE" : i == newest ? "PARTIDA MAS RECIENTE" : "PARTIDA GUARDADA",
		          sel ? 120 : 96, sel ? 222 : 170, sel ? 245 : 192);
		if (s[0]) {
			font_draw(LAYER_DEBUG, x + SYS_X(50), y + SYS_Y(20),
			          ch >= 0 && ch < MAX_CHAPTERS ? chapter_name[ch] : "Capitulo guardado", 245, 248, 255);
			snprintf(line, sizeof(line), "TIEMPO JUGADO   %02d:%02d", (int)(secs / 3600), (int)(secs / 60 % 60));
			font_draw(LAYER_DEBUG, x + SYS_X(50), y + SYS_Y(32), line, 138, 175, 194);
		} else {
			font_draw(LAYER_DEBUG, x + SYS_X(50), y + SYS_Y(20),
			          ui_mode == UI_SAVE ? "Toca esta tarjeta para guardar aqui" : "No hay datos en este espacio",
			          190, 202, 216);
		}
		if (sel) {
			font_draw(LAYER_DEBUG, x + w - SYS_X(58), y + SYS_Y(16), "X  ELEGIR", 115, 230, 248);
			video_rect(LAYER_TEXT, x + w - SYS_X(8), y + SYS_Y(8), SYS_X(2), h - SYS_Y(16), 92, 225, 245, 0);
		}
	}
	font_draw(LAYER_DEBUG, VITA_SLOTS_X, SCREEN_H - SYS_Y(18), "TOCA UNA TARJETA   X ELEGIR   O VOLVER", 112, 155, 176);
#else
	draw_center(SYS_Y(30), ui_mode == UI_SAVE ? TX(T_SAVE_TITLE) : TX(T_LOAD_TITLE), 255, 220, 40);
	for (i = 0; i < SLOTS; i++) {
		const uint8_t *s = slot_ptr(i);
		int y = SYS_Y(70) + i * lh, sel = i == ui_cursor, c = sel ? 255 : 128;

		if (s[0]) {
			uint32_t secs = rd32(s + 12) / 60;
			int ch = rd16(s + 2);

			sprintf(line, TX(T_SLOT_FORMAT), i + 1, ch < MAX_CHAPTERS ? chapter_name[ch] : "",
			        (int)(secs / 3600), (int)(secs / 60 % 60), i == newest ? "  *" : "");
		} else {
			sprintf(line, TX(T_SLOT_EMPTY_FORMAT), i + 1);
		}
		if (sel)
			video_triangle_down(LAYER_TEXT, SYS_X(14), y + SYS_Y(4), SYS_X(8), SYS_Y(6), 255, 220, 40);
		font_draw(LAYER_TEXT, SYS_X(28), y, line, c, sel ? 220 : 128, sel ? 40 : 128);
	}
	draw_center(SCREEN_H - SYS_Y(30), TX(T_SLOTS_HELP), 96, 96, 96);
#endif
}

static void update_confirm(void)
{
	int x, y;
	int touched = input_touch(&x, &y);
	int touch_yes = 0, touch_no = 0;
	if (touched && y >= SYS_Y(108) && y <= SYS_Y(140)) {
		touch_yes = x < SYS_X(150);
		touch_no = !touch_yes;
		confirm_yes = touch_yes;
	}
	if (input_pressed(BTN_LEFT) || input_pressed(BTN_RIGHT) || input_pressed(BTN_UP) || input_pressed(BTN_DOWN))
		confirm_yes = !confirm_yes;
	if (input_pressed(BTN_CIRCLE) || (input_pressed(BTN_CROSS) && !confirm_yes) || touch_no) {
		state = confirm_action == CONF_OVERWRITE ? ST_SLOTS : ST_PMENU;
		return;
	}
	if (!input_pressed(BTN_CROSS) && !touch_yes)
		return;
	if (confirm_action == CONF_OVERWRITE) {
		do_save(ui_slot);
	} else {
		write_card(TX(T_SAVING_PROGRESS));
		go_title();
	}
}

static void draw_confirm(void)
{
	draw_system_panel();
	draw_center(SYS_Y(90), confirm_text, 128, 128, 128);
	font_draw(LAYER_TEXT, SYS_X(120), SYS_Y(120), TX(T_YES), confirm_yes ? 255 : 96, confirm_yes ? 220 : 96,
	          confirm_yes ? 40 : 96);
	font_draw(LAYER_TEXT, SYS_X(180), SYS_Y(120), TX(T_NO), confirm_yes ? 96 : 255, confirm_yes ? 96 : 220,
	          confirm_yes ? 96 : 40);
}

static void update_options(void)
{
	int *level = ui_cursor == 0 ? &music_level : ui_cursor == 1 ? &sfx_level : 0;
	int touch_x = 0, touch_y = 0, touched = input_touch(&touch_x, &touch_y), touch_activate = 0;
	int touch_left = 0, touch_right = 0;
#ifdef VN_VITA_NATIVE_MENU
	const int option_count = 11;
#elif defined(VN_NATIVE_SNOOT_MENU)
	const int option_count = 5;
#else
	const int option_count = 3;
#endif
	if (touched) {
#ifdef VN_VITA_NATIVE_MENU
		int row = (touch_y - SCREEN_W * 32 / 480) / (SCREEN_W * 20 / 480);
		if (touch_x >= SCREEN_W * 28 / 480 && touch_x < SCREEN_W * 310 / 480 && row >= 0 && row < option_count &&
		    touch_y >= SCREEN_W * 32 / 480 &&
		    touch_y < SCREEN_W * (32 + option_count * 20) / 480) {
			ui_cursor = row;
			touch_activate = 1;
			if (row < 2) {
				int start = SCREEN_W * 210 / 480, width = SCREEN_W * 80 / 480;
				int value = (touch_x - start) * 10 / width;
				if (value < 0) value = 0;
				if (value > 10) value = 10;
				if (row == 0) music_level = value; else sfx_level = value;
				ui_changed = 1;
				touch_activate = 0;
			} else if (row == 2 || row == 3 || row == 4) {
				touch_right = touch_x >= SCREEN_W * 200 / 480;
				touch_left = !touch_right;
			}
		}
#endif
	}

	if (input_pressed(BTN_UP))
		ui_cursor = (ui_cursor + option_count - 1) % option_count;
	if (input_pressed(BTN_DOWN))
		ui_cursor = (ui_cursor + 1) % option_count;
	if (level && (input_pressed(BTN_LEFT) || touch_left) && *level > 0) {
		(*level)--;
		ui_changed = 1;
	}
	if (level && (input_pressed(BTN_RIGHT) || touch_right) && *level < 10) {
		(*level)++;
		ui_changed = 1;
	}
#ifdef VN_NATIVE_SNOOT_MENU
	if (ui_cursor == 2 && (input_pressed(BTN_LEFT) || input_pressed(BTN_RIGHT) || touch_left || touch_right)) {
		static const int speeds[] = {0, 20, 35, 50, 70, 100};
		int i;

		for (i = 0; i < (int)(sizeof(speeds) / sizeof(speeds[0])) - 1; i++)
			if (text_speed_cps <= speeds[i])
				break;
		if ((input_pressed(BTN_RIGHT) || touch_right) && i < (int)(sizeof(speeds) / sizeof(speeds[0])) - 1)
			i++;
		else if ((input_pressed(BTN_LEFT) || touch_left) && i > 0 && text_speed_cps >= speeds[i])
			i--;
		text_speed_cps = speeds[i];
		text_set_default_cps(text_speed_cps);
		ui_changed = 1;
	}
	if (ui_cursor == 3 && (input_pressed(BTN_LEFT) || input_pressed(BTN_RIGHT) || touch_left || touch_right)) {
		static const int delays[] = {0, 5, 10, 15, 20, 30};
		int i;

		for (i = 0; i < (int)(sizeof(delays) / sizeof(delays[0])) - 1; i++)
			if (auto_forward_seconds <= delays[i])
				break;
		if ((input_pressed(BTN_RIGHT) || touch_right) && i < (int)(sizeof(delays) / sizeof(delays[0])) - 1)
			i++;
		else if ((input_pressed(BTN_LEFT) || touch_left) && i > 0 && auto_forward_seconds >= delays[i])
			i--;
		auto_forward_seconds = delays[i];
		ui_changed = 1;
	}
#endif
#ifdef VN_VITA_NATIVE_MENU
	if (ui_cursor == 4 && (input_pressed(BTN_LEFT) || input_pressed(BTN_RIGHT) || input_pressed(BTN_CROSS) || touch_activate)) {
		if (input_pressed(BTN_CROSS) || touch_activate)
			pref_transitions = (pref_transitions + 1) % 3;
		else
			pref_transitions = (pref_transitions + (input_pressed(BTN_RIGHT) ? 1 : 2)) % 3;
		ui_changed = 1;
	}
	if ((ui_cursor == 5 || ui_cursor == 6 || ui_cursor == 7) && (input_pressed(BTN_CROSS) || touch_activate)) {
		int *value = ui_cursor == 5 ? &pref_skip_unseen
		             : ui_cursor == 6 ? &pref_skip_after_choices
	                              : &pref_chapter_select;
		*value = !*value;
		ui_changed = 1;
	}
	if (ui_cursor == 9 && pref_chapter_select && (input_pressed(BTN_CROSS) || touch_activate)) {
		dev_back = ST_OPTIONS;
		menu_cursor = menu_top = 0;
		state = ST_DEV;
	}
#endif
	audio_set_levels(music_level, sfx_level);
	if (input_pressed(BTN_CIRCLE) || (ui_cursor == option_count - 1 && (input_pressed(BTN_CROSS) || touch_activate))) {
#ifdef VN_TRACE
		int save_result = SAVE_OK;
#endif
		if (ui_changed) {
			int result = write_card(TX(T_SAVING_OPTIONS));
#ifdef VN_TRACE
			save_result = result;
#else
			(void)result;
#endif
		}
#ifdef VN_TRACE
		printf("settings: saved music=%d sfx=%d text_cps=%d auto=%d result=%d\n", music_level, sfx_level,
		       text_speed_cps, auto_forward_seconds, save_result);
#ifdef VN_VITA_NATIVE_MENU
		printf("preferences: transitions=%d unseen=%d after_choices=%d chapter_select=%d lewd=1\n",
		       pref_transitions, pref_skip_unseen, pref_skip_after_choices, pref_chapter_select);
#endif
#endif
		ui_cursor = 0;
		state = ui_back;
	}
}

static void draw_bar(int y, const char *label, int level, int sel)
{
	int i;

	y = SYS_Y(y);
	font_draw(LAYER_TEXT, SYS_X(60), y, label, sel ? 255 : 128, sel ? 220 : 128, sel ? 40 : 128);
	for (i = 0; i < 10; i++)
		video_rect(LAYER_TEXT, SYS_X(150 + i * 11), y + SYS_Y(3), SYS_X(8), SYS_Y(9), i < level ? 220 : 50,
		           i < level ? 200 : 50, i < level ? 60 : 50, 0);
}

static void draw_options(void)
{
#ifdef VN_NATIVE_SNOOT_MENU
	snoot_psp_options_draw(music_level, sfx_level, text_speed_cps, auto_forward_seconds, ui_cursor,
#ifdef VN_VITA_NATIVE_MENU
	                       pref_transitions, pref_skip_unseen, pref_skip_after_choices, pref_chapter_select,
	                       ui_back != ST_TITLE);
#else
	                       2, 0, 1, 0, ui_back != ST_TITLE);
#endif
	return;
#endif
	panel();
	draw_center(SYS_Y(40), TX(T_OPTIONS), 255, 220, 40);
	draw_bar(80, TX(T_MUSIC), music_level, ui_cursor == 0);
	draw_bar(105, TX(T_SFX), sfx_level, ui_cursor == 1);
	draw_center(SYS_Y(140), TX(T_BACK), ui_cursor == 2 ? 255 : 128, ui_cursor == 2 ? 220 : 128,
	            ui_cursor == 2 ? 40 : 128);
	draw_center(SCREEN_H - SYS_Y(30), TX(T_OPTIONS_HELP), 96, 96, 96);
}

static int gallery_seen_at(int k)
{
	return (gallery_seen[k >> 3] >> (k & 7)) & 1;
}

static int gallery_first_image(int k)
{
	int i;

	for (i = 0; i < gallery_pairs; i++)
		if (gallery_index[i] == k)
			return gallery_image[i];
	return 0;
}

/* Indice del video (VID1) de la entrada k de la galeria, o -1 */
static int video_at(int k)
{
	int i;

	for (i = 0; i < video_count; i++)
		if (video_slot[i] == k)
			return i;
	return -1;
}

static void update_gallery(void)
{
	int v;
#ifdef VN_NATIVE_SNOOT_MENU
	if (snoot_psp_gallery_update(gallery_seen, gallery_count)) {
		ui_cursor = 3;
		state = ST_TITLE;
	}
	return;
#endif

	if (input_pressed(BTN_LEFT))
		gallery_pos = (gallery_pos + gallery_count - 1) % gallery_count;
	if (input_pressed(BTN_RIGHT))
		gallery_pos = (gallery_pos + 1) % gallery_count;
	v = video_at(gallery_pos);
	if (v >= 0 && gallery_seen_at(gallery_pos) && input_pressed(BTN_CROSS)) {
		fmv_select(video_file[v]);
		special_return = ST_GALLERY;
		begin_special(SPECIAL_VIDEO);
		return;
	}
	if (input_pressed(BTN_CIRCLE)) {
		ui_cursor = 3;
		state = ST_TITLE;
	}
}

static void draw_gallery(void)
{
	char line[24];
	int image = gallery_first_image(gallery_pos);
#ifdef VN_NATIVE_SNOOT_MENU
	snoot_psp_gallery_draw(gallery_seen, gallery_count);
	return;
#endif

	if (gallery_seen_at(gallery_pos) && image) {
		const Image *img;

		res_new_scene();
		img = res_get(image);
		if (img && img->kind <= 1) {
			int x = (SCREEN_W - img->full_w) / 2;
			int y = VIEW_Y + (img->kind == 0 ? (VIEW_H - img->full_h) / 2 : VIEW_H - img->full_h);

			if (img->big)
				img_draw_big(img, LAYER_SPRITE0, x, y);
			else
				img_draw(img, LAYER_SPRITE0, x, y);
		}
	} else if (video_at(gallery_pos) >= 0 && gallery_seen_at(gallery_pos)) {
		video_rect(LAYER_SPRITE0, SYS_X(60), SYS_Y(70), SYS_X(200), SYS_Y(100), 30, 30, 30, 0);
		draw_center(SYS_Y(100), "Video", 200, 200, 200);
		draw_center(SYS_Y(120), "X para ver", 128, 128, 128);
	} else {
		video_rect(LAYER_SPRITE0, SYS_X(60), SYS_Y(70), SYS_X(200), SYS_Y(100), 30, 30, 30, 0);
		draw_center(SYS_Y(110), "???", 96, 96, 96);
	}
	video_clip_scene();
	sprintf(line, "%d / %d", gallery_pos + 1, gallery_count);
	draw_center(SCREEN_H - SYS_Y(24), line, 128, 128, 128);
	font_draw(LAYER_TEXT, SYS_X(8), SCREEN_H - SYS_Y(24), "< >", 96, 96, 96);
	font_draw(LAYER_TEXT, SCREEN_W - SYS_X(70), SCREEN_H - SYS_Y(24), TX(T_CIRCLE_BACK), 96, 96, 96);
}

static void update_extras(void)
{
	int list[MAX_EXTRAS], n = extras_list(list), r = list_input(&ui_cursor, n);
#ifdef VN_VITA_NATIVE_MENU
	int row;
	if (touch_list_row(SYS_Y(50), font_line_height() + font_line_height() * 2 / 7,
	                   font_line_height() + 2, n, SYS_X(20), SCREEN_W - SYS_X(20), &row)) {
		ui_cursor = row;
		r = 1;
	}
#endif

	if (r < 0) {
		ui_cursor = 2;
		state = ST_TITLE;
		return;
	}
	if (r != 1)
		return;
	dev_session = 0;
	begin(extra_chapter[list[ui_cursor]], extra_offset[list[ui_cursor]], 255, 0);
	playing_extra = 1;
}

static void draw_extras(void)
{
	int list[MAX_EXTRAS], n = extras_list(list), i;
	const char *items[MAX_EXTRAS];

	for (i = 0; i < n; i++)
		items[i] = extra_name[list[i]];
	draw_system_panel();
	draw_center(SYS_Y(24), TX(T_EXTRAS_TITLE), 255, 220, 40);
	draw_list(50, items, n, ui_cursor, 0);
}

/* SPECIAL id del guion: abre el modulo que la tabla del juego asigna a ese id (games/<juego>/specials.c) */
static void begin_special(int id)
{
	int i;

	skip_mode = 0; /* como en Ren'Py: una pantalla interactiva detiene el salto */
	for (i = 0; i < game_special_count; i++)
		if (game_specials[i].id == id && game_specials[i].special->begin()) {
			special = game_specials[i].special;
			state = (State)(ST_SPECIAL + i);
			return;
		}
}

/* Fin de una partida: se guarda el progreso (texto leido, galeria, finales) y se vuelve al titulo */
static void finish_game(void)
{
	if (!dev_session)
		write_card(TX(T_SAVING_PROGRESS));
	go_title();
}

/* Prueba de recorrido (AUTOTEST con autoplay): avanza solo, sin saltar, para que se cargue todo como al jugar. Las
 * decisiones siguen las del guion (una ruta) y, si no hay, van rotando para pasar por ramas distintas; al volver al
 * titulo o ante un error pide cerrar el emulador. */
static int autoplay_started, autoplay_menus;

static void autoplay_step(void)
{
	switch (state) {
	case ST_SAY:
		if (res_prefetch_busy() || audio_loading() || !say_prefetched)
			break; /* como un jugador: lee la linea mientras la precarga sigue */
		text_reveal_page(&run);
		if (run.at_page_end) {
			text_next_page(&run);
		} else {
			seen_set(vm.ev_gid);
			state = ST_RUN;
		}
		autoplay_started = 1;
		break;
	/* pausas y transiciones: corren su tiempo solo si hay una carga en segundo plano (como al jugar) */
	case ST_PAUSE:
		if (!res_prefetch_busy() && !audio_loading())
			state = ST_RUN;
		break;
	case ST_MENU: {
		/* la ruta del guion manda (indice de la opcion en la VM; fuera de rango la VM avisa el error); sin ruta, rota */
		int c = input_autotest_choice();

		vm.choice = c >= 0 ? c : menu_order[autoplay_menus++ % vm.menu_n];
		state = ST_RUN;
		break;
	}
	case ST_TRANS:
		if (!res_prefetch_busy() && !audio_loading())
			end_transition();
		break;
	case ST_NOTICE:
		state = notice_next;
		break;
	case ST_ERROR:
		autotest_quit = 1;
		break;
	case ST_TITLE:
		if (autoplay_started)
			autotest_quit = 1;
		break;
	default:
		/* pantallas especiales: cada modulo decide cuando seguir (el teclado escribe un ejemplo) */
		if (IS_SPECIAL(state) && (!special->autoplay || special->autoplay()))
			state = ST_RUN;
		break;
	}
}

/* Pasa varias lineas ya leidas y pausas por cuadro; se detiene en la primera linea no leida. */
static void skip_step(void)
{
	int n;

	for (n = 0; skip_mode && n < SKIP_PER_FRAME; n++) {
		if (state == ST_SAY) {
			if (!say_seen_before && !dev_session
#ifdef VN_VITA_NATIVE_MENU
			    && !pref_skip_unseen
#endif
			) {
				skip_mode = 0;
				break;
			}
			seen_set(vm.ev_gid);
			state = ST_RUN;
		} else if (state == ST_PAUSE) {
			state = ST_RUN;
		} else if (state != ST_RUN) {
			break; /* decision, final o error: espera al jugador */
		}
		run_vm();
	}
	if (skip_mode && state == ST_SAY)
		text_reveal_page(&run); /* la linea que queda a la vista sale completa, sin maquina de escribir */
}

static void update_say(void)
{
	int auto_advance = 0;
	int advance;
	int touch_x, touch_y;
	int touched = input_touch(&touch_x, &touch_y);

	if (auto_forward_seconds && text_page_done(&run) && text_finished(&run)) {
		if (say_auto_elapsed_frames < (uint32_t)auto_forward_seconds * 60)
			say_auto_elapsed_frames++;
		auto_advance = say_auto_elapsed_frames >= (uint32_t)auto_forward_seconds * 60;
	} else {
		say_auto_elapsed_frames = 0;
	}
	advance = (input_pressed(BTN_CROSS) && !eat_cross) || touched ||
	          (text_page_done(&run) && run.auto_next && text_finished(&run)) || auto_advance;

	if (!say_prefetched && !res_prefetch_busy()) {
		say_prefetched = 1; /* una vez por linea, cuando el CD quedo libre */
		prefetch_next();
	}
	if (show_box && box_level < BOX_FADE)
		return; /* como en Ren'Py, el texto empieza cuando la caja termino de aparecer */
	if (advance) {
		if (!text_page_done(&run)) {
			text_reveal_page(&run);
		} else if (run.at_page_end) {
			text_next_page(&run);
		} else {
			seen_set(vm.ev_gid);
			state = ST_RUN;
			return;
		}
	}
	text_update(&run);
}

static void update_imgmenu(void)
{
	int tx, ty;
	int touched = input_touch(&tx, &ty);
	int i, hit = 0;
	if ((input_pressed(BTN_LEFT) || input_pressed(BTN_UP)) && menu_cursor > 0)
		menu_cursor--;
	if ((input_pressed(BTN_RIGHT) || input_pressed(BTN_DOWN)) && menu_cursor < vm.menu_n - 1)
		menu_cursor++;
	if (touched)
		for (i = 0; i < vm.menu_n; i++) {
			const Image *img = scene_image(vm.menu_img_id[i]);
			if (img && tx >= vm.menu_x[i] && tx < vm.menu_x[i] + img->full_w &&
			    ty >= VIEW_Y + vm.menu_y[i] && ty < VIEW_Y + vm.menu_y[i] + img->full_h) {
				menu_cursor = i;
				hit = 1;
				break;
			}
		}
	if (input_pressed(BTN_CROSS) || hit) {
		vm.choice = menu_cursor;
#ifdef VN_VITA_NATIVE_MENU
		if (!pref_skip_after_choices)
			skip_mode = 0;
#endif
		state = ST_RUN;
	}
}

static void update_menu(void)
{
	int tx, ty, touched = input_touch(&tx, &ty), row;
	if (vm.menu_img) {
		update_imgmenu();
		return;
	}
	if (input_pressed(BTN_UP) && menu_cursor > 0)
		menu_cursor--;
	if (input_pressed(BTN_DOWN) && menu_cursor < vm.menu_n - 1)
		menu_cursor++;
	if (touched && snoot_psp_choice_hit(tx, ty, vm.menu_n, menu_cursor, &row)) {
		menu_cursor = row;
		vm.choice = menu_order[menu_cursor];
#ifdef VN_VITA_NATIVE_MENU
		if (!pref_skip_after_choices) skip_mode = 0;
#endif
		state = ST_RUN;
		return;
	}
	if (input_pressed(BTN_CROSS)) {
		vm.choice = menu_order[menu_cursor];
#ifdef VN_VITA_NATIVE_MENU
		if (!pref_skip_after_choices)
			skip_mode = 0;
#endif
		state = ST_RUN;
	}
}

static void draw_debug(int fps)
{
	char buf[64];

	sprintf(buf, "FPS %d  CAP %d  PC %d  TXT %d", fps, vm.chapter, (int)vm.pc, vm.ev_gid);
	font_draw(LAYER_DEBUG, 4, 2, buf, 255, 220, 40);
}

/* SHOW con VM_SHOW_FROM_CURRENT: el aspecto que se esta dibujando; al saltar todo esta asentado y no se captura */
static int capture(void *user, const Vm *v, const VmSprite *s, VmLook *out)
{
	(void)user;
	return !skip_mode && scene_capture(v, s, (int)(v->now - s->shown_at), out);
}

int main(int argc, const char *argv[])
{
	static const VmHost host = {0, chapter_loader, notify, capture};
	int vb_start, last_vb, frames_window = 0, fps = 0;

	video_init();
	cd_init();
	audio_init();
	vn_platform_set(
	    vn_platform_default_ops()); /* el nucleo (chapter, text) lee archivos y dibuja la fuente por aqui */
	{
		/* disco sin memory card (build_game.py --nocard): no se instala su controlador (prueba en POPStarter) */
		int lba, size;

		card_disabled = cd_find("\\DATA\\NOCARD.BIN", &lba, &size) == 0;
	}
	input_init(!card_disabled);
	{
		/* la fuente pasa por el heap antes del primer capitulo; font_load la copia a la VRAM y se suelta */
		int lba, size, bytes = 0;
		uint8_t *font = 0;

		if (cd_find("\\DATA\\FONT.BIN", &lba, &size) == 0) {
			bytes = (size + 2047) / 2048 * 2048;
			font = malloc(bytes);
		}
		if (!font || cd_read_file("\\DATA\\FONT.BIN", font, bytes) != size || font_load(font, size) != 0)
			fatal("FONT.BIN");
		free(font);
	}
	load_game_tables();
	scene_set_positions(pos_x, pos_y, pos_count);
	vm_init(&vm, &host);
	if (cd_read_file("\\DATA\\MCICON.BIN", file_buf, sizeof(file_buf)) >= 160)
		save_set_icon(file_buf);
	{
		/* prueba automatica: solo existe en los discos de prueba (tools/engine/emutest.py) */
		static uint8_t autotest[16384] __attribute__((aligned(4)));
		int lba, size;

		if (cd_find("\\DATA\\AUTOTEST.BIN", &lba, &size) == 0 && size <= (int)sizeof(autotest) &&
		    cd_read_file("\\DATA\\AUTOTEST.BIN", autotest, sizeof(autotest)) > 2)
			input_set_script(autotest + 2, autotest[0] | (autotest[1] << 8));
	}
	{
		/* creditos mientras se lee la memory card; con Select presionado no se lee */
		int i;

		for (i = 0; i < 2; i++) {
			draw_center(SYS_Y(90), TX(T_BOOT_TITLE), 255, 220, 40);
			draw_center(SYS_Y(108), TX(T_BOOT_AUTHOR), 128, 128, 128);
			draw_center(SYS_Y(140), TX(T_BOOT_PORT), 96, 96, 96);
			video_flip();
		}
		if (input_raw_down(BTN_SELECT))
			card_disabled = 1;
		card_state = card_disabled ? SAVE_NO_CARD : save_read(save_buf);
		{
			int t0 = video_vblank_count();
			if (card_state == SAVE_OK)
				progress_from_buf();
			else
				memset(save_buf, 0, sizeof(save_buf));
			while (video_vblank_count() - t0 < 150) {
				draw_center(SYS_Y(90), TX(T_BOOT_TITLE), 255, 220, 40);
				draw_center(SYS_Y(108), TX(T_BOOT_AUTHOR), 128, 128, 128);
				draw_center(SYS_Y(140), TX(T_BOOT_PORT), 96, 96, 96);
				video_flip();
			}
		}
	}
	go_title();
	vb_start = last_vb = video_vblank_count();

	for (;;) {
		int vb, in_game;
#if defined(VN_TRACE) || defined(VN_PSP_PROGRESS)
		static unsigned long hb_it;
#endif

#if defined(VN_TRACE) || defined(VN_PSP_PROGRESS)
		/* Latido del bucle principal, para los atascos que solo salen en consola. Distingue las dos familias sin
		 * tener que adivinar: si `it` sube y `cuadros` no, el freno esta dentro de la iteracion (el VM o el
		 * portero de algo); si no sube ninguno, el bucle esta esperando a algo que no vuelve (vblank del GE en
		 * el flip, o el hilo de audio). En PSP imprimir cuesta, por eso cada 300 cuadros; las etapas de cada cuadro
		 * las marca VN_ETAPA abajo, que es lo que dice donde muere realmente la iteracion. */
		if (!(++hb_it % (
#ifdef VN_TRACE
					300
#else
					30
#endif
					))) {
			printf("hb: it=%lu cuadros=%u estado=%d autoplay=%lu\n", hb_it, (unsigned)video_frames(), state,
			       (unsigned long)autotest_autoplay);
			fflush(stdout);
		}
#endif

		input_update();
		VN_ETAPA(1);
		audio_update();
		VN_ETAPA(2);
		if (autotest_autoplay)
			autoplay_step();
		VN_ETAPA(4);
		in_game = state <= ST_ERROR || state == ST_TRANS || SPECIAL_HAS(SP_IN_GAME);
		if (in_game)
			anim_sounds();
		if (in_game && input_pressed(BTN_SELECT))
			debug = !debug;
		if (in_game && input_pressed(BTN_SQUARE))
			show_box = !show_box;
		/* Start abre la pausa donde se puede guardar: en una linea o en una decision */
		if ((state == ST_SAY || state == ST_MENU) && input_pressed(BTN_START)) {
			pause_from = state;
			pause_opened = video_frames();
			skip_mode = 0;
			ui_cursor = 0;
			state = ST_PMENU;
		}
		/* R1 activa o detiene el salto; X tambien lo detiene (fuera de una decision) sin avanzar la linea */
		eat_cross = 0;
		if (!in_game) {
			/* en los menus no se salta */
		} else if (input_pressed(BTN_R1) && !SPECIAL_HAS(SP_OWN_R1)) { /* en el chat R1 solo acelera */
			skip_mode = !skip_mode;
		} else if (skip_mode && state != ST_MENU && input_pressed(BTN_CROSS)) {
			skip_mode = 0;
			eat_cross = 1;
		}
		if (in_game)
			play_frames++;
		if (autosave_pending && state == ST_SAY && !dev_session) {
			autosave_pending = 0;
			write_card(TX(T_SAVING_PROGRESS));
		}

		VN_ETAPA(8); /* maquina de estados: run_vm, skip_step, scene_sync y el update del estado actual */
		if (state == ST_RUN)
			run_vm();
		skip_step();
		scene_sync();
		switch (state) {
		case ST_SAY:
			update_say();
			break;
		case ST_MENU:
			update_menu();
			break;
		case ST_PAUSE:
			if (--pause_left <= 0 || (vm.ev_flags & 1 && input_pressed(BTN_CROSS) && !eat_cross))
				state = ST_RUN;
			break;
		case ST_END:
			finish_game();
			break;
		case ST_ERROR:
			if (input_pressed(BTN_CROSS))
				go_title();
			break;
		case ST_DEV:
			update_dev();
			break;
		case ST_TRANS:
			update_transition();
			break;
		case ST_TITLE:
			update_title();
			break;
		case ST_PMENU:
			update_pause_menu();
			break;
		case ST_SLOTS:
			update_slots();
			break;
		case ST_OPTIONS:
			update_options();
			break;
		case ST_GALLERY:
			update_gallery();
			break;
		case ST_EXTRAS:
			update_extras();
			break;
		case ST_CONFIRM:
			update_confirm();
			break;
		case ST_NOTICE:
			{
				int tx, ty;
				if (input_pressed(BTN_CROSS) || input_pressed(BTN_CIRCLE) || input_touch(&tx, &ty))
				state = notice_next;
			}
			break;
		default:
			if (IS_SPECIAL(state) && special->update()) {
				state = special_return;
				special_return = ST_RUN;
			}
			break;
		}
		if (state <= ST_ERROR || state == ST_TRANS)
			update_box(); /* en la pausa y los menus la caja queda como estaba */
		res_tick();

#ifdef VN_NATIVE_SNOOT_MENU
		/* La página del menú no ocupa memoria durante la historia. */
		if (state != ST_TITLE && state != ST_GALLERY && state != ST_OPTIONS)
			snoot_psp_menu_release();
#endif

		transition_offset();
		img_brightness = transition_brightness();
		if (state != ST_GALLERY && !SPECIAL_HAS(SP_OWN_SCENE))
			draw_scene();
		img_brightness = 256;
		draw_transition();
		switch (state) {
		case ST_DEV:
			draw_dev();
			break;
		case ST_SAY:
			if (box_level > 0)
				draw_box(1);
			break;
		case ST_RUN:
		case ST_PAUSE:
		case ST_TRANS:
			if (box_level > 0)
				draw_box(0);
			break;
		case ST_MENU:
			draw_menu();
			break;
		case ST_ERROR:
			font_draw(LAYER_TEXT, 8, BOX_Y + 8, vm.error ? vm.error : "error", 255, 80, 80);
			break;
		case ST_TITLE:
			draw_title();
			break;
		case ST_PMENU:
			draw_system_panel();
			draw_center(SYS_Y(40), TX(T_PAUSE), 255, 220, 40);
			draw_list(70, pause_items, 6, ui_cursor, 0);
			break;
		case ST_SLOTS:
			draw_slots();
			break;
		case ST_OPTIONS:
			draw_options();
			break;
		case ST_GALLERY:
			draw_gallery();
			break;
		case ST_EXTRAS:
			draw_extras();
			break;
		case ST_CONFIRM:
			draw_confirm();
			break;
		case ST_NOTICE:
			draw_system_panel();
			draw_center(SYS_Y(92), notice_text, 128, 128, 128);
			draw_center(SYS_Y(140), TX(T_CROSS_CONTINUE), 96, 96, 96);
			break;
		default:
			if (IS_SPECIAL(state))
				special->draw();
			break;
		}
		video_unclip();
		VN_ETAPA(16); /* dibujado de la capa hud/debug y contabilidad de vblank */

		vb = video_vblank_count();
		if (in_game && vb - last_vb > STALL_VBLANKS)
			log_stall(vb - last_vb);
		last_vb = vb;
		frames_window++;
		if (vb - vb_start >= 60) {
			fps = (frames_window * 60) / (vb - vb_start);
			frames_window = 0;
			vb_start = vb;
		}
		if (debug)
			draw_debug(fps);
		if (skip_mode)
			font_draw(LAYER_DEBUG, SCREEN_W - 84, 12, TX(T_SKIPPING), 255, 220, 40);
		VN_ETAPA(32);
		video_flip();
	}
	return 0;
}
