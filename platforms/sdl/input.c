#include "input.h"
#include "sdl_target.h"
#include "video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__PSP__)
#include <pspctrl.h>
static SceCtrlData psp_pad;
#endif

volatile uint32_t autotest_autoplay, autotest_quit;
static volatile uint32_t autotest_shot;
static const uint8_t *script;
static int count, choice_at, frame;
static uint8_t done[2800];
static uint16_t cur, prev;
static int shot_pending;
static int touch_x, touch_y, touch_hit;
static int finger_down;
#if SDL_TARGET_HAS_JOYSTICK
static SDL_Joystick *joystick;
#elif SDL_TARGET_HAS_GAME_CONTROLLER
static SDL_GameController *controller;
#endif
extern void sdl_shot_request(void);

/* Tactil (Vita, 3DS, Switch): SDL_FINGERDOWN da la posicion normalizada a la ventana. Se convierte a
 * coordenadas del guion (320x240) con el mapeo inverso del que usa video.c para dibujar: en las consolas
 * con escala manual (PSP, Vita, PS3, PS4) la vista es VIEW_X/Y/W/H; en las de tamano logico (PC, 3DS, Wii,
 * Xbox, Switch, PS2) SDL encaja 320x240 en la ventana manteniendo el aspecto. */
static void touch_map(float fx, float fy)
{
	int wx = (int)(fx * SDL_TARGET_WINDOW_W);
	int wy = (int)(fy * SDL_TARGET_WINDOW_H);

#if SDL_TARGET_MANUAL_SCALE
	touch_x = (wx - SDL_TARGET_VIEW_X) * SCREEN_W / SDL_TARGET_VIEW_W;
	touch_y = (wy - SDL_TARGET_VIEW_Y) * SCREEN_H / SDL_TARGET_VIEW_H;
#else
	float sx = (float)SDL_TARGET_WINDOW_W / SCREEN_W;
	float sy = (float)SDL_TARGET_WINDOW_H / SCREEN_H;
	float s = sx < sy ? sx : sy;

	touch_x = (int)((wx - (SDL_TARGET_WINDOW_W - SCREEN_W * s) / 2) / s);
	touch_y = (int)((wy - (SDL_TARGET_WINDOW_H - SCREEN_H * s) / 2) / s);
#endif
}

static void touch_mouse(int x, int y)
{
	touch_map((float)x / SDL_TARGET_WINDOW_W, (float)y / SDL_TARGET_WINDOW_H);
	touch_hit = 1;
}

void input_init(int card)
{
	(void)card;
#if defined(__PSP__)
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
#endif
#if SDL_TARGET_HAS_JOYSTICK
	if (SDL_NumJoysticks() > 0)
		joystick = SDL_JoystickOpen(0);
#elif SDL_TARGET_HAS_GAME_CONTROLLER
	if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0))
		controller = SDL_GameControllerOpen(0);
#endif
}
void input_set_script(const uint8_t *p, int n)
{
	script = p;
	count = n;
	choice_at = 0;
	frame = 0;
	memset(done, 0, sizeof(done));
}
void input_update(void)
{
	SDL_Event e;
	const Uint8 *k;
	int i;
	prev = cur;
	cur = 0;
#if defined(__PSP__)
	if (sceCtrlPeekBufferPositive(&psp_pad, 1) > 0) {
		cur = (uint16_t)(psp_pad.Buttons & (PSP_CTRL_SELECT | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_RIGHT |
		                                    PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_TRIANGLE |
		                                    PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE));
		if (psp_pad.Buttons & PSP_CTRL_LTRIGGER)
			cur |= BTN_L1;
		if (psp_pad.Buttons & PSP_CTRL_RTRIGGER)
			cur |= BTN_R1;
		if (psp_pad.Lx < 80)
			cur |= BTN_LEFT;
		if (psp_pad.Lx > 176)
			cur |= BTN_RIGHT;
		if (psp_pad.Ly < 80)
			cur |= BTN_UP;
		if (psp_pad.Ly > 176)
			cur |= BTN_DOWN;
#ifdef VN_TRACE
		if (cur != prev)
			printf("input: PSP mask=%04x stick=%d,%d\n", cur, psp_pad.Lx, psp_pad.Ly);
#endif
	}
#endif
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT)
			autotest_quit = 1;
		if (e.type == SDL_FINGERDOWN) {
			finger_down = 1;
			touch_map(e.tfinger.x, e.tfinger.y);
			touch_hit = 1;
		} else if (e.type == SDL_FINGERMOTION && finger_down) {
			touch_mouse((int)(e.tfinger.x * SDL_TARGET_WINDOW_W), (int)(e.tfinger.y * SDL_TARGET_WINDOW_H));
			touch_hit = 0;
		} else if (e.type == SDL_FINGERUP) {
			finger_down = 0;
		} else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
			touch_mouse(e.button.x, e.button.y);
		}
	}
	k = SDL_GetKeyboardState(NULL);
