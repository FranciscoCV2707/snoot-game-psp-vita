#include "psp_menu.h"

#ifdef VN_NATIVE_SNOOT_MENU
#include "img.h"
#include "text.h"
#include "input.h"
#include "res.h"
#include "audio_native.h"
#include "vn_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* API por enlace de los backends SDL; no pertenece al contrato del núcleo. */
extern void img_release(Image *image);
static Image page_image;
static char loaded_page[24];
static Image dialog[2];
static Image choice[2];
static Image gallery_cover;
static uint16_t gallery_images[256];
typedef struct {
	uint16_t item, first, count;
} GalleryAnimation;
static GalleryAnimation gallery_animations[16];
static uint16_t gallery_frame_lengths[512];
static int gallery_animations_count, gallery_frames_count;
static uint32_t gallery_movie_start;
static Image gallery_movie_image;
static int gallery_movie_item = -1, gallery_movie_frame = -1;
static struct {
	char name[32];
	int count;
	uint16_t index[256];
} gallery_tabs[5];
static int gallery_tabs_count, gallery_tab, gallery_cursor, gallery_view, gallery_zoom;
static int gallery_pan_x, gallery_pan_y;

static int menu_scale(void)
{
	int scale = SCREEN_W / 480;
	return scale > 0 ? scale : 1;
}

static int menu_px(int value)
{
	return value * menu_scale();
}

static int menu_image_limit(void)
{
	return SCREEN_W * SCREEN_H * 4 + 64;
}

static int load_image(const char *name, Image *image)
{
	int size = vn_ops->file_size(name), result;
	uint8_t *data;
	if (size <= 32 || size > menu_image_limit())
		return 0;
	data = malloc((size_t)size);
	if (!data)
		return 0;
	result = vn_ops->file_read(name, data, size);
	result = result == size ? img_load(image, data, size, 0, 0, 0) : -1;
	free(data);
	return result == 0;
}

void snoot_psp_menu_release(void)
{
	img_release(&page_image);
	memset(&page_image, 0, sizeof(page_image));
	loaded_page[0] = 0;
}

int snoot_psp_menu_draw(const char *page)
{
	if (strcmp(loaded_page, page) != 0) {
		char name[32];
		uint8_t *data;
		int size, result;

		snoot_psp_menu_release();
		snprintf(name, sizeof(name), "%s.IMG", page);
		size = vn_ops->file_size(name);
		if (size <= 32 || size > menu_image_limit())
			return 0;
		data = malloc((size_t)size);
		if (!data)
			return 0;
		result = vn_ops->file_read(name, data, size);
		if (result == size)
			result = img_load(&page_image, data, size, 0, 0, 0);
		else
			result = -1;
		free(data);
		if (result != 0)
			return 0;
		snprintf(loaded_page, sizeof(loaded_page), "%s", page);
		printf("menu: original es page=%s size=%dx%d\n", page, page_image.w, page_image.h);
	}
	img_draw(&page_image, LAYER_BOX, 0, 0);
	return 1;
}

void snoot_psp_choice_draw(const char *const *choices, int count, int cursor)
{
	int i, first, visible, row_height, spacing, total_height, top, max_rows;
	Image *idle = &choice[0], *hover = &choice[1];
	const char *files[] = {"CHOICE_IDLE.IMG", "CHOICE_HOVER.IMG"};

	for (i = 0; i < 2; i++)
		if (!choice[i].loaded)
			load_image(files[i], &choice[i]);
	if (!idle->loaded || !hover->loaded || count < 1)
		return;
	row_height = idle->full_h;
	spacing = SCREEN_H * 33 / 1080;
	max_rows = (SCREEN_H - SCREEN_H * 60 / 1080 + spacing) / (row_height + spacing);
	visible = count < max_rows ? count : max_rows;
	first = cursor >= visible ? cursor - visible + 1 : 0;
	total_height = visible * row_height + (visible - 1) * spacing;
	top = SCREEN_H * 405 / 1080 - total_height / 2;
	for (i = 0; i < visible; i++) {
		int selected = first + i == cursor;
		Image *button = selected ? hover : idle;
		int y = top + i * (row_height + spacing);
		int text_y = y + (row_height - font_line_height()) / 2;
		int text_x = (SCREEN_W - font_width(choices[first + i])) / 2;
		img_draw(button, LAYER_BOX, (SCREEN_W - button->full_w) / 2, y);
		font_draw(LAYER_TEXT, text_x, text_y, choices[first + i], selected ? 255 : 204, selected ? 255 : 204,
		          selected ? 255 : 204);
	}
}

