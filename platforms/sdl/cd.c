#include "cd.h"
#include "audio.h"
#include "sdl_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char root[512] = SDL_TARGET_DATA_DIR;
static char filename[600];
volatile uint32_t cd_sync_vblanks, cd_wait_vblanks;
/* Raiz de datos ya resuelta (en PSP puede ser ef0: en una PSP Go). La declara extern quien la necesita:
 * cd.h es contrato y no lleva nada de rutas. */
const char *cd_root(void)
{
	return root;
}
static void psp_stage_shot_names(void)
{
#if defined(__PSP__)
	char path[600];
	FILE *f;
	snprintf(path, sizeof(path), "%s/shot_names.txt", root);
	f = fopen(path, "r");
	if (!f)
		return;
	{
		char names[256];
		if (fgets(names, sizeof(names), f)) {
			size_t l = strlen(names);
			if (l && names[l - 1] == '\n')
				names[l - 1] = 0;
			SDL_setenv("VN_SHOT_NAMES", names, 1);   /* setenv a secas no esta declarado con -std=c99 en el SDK */
		}
	}
	fclose(f);
#endif
}

int cd_init(void)
{
	const char *p = getenv("VN_DATA_DIR");
	char base[512];
	if (p && *p) {
		snprintf(root, sizeof(root), "%s", p);
		return 1;
	}
	if (SDL_TARGET_FIND_GAME_BASE(base, sizeof(base)))
		snprintf(root, sizeof(root), "%s/DATA", base);
#ifdef VN_TRACE
	printf("cd: raiz %s\n", root);
#endif
	/* Los nombres de los pasos `shot` del escenario vienen en un fichero al lado de los datos, como en Vita y 3DS.
	 * En PSP no se pueden pasar por variable de entorno antes de arrancar porque la raiz (ms0:, o ef0: en una Go)
	 * se resuelve aqui mismo; sin esto todas las capturas se llaman "shot.bmp" y la ultima pisa a las demas (T-633). */
	psp_stage_shot_names();
	return 1;
}
static void make_path(const char *path)
{
	const char *base = strrchr(path, '\\');
	const char *slash = strrchr(path, '/');
	int i;
	base = base ? base + 1 : path;
	if (slash && slash + 1 > base)
		base = slash + 1;
	snprintf(filename, sizeof(filename), "%s/%s", root, base);
	for (i = (int)strlen(root) + 1; filename[i]; i++)
		filename[i] = (char)toupper((unsigned char)filename[i]);
}
int cd_find(const char *p, int *lba, int *size)
{
	FILE *f;
	long n;
	make_path(p);
	f = fopen(filename, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	if (lba)
		*lba = 0;
	if (size)
		*size = (int)n;
	return 0;
}
int cd_read_file(const char *p, void *b, int cap)
{
	FILE *f;
	long n;
	make_path(p);
	f = fopen(filename, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	if (n > cap || fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return (int)n;
}
int cd_read_sectors(int l, int n, void *b)
{
	(void)l;
	(void)n;
	(void)b;
	return -1;
}
int cd_read_start(int o, int l, int n, void *b)
{
	(void)o;
	(void)l;
	(void)n;
	(void)b;
	return -1;
}
int cd_read_poll(int o)
{
	(void)o;
	return 0;
}
int cd_busy(void)
{
	return 0;
}
void cd_wait(void)
{
}