#define KEY(key, bit)                                                                                                  \
	if (k[SDL_GetScancodeFromKey(key)])                                                                            \
	cur |= (bit)
	KEY(SDLK_ESCAPE, BTN_CIRCLE);
	KEY(SDLK_RETURN, BTN_CROSS);
	KEY(SDLK_SPACE, BTN_CROSS);
	KEY(SDLK_BACKSPACE, BTN_SQUARE);
	KEY(SDLK_TAB, BTN_TRIANGLE);
	KEY(SDLK_UP, BTN_UP);
	KEY(SDLK_w, BTN_UP);
	KEY(SDLK_RIGHT, BTN_RIGHT);
	KEY(SDLK_d, BTN_RIGHT);
	KEY(SDLK_DOWN, BTN_DOWN);
	KEY(SDLK_s, BTN_DOWN);
	KEY(SDLK_LEFT, BTN_LEFT);
	KEY(SDLK_a, BTN_LEFT);
	KEY(SDLK_q, BTN_L1);
	KEY(SDLK_e, BTN_R1);
	KEY(SDLK_LSHIFT, BTN_L2);
	KEY(SDLK_RSHIFT, BTN_R2);
	KEY(SDLK_F1, BTN_START);
	KEY(SDLK_F2, BTN_SELECT);
#undef KEY
#if SDL_TARGET_HAS_JOYSTICK
	if (joystick) {
		SDL_JoystickUpdate();
#define JOY(button, bit)                                                                                               \
	if (SDL_JoystickGetButton(joystick, button))                                                                   \
	cur |= (bit)
		JOY(0, BTN_CROSS);
		JOY(1, BTN_CIRCLE);
		JOY(2, BTN_SQUARE);
		JOY(3, BTN_TRIANGLE);
		JOY(4, BTN_L1);
		JOY(5, BTN_R1);
		JOY(9, BTN_START);
		JOY(13, BTN_UP);
		JOY(14, BTN_DOWN);
		JOY(15, BTN_LEFT);
		JOY(16, BTN_RIGHT);
		JOY(17, BTN_SELECT);
		JOY(18, BTN_L2);
		JOY(19, BTN_R2);
#undef JOY
		if (SDL_JoystickGetAxis(joystick, 0) < -16000)
			cur |= BTN_LEFT;
		if (SDL_JoystickGetAxis(joystick, 0) > 16000)
			cur |= BTN_RIGHT;
		if (SDL_JoystickGetAxis(joystick, 1) < -16000)
			cur |= BTN_UP;
		if (SDL_JoystickGetAxis(joystick, 1) > 16000)
			cur |= BTN_DOWN;
	}
#elif SDL_TARGET_HAS_GAME_CONTROLLER
	if (controller) {
		SDL_GameControllerUpdate();
#define PAD(button, bit)                                                                                               \
	if (SDL_GameControllerGetButton(controller, button))                                                           \
	cur |= (bit)
		PAD(SDL_CONTROLLER_BUTTON_BACK, BTN_SELECT);
		PAD(SDL_CONTROLLER_BUTTON_START, BTN_START);
		PAD(SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_UP);
		PAD(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_RIGHT);
		PAD(SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DOWN);
		PAD(SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_LEFT);
		PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L1);
		PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R1);
		PAD(SDL_CONTROLLER_BUTTON_Y, BTN_TRIANGLE);
		PAD(SDL_CONTROLLER_BUTTON_B, BTN_CIRCLE);
		PAD(SDL_CONTROLLER_BUTTON_A, BTN_CROSS);
		PAD(SDL_CONTROLLER_BUTTON_X, BTN_SQUARE);
#undef PAD
		if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
			cur |= BTN_L2;
		if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
			cur |= BTN_R2;
		/* el stick izquierdo mueve igual que la cruceta (teclado en pantalla, menus) */
		if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) < -16000)
			cur |= BTN_LEFT;
		if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) > 16000)
			cur |= BTN_RIGHT;
		if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) < -16000)
			cur |= BTN_UP;
		if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) > 16000)
			cur |= BTN_DOWN;
	}