int snoot_psp_choice_hit(int x, int y, int count, int cursor, int *row)
{
	int i, visible, spacing, top, first, max_rows, row_height;
	if (!choice[0].loaded) load_image("CHOICE_IDLE.IMG", &choice[0]);
	if (!choice[1].loaded) load_image("CHOICE_HOVER.IMG", &choice[1]);
	if (!choice[0].loaded || count < 1) return 0;
	row_height = choice[0].full_h;
	spacing = SCREEN_H * 33 / 1080;
	max_rows = (SCREEN_H - SCREEN_H * 60 / 1080 + spacing) / (row_height + spacing);
	visible = count < max_rows ? count : max_rows;
	first = cursor >= visible ? cursor - visible + 1 : 0;
	top = SCREEN_H * 405 / 1080 - (visible * row_height + (visible - 1) * spacing) / 2;
	if (x < (SCREEN_W - choice[0].full_w) / 2 || x >= (SCREEN_W + choice[0].full_w) / 2) return 0;
	for (i = 0; i < visible; i++) {
		int y0 = top + i * (row_height + spacing);
		if (y >= y0 && y < y0 + row_height) {
			*row = first + i;
			return 1;
		}
	}
	return 0;
}

void snoot_psp_options_draw(int music_level, int sfx_level, int text_cps, int auto_forward, int cursor,
                            int transitions, int skip_unseen, int skip_after_choices, int chapter_select,
                            int system_menu)
{
#ifdef VN_VITA_NATIVE_MENU
	int i, j;
	const int levels[] = {music_level, sfx_level};
	const char *labels[] = {"M\u00fasica", "Efectos", "Velocidad texto", "Avance autom\u00e1tico", "Transiciones",
	                        "Texto no visto", "Tras elegir", "Selector cap\u00edtulos", "Im\u00e1genes lascivas",
	                        "Abrir selector", "Volver"};
	const char *transition_values[] = {"Ninguna", "Reducidas", "Completas"};
	snoot_psp_menu_draw(system_menu ? "SYSTEM_MENU" : "OPTIONS");
	font_draw(LAYER_TEXT, menu_px(35), menu_px(10), "Opciones", 128, 128, 128);
	for (i = 0; i < 11; i++) {
		int y = menu_px(32 + i * 20);
		int selected = cursor == i;
		const char *value = NULL;
		font_draw(LAYER_TEXT, menu_px(35), y, labels[i], selected ? 0 : 128, 128, selected ? 1 : 128);
		if (i < 2) {
			for (j = 0; j < 10; j++)
				video_rect(LAYER_TEXT, menu_px(210 + j * 8), y + menu_px(6), menu_px(6), menu_px(8),
				           j < levels[i] ? 204 : 48, j < levels[i] ? 102 : 48, j < levels[i] ? 255 : 48, 0);
		} else if (i == 2) {
			static char speed[24];
			snprintf(speed, sizeof(speed), text_cps ? "%d cps" : "Instant\u00e1neo", text_cps);
			value = speed;
		} else if (i == 3) {
			static char delay[24];
			if (auto_forward)
				snprintf(delay, sizeof(delay), "%d s", auto_forward);
			else
				snprintf(delay, sizeof(delay), "No");
			value = delay;
		} else if (i == 4)
			value = transition_values[transitions >= 0 && transitions <= 2 ? transitions : 2];
		else if (i == 5)
			value = skip_unseen ? "S\u00ed" : "No";
		else if (i == 6)
			value = skip_after_choices ? "S\u00ed" : "No";
		else if (i == 7)
			value = chapter_select ? "Activado" : "Desactivado";
		else if (i == 8)
			value = "Activadas";
		if (value)
			font_draw(LAYER_TEXT, menu_px(210), y, value, selected ? 0 : 128, 128, selected ? 1 : 128);
	}
#else
	int i, j;
	const int levels[] = {music_level, sfx_level};
	const char *labels[] = {"Música", "Efectos", "Velocidad texto", "Avance auto", "Volver"};
	snoot_psp_menu_draw("OPTIONS");
	font_draw(LAYER_TEXT, menu_px(55), menu_px(34), "Opciones", 128, 128, 128);
	for (i = 0; i < 5; i++) {
		int y = menu_px(66 + i * 34);
		int selected = cursor == i;
		font_draw(LAYER_TEXT, menu_px(35), y, labels[i], selected ? 0 : 128, 128, selected ? 1 : 128);
		if (i < 2) {
			for (j = 0; j < 10; j++)
				video_rect(LAYER_TEXT, menu_px(190 + j * 10), y + menu_px(5), menu_px(7), menu_px(10),
				           j < levels[i] ? 204 : 48, j < levels[i] ? 102 : 48, j < levels[i] ? 255 : 48, 0);
		} else if (i == 2) {
			char value[24];
			snprintf(value, sizeof(value), text_cps ? "%d cps" : "Instantáneo", text_cps);
			font_draw(LAYER_TEXT, menu_px(200), y, value, selected ? 0 : 128, 128, selected ? 1 : 128);
		} else if (i == 3) {
			char value[24];
			if (auto_forward)
				snprintf(value, sizeof(value), "%d s", auto_forward);
			else
				snprintf(value, sizeof(value), "No");
			font_draw(LAYER_TEXT, menu_px(200), y, value, selected ? 0 : 128, 128, selected ? 1 : 128);
		}
	}
#endif
}

