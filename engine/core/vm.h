/*
 * Maquina virtual del bytecode VNB (docs/bytecode.md). Codigo portable: no usa nada de la PS1, asi que tambien se
 * compila en la PC para compararlo contra el simulador de Python (tests/test_vm_host.py).
 *
 * Uso: vm_init; luego vm_run en bucle. vm_run ejecuta hasta un evento que necesita al anfitrion (texto, menu, pausa,
 * transicion, fin). Las demas instrucciones se aplican a la escena y se avisan por host->notify.
 */
#pragma once

#include <stdint.h>

#define VM_MAX_SPRITES 16
#define VM_STACK 16
#define VM_CALLS 8
#define VM_VARS 64
#define VM_MENU_MAX 16
#define VM_PHONE_MAX 32
#define VM_Z_KEEP (-128)

typedef struct {
	const uint8_t *code;        /* inicio del codigo (el .VNB cargado + 32) */
	uint32_t code_size;
	uint32_t text_base;         /* id global del primer texto del capitulo */
	uint16_t text_count;
	uint16_t anim_count;
	const uint8_t *anims;       /* tabla ANIM o NULL */
} VmChapter;

#define VM_SPRITE_RANDOM 0x04       /* flag de SHOW_RANDOM: aparece y se desvanece en (rx, ry) como randPosition */
#define VM_SHOW_FROM_CURRENT 0x08   /* flag de SHOW: la animacion nueva parte del aspecto actual, no de su clave 0 */

/* Aspecto capturado de un sprite (VM_SHOW_FROM_CURRENT): sustituye la clave 0 hasta la clave 1 del primer pase */
typedef struct {
	int16_t xa, ya, angle;
	uint8_t alpha, zoom, r, g, b;
	uint8_t on;
} VmLook;

typedef struct {
	uint8_t used, tag, pos, flags;
	int8_t z;
	int16_t depth;             /* profundidad de perspectiva (OP_DEPTH); no es el orden de dibujo (z) */
	uint8_t zzoom;             /* 1: la capa conserva su tamano en perspectiva (zzoom de Ren'Py) */
	uint16_t image, anim;
	uint32_t born;              /* orden de creacion (desempata el z) */
	uint32_t shown_at;          /* vm->now cuando empezo su animacion */
	int16_t rx, ry;             /* alineacion 8.8 de los sprites de SHOW_RANDOM */
	int32_t sound_t;            /* ultimo cuadro de la animacion cuyos sonidos ya sonaron */
} VmSprite;

typedef enum {
	VM_EV_NONE = 0,
	VM_EV_SAY,          /* vm->ev_speaker, ev_text, ev_gid, ev_extend */
	VM_EV_MENU,         /* vm->menu_*; el anfitrion pone vm->choice y vuelve a llamar vm_run */
	VM_EV_PAUSE,        /* vm->ev_frames, ev_flags */
	VM_EV_TRANSITION,   /* vm->ev_args[0..3] */
	VM_EV_SPECIAL,      /* vm->ev_args[0] */
	VM_EV_END,
	VM_EV_ERROR
} VmEvent;

typedef struct Vm Vm;

typedef struct {
	void *user;
	/* Carga un capitulo (blocking). Devuelve 0 si salio bien. */
	int (*load_chapter)(void *user, int chapter, VmChapter *out);
	/* Aviso de una instruccion ya aplicada: opcode y sus campos en orden. */
	void (*notify)(void *user, int opcode, const int32_t *args, int nargs);
	/* Aspecto actual de `s` antes de que un SHOW con VM_SHOW_FROM_CURRENT lo reemplace. Devuelve 0 para no capturar
	 * (al saltar); NULL: la animacion nueva parte de su clave 0, como sin el flag. */
	int (*capture)(void *user, const Vm *vm, const VmSprite *s, VmLook *out);
} VmHost;

struct Vm {
	const VmHost *host;
	VmChapter ch;
	int chapter;
	uint32_t pc;
	int16_t vars[VM_VARS];
	int16_t stack[VM_STACK];
	int sp;
	struct { int chapter; uint32_t pc; } calls[VM_CALLS];
	int ncalls;

	/* escena */
	uint16_t bg, bg_anim;
	VmSprite sprites[VM_MAX_SPRITES];
	VmLook from[VM_MAX_SPRITES];        /* aspecto de partida por sprite; todo SHOW que crea o reinicia lo fija (no va en el guardado) */
	int persp_on, persp_z11, persp_zc;   /* OP_PERSPECTIVE: camara con perspectiva (z11 por defecto 1000) */
	int depth_tag, depth_z, depth_zzoom; /* OP_DEPTH: profundidad y zzoom para el SHOW del tag que sigue */
	uint32_t born_counter;

	/* datos del evento actual */
	int ev_speaker, ev_text, ev_gid, ev_extend;
	uint32_t ev_pc;             /* inicio de la instruccion SAY o MENU del evento (donde apunta un guardado) */
	int ev_frames, ev_flags;
	int32_t ev_args[4];
	int menu_n, menu_flags;
	int menu_text[VM_MENU_MAX];
	uint32_t menu_target[VM_MENU_MAX];
	int menu_img;               /* 1 si es un menu de imagenes (OP_IMGMENU): usa los campos de abajo */
	uint16_t menu_bg;           /* imagen de fondo del menu de imagenes */
	uint16_t menu_img_id[VM_MENU_MAX], menu_seen[VM_MENU_MAX];   /* seen 0xFFFF: nunca se apaga */
	int16_t menu_x[VM_MENU_MAX], menu_y[VM_MENU_MAX];           /* pixeles de la vista (320x180) */
	int phone_n;                /* mensajes del chat de telefono (OP_PHONE), hasta VM_PHONE_MAX */
	uint8_t phone_side[VM_PHONE_MAX];
	uint16_t phone_text[VM_PHONE_MAX], phone_img[VM_PHONE_MAX];
	int choice;                 /* lo pone el anfitrion al resolver un menu */
	int in_menu;
	const char *error;
	uint32_t now;               /* reloj del anfitrion en cuadros (inicio de las animaciones) */
	uint32_t rng;               /* estado del azar de SHOW_RANDOM */
};

void vm_init(Vm *vm, const VmHost *host);
/* Empieza a ejecutar el capitulo `chapter` en el offset `entry`. Devuelve 0 si salio bien. */
int vm_start(Vm *vm, int chapter, uint32_t entry);
VmEvent vm_run(Vm *vm);
/* Sprite por tag (NULL si no existe) */
VmSprite *vm_sprite(Vm *vm, int tag);
