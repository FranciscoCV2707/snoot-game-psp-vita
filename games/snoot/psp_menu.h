#pragma once
#include "text.h"

#if defined(VN_PSP_NATIVE_MENU) || defined(VN_VITA_NATIVE_MENU)
#define VN_NATIVE_SNOOT_MENU 1
#endif

/* Interfaz nativa de Snoot compuesta desde el arte y las traducciones originales. */
int snoot_psp_menu_draw(const char *page);
void snoot_psp_choice_draw(const char *const *choices, int count, int cursor);
int snoot_psp_choice_hit(int x, int y, int count, int cursor, int *row);
void snoot_psp_menu_release(void);
void snoot_psp_options_draw(int music_level, int sfx_level, int text_cps, int auto_forward, int cursor,
                            int transitions, int skip_unseen, int skip_after_choices, int chapter_select,
                            int system_menu);
void snoot_psp_dialog_draw(const TextRun *run, const char *speaker, int r, int g, int b, int fade, int fade_max,
                           int with_text);
void snoot_psp_gallery_open(void);
int snoot_psp_gallery_update(const uint8_t *seen, int count);
void snoot_psp_gallery_draw(const uint8_t *seen, int count);