#endif
	for (i = 0; i < count; i++) {
		const uint8_t *p = script + 6 * i;
		uint32_t at = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		uint16_t val = (uint16_t)(p[4] | (p[5] << 8));
		if (frame < (int)at || done[i])
			continue;
		done[i] = 1;
		if (val == 0xffff) {
			autotest_shot++;
			shot_pending = 1;
		} else if (val == 0xfffe)
			autotest_quit = 1;
		else if (val == 0xfffd)
			autotest_autoplay = 1;
		else if (val < 0xffe0)
			cur |= val;
	}
	frame++;
	if (shot_pending) {
		sdl_shot_request();
		shot_pending = 0;
	}
	if (autotest_quit) {
		SDL_TARGET_QUIT_MARKER();
		/* Higiene: la salida va por sceIoWrite al tty de la consola y puede quedar en el buffer; se vacia antes de
		 * salir para no perder lo que el motor ya dijo. (Medido en Vita el 2026-10-30: no explica el log mudo de
		 * T-643, porque ahi el proceso cae antes de llegar a este punto.) */
		fflush(stdout);
		exit(0);
	}
}
int input_down(uint16_t m)
{
	return (cur & m) != 0;
}
int input_pressed(uint16_t m)
{
	return (cur & ~prev & m) != 0;
}
int input_raw_down(uint16_t m)
{
	return input_down(m);
}
int input_touch(int *x, int *y)
{
	if (!touch_hit)
		return 0;
	touch_hit = 0;
	*x = touch_x;
	*y = touch_y;
	return 1;
}
int input_autotest_choice(void)
{
	for (; choice_at < count; choice_at++) {
		const uint8_t *p = script + 6 * choice_at;
		int v = p[4] | (p[5] << 8);
		if (v >= 0xffe0 && v < 0xfff0) {
			choice_at++;
			return v - 0xffe0;
		}
	}
	return -1;
}

int input_stick_x(void)
{
#if defined(__PSP__)
	int value = (int)psp_pad.Lx - 128;
	return value < -16 || value > 16 ? (value < -127 ? -127 : value) : 0;
#endif
#if SDL_TARGET_HAS_GAME_CONTROLLER
	if (controller) {
		SDL_GameControllerUpdate();
		int v = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
		if (v < -8000)
			return -127;
		if (v > 8000)
			return 127;
		if (v < -4000)
			return -64;
		if (v > 4000)
			return 64;
		if (v < -2000)
			return -32;
		if (v > 2000)
			return 32;
	}
#endif
#if SDL_TARGET_HAS_JOYSTICK
	if (joystick) {
		SDL_JoystickUpdate();
		int v = SDL_JoystickGetAxis(joystick, 0);
		if (v < -16000)
			return -127;
		if (v > 16000)
			return 127;
		if (v < -8000)
			return -64;
		if (v > 8000)
			return 64;
		if (v < -4000)
			return -32;
		if (v > 4000)
			return 32;
	}
#endif
	return 0;
}

int input_stick_y(void)
{
#if defined(__PSP__)
	int value = (int)psp_pad.Ly - 128;
	return value < -16 || value > 16 ? (value < -127 ? -127 : value) : 0;
#endif
#if SDL_TARGET_HAS_GAME_CONTROLLER
	if (controller) {
		SDL_GameControllerUpdate();
		int v = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
		if (v < -8000)
			return -127;
		if (v > 8000)
			return 127;
		if (v < -4000)
			return -64;
		if (v > 4000)
			return 64;
		if (v < -2000)
			return -32;
		if (v > 2000)
			return 32;
	}
#endif
#if SDL_TARGET_HAS_JOYSTICK
	if (joystick) {
		SDL_JoystickUpdate();
		int v = SDL_JoystickGetAxis(joystick, 1);
		if (v < -16000)
			return -127;
		if (v > 16000)
			return 127;
		if (v < -8000)
			return -64;
		if (v > 8000)
			return 64;
		if (v < -4000)
			return -32;
		if (v > 4000)
			return 32;
	}
#endif
	return 0;
}

int input_touch_down(int *x, int *y)
{
	int mx = 0, my = 0;
	Uint32 buttons = SDL_GetMouseState(&mx, &my);
	int down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
	if (finger_down) {
		if (x)
			*x = touch_x;
		if (y)
			*y = touch_y;
		return 1;
	}
	if (down)
		touch_mouse(mx, my);
	if (x)
		*x = touch_x;
	if (y)
		*y = touch_y;
	return down;
}
