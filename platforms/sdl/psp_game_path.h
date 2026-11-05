#pragma once

#include <stdio.h>

typedef int (*PspGameDataExists)(const char *path);

static inline int psp_find_game_base_with(char *out, int n, PspGameDataExists data_exists)
{
	static const char *const drives[] = {"ms0", "ef0"};
	char data_file[96];
	int i, written;

	if (!out || n <= 0 || !data_exists)
		return 0;
	for (i = 0; i < (int)(sizeof(drives) / sizeof(drives[0])); i++) {
		written = snprintf(out, (size_t)n, "%s:/PSP/GAME/SNOOT", drives[i]);
		if (written < 0 || written >= n)
			continue;
		written = snprintf(data_file, sizeof(data_file), "%s/DATA/GAME.BIN", out);
		if (written >= 0 && written < (int)sizeof(data_file) && data_exists(data_file))
			return 1;
	}
	out[0] = 0;
	return 0;
}
