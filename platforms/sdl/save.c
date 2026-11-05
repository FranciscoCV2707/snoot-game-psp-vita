#include "save.h"
#include "sdl_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char dir[512] = SDL_TARGET_SAVE_DIR;
static unsigned seq;
static int last = -1;
static unsigned checksum(const uint8_t *p)
{
	unsigned s = 0x5a17;
	int i;
	for (i = SAVE_CONTROL_BYTES; i < SAVE_DATA_BYTES; i++)
		s = s * 33 + p[i];
	return s;
}
static void path(char *out, size_t n, int copy)
{
	const char *p = getenv("VN_SAVE_DIR");
	char base[512];
	if (p && *p)
		snprintf(dir, sizeof(dir), "%s", p);
	else if (SDL_TARGET_FIND_GAME_BASE(base, sizeof(base)))
		snprintf(dir, sizeof(dir), "%s/SAVE", base);
	snprintf(out, n, "%s/save%c.bin", dir, copy ? 'B' : 'A');
}
void save_set_icon(const uint8_t *p)
{
	(void)p;
}
static int readcopy(int copy, uint8_t *d, unsigned *number)
{
	char p[600];
	FILE *f;
	uint8_t block[SAVE_DATA_BYTES];
	unsigned saved, stored;
	path(p, sizeof(p), copy);
	f = fopen(p, "rb");
	if (!f)
		return -1;
	if (fread(block, 1, sizeof(block), f) != sizeof(block)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	memcpy(&saved, block + 4, 4);
	memcpy(&stored, block + 8, 4);
	if (memcmp(block, "SNS1", 4) || checksum(block) != stored)
		return -1;
	*number = saved;
	memcpy(d, block, sizeof(block));
	return 0;
}
SaveResult save_read(uint8_t *d)
{
	char p[600];
	unsigned a = 0, b = 0;
	uint8_t other[SAVE_DATA_BYTES];
	int ra, rb;
	path(p, sizeof(p), 0);
	ra = readcopy(0, d, &a);
	rb = readcopy(1, other, &b);
	if (ra && rb)
		return SAVE_NO_FILE;
	if (!rb && (ra || b > a)) {
		memcpy(d, other, sizeof(other));
		seq = b;
		last = 1;
	} else {
		seq = a;
		last = 0;
	}
	return SAVE_OK;
}
SaveResult save_write(uint8_t *d)
{
	char p[600], folder[600];
	FILE *f;
	int copy = last == 0 ? 1 : 0;
	memcpy(d, "SNS1", 4);
	++seq;
	memcpy(d + 4, &seq, 4);
	{
		unsigned c = checksum(d);
		memcpy(d + 8, &c, 4);
		memset(d + 12, 0, 4);
	}
	path(p, sizeof(p), copy);
	snprintf(folder, sizeof(folder), "%s", p);
	{
		char *slash = strrchr(folder, '/');
		if (slash) {
			*slash = 0;
			make_dir(folder);
		}
	}
	f = fopen(p, "wb");
	if (!f)
		return SAVE_WRITE_ERROR;
	if (fwrite(d, 1, SAVE_DATA_BYTES, f) != SAVE_DATA_BYTES) {
		fclose(f);
		return SAVE_WRITE_ERROR;
	}
	fclose(f);
	last = copy;
	return SAVE_OK;
}