void snoot_psp_dialog_draw(const TextRun *run, const char *speaker, int r, int g, int b, int fade, int fade_max,
                           int with_text)
{
	int named = speaker[0] != 0;
	Image *box = &dialog[named];
	int brightness = 128 * fade / fade_max;
	if (!box->loaded)
		load_image(named ? "BOX_NAME.IMG" : "BOX_NONE.IMG", box);
	if (box->loaded)
		img_draw_ex(box, LAYER_BOX, (SCREEN_W - box->full_w) / 2, SCREEN_H - box->full_h, 0, 64, brightness,
		            brightness, brightness);
	if (!with_text)
		return;
	if (named) {
		font_draw(LAYER_TEXT, menu_px(99) - font_width(speaker) / 2, BOX_Y - menu_px(18), speaker, r * fade / fade_max,
		          g * fade / fade_max, b * fade / fade_max);
	}
	text_draw(run, LAYER_TEXT, menu_px(55), BOX_Y + menu_px(5), brightness, brightness, brightness);
	if (fade >= fade_max && text_page_done(run) && ((video_frames() / 20) & 1))
		video_triangle_down(LAYER_TEXT, menu_px(415), SCREEN_H - menu_px(17), menu_px(7), menu_px(5), 128, 128, 128);
}

void snoot_psp_gallery_open(void)
{
	uint8_t data[2048];
	int size, off = 8, t, i, variable_frame_lengths;
	gallery_tab = gallery_cursor = gallery_view = gallery_zoom = 0;
	if (gallery_tabs_count)
		return;
	size = vn_ops->file_read("GALLERY.BIN", data, sizeof(data));
	if (size < 8 || (memcmp(data, "SGP1", 4) && memcmp(data, "SGP2", 4)) || data[5] || data[4] > 5)
		return;
	variable_frame_lengths = memcmp(data, "SGP2", 4) == 0;
	for (t = 0; t < data[4]; t++) {
		int length, count;
		if (off >= size)
			return;
		length = data[off++];
		if (length >= 32 || off + length + 2 > size)
			return;
		memcpy(gallery_tabs[t].name, data + off, length);
		gallery_tabs[t].name[length] = 0;
		off += length;
		count = data[off] | (data[off + 1] << 8);
		off += 2;
		if (count < 1 || count > 256 || off + count * 2 > size)
			return;
		gallery_tabs[t].count = count;
		for (i = 0; i < count; i++, off += 2)
			gallery_tabs[t].index[i] = (uint16_t)(data[off] | (data[off + 1] << 8));
	}
	if (off + 6 > size || memcmp(data + off, "IMGS", 4))
		return;
	i = data[off + 4] | (data[off + 5] << 8);
	off += 6;
	if (i > 256 || off + i * 2 > size)
		return;
	for (t = 0; t < i; t++, off += 2)
		gallery_images[t] = (uint16_t)(data[off] | (data[off + 1] << 8));
	if (off + 6 > size || memcmp(data + off, "ANIM", 4))
		return;
	gallery_animations_count = data[off + 4] | (data[off + 5] << 8);
	off += 6;
	if (gallery_animations_count > 16)
		return;
	for (t = 0; t < gallery_animations_count; t++) {
		int count, j;
		if (off + 4 > size)
			return;
		gallery_animations[t].item = (uint16_t)(data[off] | (data[off + 1] << 8));
		count = data[off + 2] | (data[off + 3] << 8);
		off += 4;
		if (count > 256 || gallery_frames_count + count > 512 ||
		    off + (variable_frame_lengths ? count * 2 : 2) > size)
			return;
		gallery_animations[t].first = (uint16_t)gallery_frames_count;
		gallery_animations[t].count = (uint16_t)count;
		if (variable_frame_lengths) {
			for (j = 0; j < count; j++, off += 2) {
				gallery_frame_lengths[gallery_frames_count] = (uint16_t)(data[off] | (data[off + 1] << 8));
				gallery_frames_count++;
			}
		} else {
			uint16_t duration = (uint16_t)(data[off] | (data[off + 1] << 8));
			off += 2;
			for (j = 0; j < count; j++) {
				gallery_frame_lengths[gallery_frames_count] = duration;
				gallery_frames_count++;
			}
		}
	}
	gallery_tabs_count = data[4];
}

static const GalleryAnimation *gallery_animation(int item)
{
	int i;
	for (i = 0; i < gallery_animations_count; i++)
		if (gallery_animations[i].item == item)
			return &gallery_animations[i];
	return NULL;
}

static int gallery_unlocked(const uint8_t *seen, int count, int cursor)
{
	int index = gallery_tabs[gallery_tab].index[cursor];
	return index < count && ((seen[index >> 3] >> (index & 7)) & 1);
}

int snoot_psp_gallery_update(const uint8_t *seen, int count)
{
	int n, old = gallery_cursor, step = 0;
	int tx, ty, touched = input_touch(&tx, &ty), touch_open = 0;
	if (input_pressed(BTN_CIRCLE)) {
#ifdef VN_NATIVE_AUDIO
		audio_native_ui(1);
#endif
		if (gallery_view) {
			gallery_view = 0;
			gallery_zoom = 0;
			img_release(&gallery_movie_image);
			memset(&gallery_movie_image, 0, sizeof(gallery_movie_image));
			gallery_movie_item = gallery_movie_frame = -1;
			return 0;
		}
		img_release(&gallery_cover);
		memset(&gallery_cover, 0, sizeof(gallery_cover));
		snoot_psp_menu_release();
		return 1;
	}
	if (!gallery_tabs_count)
		return 0;
	n = gallery_tabs[gallery_tab].count;
	if (gallery_view) {
		if (touched && ty < SCREEN_H - menu_px(24)) {
			if (tx < SCREEN_W / 3) step = -1;
			else if (tx > SCREEN_W * 2 / 3) step = 1;
			else if (!gallery_animation(gallery_tabs[gallery_tab].index[gallery_cursor])) {
				gallery_zoom = !gallery_zoom;
				gallery_pan_x = gallery_pan_y = 0;
			}
		}
		if (gallery_animation(gallery_tabs[gallery_tab].index[gallery_cursor]))
			return 0;
		if (input_pressed(BTN_SQUARE)) {
			gallery_zoom = !gallery_zoom;
			gallery_pan_x = gallery_pan_y = 0;
		}
		if (gallery_zoom) {
			gallery_pan_x += (input_down(BTN_LEFT) - input_down(BTN_RIGHT)) * menu_px(6);
			gallery_pan_y += (input_down(BTN_UP) - input_down(BTN_DOWN)) * menu_px(6);
		}
		if (input_pressed(BTN_L1) || (!gallery_zoom && input_pressed(BTN_LEFT)))
			step = -1;
		if (input_pressed(BTN_R1) || (!gallery_zoom && input_pressed(BTN_RIGHT)) || input_pressed(BTN_CROSS))
			step = 1;
		if (step) {
			do {
				gallery_cursor = (gallery_cursor + n + step) % n;
			} while (gallery_cursor != old && !gallery_unlocked(seen, count, gallery_cursor));
			gallery_pan_x = gallery_pan_y = 0;
		}
		return 0;
	}
	if (input_pressed(BTN_L1) || input_pressed(BTN_R1)) {
		gallery_tab =
		    (gallery_tab + gallery_tabs_count + (input_pressed(BTN_R1) ? 1 : -1)) % gallery_tabs_count;
		gallery_cursor = 0;
#ifdef VN_NATIVE_AUDIO
		audio_native_ui(0);
#endif
		return 0;
	}
	if (touched) {
		int t, tab_x = menu_px(12);
		for (t = 0; t < gallery_tabs_count; t++) {
			int w = font_width(gallery_tabs[t].name) + menu_px(18);
			if (ty >= menu_px(8) && ty < menu_px(34) && tx >= tab_x && tx < tab_x + w) {
				gallery_tab = t;
				gallery_cursor = 0;
				#ifdef VN_NATIVE_AUDIO
				audio_native_ui(0);
				#endif
				return 0;
			}
			tab_x += w;
		}
		{
			int cell;
			for (cell = 0; cell < 12 && gallery_cursor / 12 * 12 + cell < n; cell++) {
				int cx = menu_px(12 + cell % 4 * 116), cy = menu_px(48 + cell / 4 * 69);
				if (tx >= cx && tx < cx + menu_px(108) && ty >= cy && ty < cy + menu_px(61)) {
					gallery_cursor = gallery_cursor / 12 * 12 + cell;
					touch_open = gallery_unlocked(seen, count, gallery_cursor);
					break;
				}
			}
		}
	}
	if (input_pressed(BTN_LEFT))
		gallery_cursor = (gallery_cursor + n - 1) % n;
	if (input_pressed(BTN_RIGHT))
		gallery_cursor = (gallery_cursor + 1) % n;
	if (input_pressed(BTN_UP) || input_pressed(BTN_DOWN)) {
		int rows = (n + 3) / 4;
		int row = (gallery_cursor / 4 + rows + (input_pressed(BTN_DOWN) ? 1 : -1)) % rows;
		gallery_cursor = row * 4 + gallery_cursor % 4;
		if (gallery_cursor >= n)
			gallery_cursor = n - 1;
	}
	if ((input_pressed(BTN_CROSS) || touch_open) && gallery_unlocked(seen, count, gallery_cursor)) {
		gallery_view = 1;
		gallery_movie_start = video_frames();
		snoot_psp_menu_release();
		{
			const GalleryAnimation *animation =
			    gallery_animation(gallery_tabs[gallery_tab].index[gallery_cursor]);
			if (animation) {
#ifdef VN_TRACE
				printf("gallery: animation item=%u frames=%u\n", animation->item, animation->count);
#endif
			}
		}
#ifdef VN_NATIVE_AUDIO
		audio_native_ui(0);
#endif
	}
	return 0;
}

void snoot_psp_gallery_draw(const uint8_t *seen, int count)
{
	char name[24], text[64];
	int page = gallery_cursor / 12, cell, x = menu_px(12), t, n;
	if (!gallery_tabs_count) {
		font_draw(LAYER_TEXT, menu_px(20), menu_px(40), "Faltan los datos de galería", 128, 128, 128);
		return;
	}
	n = gallery_tabs[gallery_tab].count;
	if (gallery_view) {
		int index = gallery_tabs[gallery_tab].index[gallery_cursor];
		const GalleryAnimation *animation = gallery_animation(index);
		if (animation && animation->count) {
			uint32_t total = 0, elapsed;
			int frame, k;
			char frame_name[24];
			for (k = 0; k < animation->count; k++)
				total += gallery_frame_lengths[animation->first + k];
			elapsed = total ? (video_frames() - gallery_movie_start) % total : 0;
			for (frame = 0; frame + 1 < animation->count; frame++) {
				unsigned duration = gallery_frame_lengths[animation->first + frame];
				if (elapsed < duration)
					break;
				elapsed -= duration;
			}
			if (gallery_movie_item != index || gallery_movie_frame != frame) {
				snprintf(frame_name, sizeof(frame_name), "GM%03d_%03d.IMG", index, frame);
				img_release(&gallery_movie_image);
				memset(&gallery_movie_image, 0, sizeof(gallery_movie_image));
				if (load_image(frame_name, &gallery_movie_image)) {
					gallery_movie_item = index;
					gallery_movie_frame = frame;
				} else {
					gallery_movie_item = gallery_movie_frame = -1;
				}
			}
			video_rect(LAYER_BG, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0);
			if (gallery_movie_image.loaded)
				img_draw(&gallery_movie_image, LAYER_BOX, 0, 0);
		} else if (gallery_zoom) {
			const Image *image;
			int dx, dy, cx, cy;
			snoot_psp_menu_release();
			res_new_scene();
			image = res_get(gallery_images[index]);
			if (!image)
				return;
			cx = (SCREEN_W - image->full_w) / 2;
			cy = (SCREEN_H - menu_px(24) - image->full_h) / 2;
			dx = cx + gallery_pan_x;
			dy = cy + gallery_pan_y;
			if (image->full_w <= SCREEN_W)
				dx = cx;
			else
				dx = dx > 0 ? 0 : dx < SCREEN_W - image->full_w ? SCREEN_W - image->full_w : dx;
			if (image->full_h <= SCREEN_H - menu_px(24))
				dy = cy;
			else
				dy = dy > 0 ? 0 : dy < SCREEN_H - menu_px(24) - image->full_h
				                  ? SCREEN_H - menu_px(24) - image->full_h
				                  : dy;
			gallery_pan_x = dx - cx;
			gallery_pan_y = dy - cy;
			video_rect(LAYER_BG, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0);
			if (image->big)
				img_draw_big(image, LAYER_BOX, dx, dy);
			else
				img_draw(image, LAYER_BOX, dx, dy);
		} else {
			snprintf(name, sizeof(name), "GV%03d", index);
			snoot_psp_menu_draw(name);
		}
		snprintf(text, sizeof(text), "%s   %d/%d", gallery_tabs[gallery_tab].name, gallery_cursor + 1, n);
		font_draw(LAYER_DEBUG, menu_px(8), SCREEN_H - font_line_height() - menu_px(3), text, 128, 128, 128);
		if (animation)
			font_draw(LAYER_DEBUG, menu_px(155), SCREEN_H - font_line_height() - menu_px(3),
			          "Reproduccion automatica  O volver", 128, 128,
			          128);
		else
			font_draw(LAYER_DEBUG, menu_px(155), SCREEN_H - font_line_height() - menu_px(3),
			          gallery_zoom ? "Cruceta mover  [] reducir  O volver"
			                       : "< > cambiar  [] ampliar  O volver",
			          128, 128, 128);
		video_rect(LAYER_TEXT, 0, SCREEN_H - menu_px(24), SCREEN_W, menu_px(24), 0, 0, 0, 0);
		return;
	}
	snprintf(name, sizeof(name), "G%02d%02d", gallery_tab, page);
	snoot_psp_menu_draw(name);
	for (t = 0; t < gallery_tabs_count; t++) {
		font_draw(LAYER_TEXT, x, menu_px(16), gallery_tabs[t].name, t == gallery_tab ? 0 : 128, 128,
		          t == gallery_tab ? 1 : 128);
		x += font_width(gallery_tabs[t].name) + menu_px(18);
	}
	if (!gallery_cover.loaded)
		load_image("GLOCK.IMG", &gallery_cover);
	for (cell = 0; cell < 12 && page * 12 + cell < n; cell++) {
		int cx = menu_px(12 + cell % 4 * 116), cy = menu_px(48 + cell / 4 * 69);
		if (!gallery_unlocked(seen, count, page * 12 + cell)) {
			img_draw(&gallery_cover, LAYER_TEXT, cx, cy);
			video_rect(LAYER_TEXT, cx, cy, menu_px(108), menu_px(61), 0, 0, 0, 0);
		}
		if (cell == gallery_cursor % 12) {
			video_rect(LAYER_DEBUG, cx - menu_px(2), cy - menu_px(2), menu_px(112), menu_px(2), 0, 255, 3, 0);
			video_rect(LAYER_DEBUG, cx - menu_px(2), cy + menu_px(61), menu_px(112), menu_px(2), 0, 255, 3,
			           0);
			video_rect(LAYER_DEBUG, cx - menu_px(2), cy, menu_px(2), menu_px(61), 0, 255, 3, 0);
			video_rect(LAYER_DEBUG, cx + menu_px(108), cy, menu_px(2), menu_px(61), 0, 255, 3, 0);
		}
	}
	snprintf(text, sizeof(text), "%d / %d", page + 1, (n + 11) / 12);
	font_draw(LAYER_TEXT, menu_px(12), SCREEN_H - font_line_height() - menu_px(3), "L/R: categoría", 128, 128, 128);
	font_draw(LAYER_TEXT, menu_px(220), SCREEN_H - font_line_height() - menu_px(3), text, 128, 128, 128);
	font_draw(LAYER_TEXT, menu_px(330), SCREEN_H - font_line_height() - menu_px(3), "X abrir  O volver", 128, 128,
	          128);
}
#endif
