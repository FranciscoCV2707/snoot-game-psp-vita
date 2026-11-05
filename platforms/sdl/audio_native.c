#include "audio_native.h"
#ifdef VN_NATIVE_AUDIO
#include "audio.h"
#include "sdl_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisfile.h>

#define RATE          44100
#define PCM_RATE      22050
#define VOICES        15
#define MUSIC_A       0
#define AMBIENT_A     2
#define SFX_A         6
#define UI_VOICE      14
#define BUFFER_FRAMES 1024
#define RING_FRAMES   8192 /* ~186 ms a 44.1 kHz para absorber pausas breves del decodificador */

typedef struct {
	int value, target, step, remainder, error, duration, left, sign;
} Gain;
typedef struct {
	OggVorbis_File file;
	FILE *pcm_file;
	int open, id, loop, pos, count, channel, paused;
	volatile int finished;
	volatile int decode_done;
	int pcm_format;
	int pcm_phase, pcm_sample;
	int ring_read, ring_write, ring_count;
	int16_t samples[BUFFER_FRAMES * 2];
	Gain gain;
	const int16_t *clip;
	int clip_frames;
} Stream;
typedef struct {
	int16_t *samples;
	int frames;
} UiClip;
static UiClip ui_clips[2];
static Stream voices[VOICES];
static int16_t audio_ring[VOICES][RING_FRAMES * 2];
static Gain buses[6];
static Gain level_gain[2];
static SDL_AudioDeviceID device;
static SDL_mutex *decode_mutex, *ring_mutex;
static SDL_Thread *decode_thread;
static volatile int decode_running;
static uint8_t loops[256];
static uint8_t pcm_ids[256];
static int entry_count, title_id, click_id, back_id;
static int current = -1, queued = -1, next_sfx;
static int music_level = 10, sfx_level = 10;
#ifdef VN_TRACE
static Uint32 mix_calls, mix_frames, mix_ms, mix_max_ms, mix_late;
static int mix_raw_peak, mix_gain_peak, mix_bus_peak;
static Uint32 mix_clipped;
static Uint32 mix_underruns;
#endif
extern const char *cd_root(void);

static void gain_set(Gain *gain, int target, int tenths)
{
	int delta = target - gain->value;
	memset(gain, 0, sizeof(*gain));
	gain->target = target;
	gain->value = target - delta;
	gain->duration = gain->left = tenths * (RATE / 10);
	if (gain->duration <= 0) {
		gain->value = target;
		gain->left = 0;
		return;
	}
	gain->step = delta / gain->duration;
	gain->sign = delta < 0 ? -1 : 1;
	gain->remainder = (delta < 0 ? -delta : delta) % gain->duration;
}

static int gain_next(Gain *gain)
{
	if (gain->left > 0) {
		gain->value += gain->step;
		gain->error += gain->remainder;
		if (gain->error >= gain->duration) {
			gain->value += gain->sign;
			gain->error -= gain->duration;
		}
		if (--gain->left == 0)
			gain->value = gain->target;
	}
	return gain->value;
}

static void close_stream(Stream *voice)
{
	#ifdef VN_PSP_AUDIO_DIAG
	if (voice->open && !voice->pcm_file)
		printf("audio: closing vorbis id=%d voice=%p pos=%d count=%d\n", voice->id, (void *)voice,
		       voice->pos, voice->count);
	#endif
	if (voice->pcm_file)
		fclose(voice->pcm_file);
	else if (voice->open)
		ov_clear(&voice->file);
	memset(voice, 0, sizeof(*voice));
	voice->id = -1;
}

static int open_stream(Stream *voice, int id, int loop, int channel)
{
	char path[640];
	vorbis_info *info;
	long bytes;
	close_stream(voice);
	#ifdef VN_PSP_AUDIO_DIAG
	printf("audio: opening id=%d voice=%p loop=%d channel=%d\n", id, (void *)voice, loop, channel);
	#endif
	if (id < 0 || id >= entry_count)
		return 0;
	if (pcm_ids[id]) {
		snprintf(path, sizeof(path), "%s/A%04d.PCM", cd_root(), id);
		voice->pcm_file = fopen(path, "rb");
		if (!voice->pcm_file) {
			printf("audio: cannot open %s\n", path);
			return 0;
		}
		voice->open = 1;
		voice->pcm_format = 1;
		voice->id = id;
		voice->loop = loop;
		voice->channel = channel;
		voice->gain.value = voice->gain.target = 65536;
		bytes = (long)fread(voice->samples, sizeof(int16_t), BUFFER_FRAMES, voice->pcm_file);
		voice->count = (int)bytes;
		if (!voice->count) {
			close_stream(voice);
			return 0;
		}
#ifdef VN_TRACE
		printf("audio: PCM id=%d loop=%d channel=%d\n", id, loop, channel);
#endif
		return 1;
	}
	snprintf(path, sizeof(path), "%s/A%04d.OGG", cd_root(), id);
	if (ov_fopen(path, &voice->file) != 0) {
		printf("audio: cannot open %s\n", path);
		return 0;
	}
	voice->open = 1;
	info = ov_info(&voice->file, -1);
	if (!info || info->rate != RATE || info->channels != 2) {
		printf("audio: expected 44100 Hz stereo: %s\n", path);
		close_stream(voice);
		return 0;
	}
	voice->id = id;
	voice->loop = loop;
	voice->channel = channel;
	voice->gain.value = voice->gain.target = 65536;
	/* Vorbis builds its decode tables on the first read. Do that before playback. */
	{
		int section;
		long bytes = ov_read(&voice->file, (char *)voice->samples, sizeof(voice->samples), 0, 2, 1, &section);
		if (bytes <= 0) {
			close_stream(voice);
			return 0;
		}
		voice->count = (int)bytes / 4;
	}
#ifdef VN_TRACE
	printf("audio: OGG id=%d loop=%d channel=%d\n", id, loop, channel);
#endif
	return 1;
}

static int read_frame(Stream *voice, int *left, int *right)
{
	int tries = 0;
	if (voice->clip && !voice->finished) {
		if (voice->pos >= voice->clip_frames) {
			voice->finished = 1;
			return 0;
		}
		*left = voice->clip[voice->pos * 2];
		*right = voice->clip[voice->pos * 2 + 1];
		voice->pos++;
		return 1;
	}
	if (!voice->open || voice->finished || voice->decode_done)
		return 0;
	if (voice->pcm_format) {
		if (voice->pcm_phase) {
			voice->pcm_phase = 0;
			*left = *right = voice->pcm_sample;
			return 1;
		}
		while (voice->pos >= voice->count) {
			size_t frames = fread(voice->samples, sizeof(int16_t), BUFFER_FRAMES, voice->pcm_file);
			if (frames) {
				voice->pos = 0;
				voice->count = (int)frames;
				break;
			}
			if (++tries <= 8 && voice->loop && fseek(voice->pcm_file, 0, SEEK_SET) == 0)
				continue;
			voice->decode_done = 1;
			return 0;
		}
		voice->pcm_sample = voice->samples[voice->pos++];
		voice->pcm_phase = 1;
		*left = *right = voice->pcm_sample;
		return 1;
	}
	while (voice->pos >= voice->count) {
		long bytes;
		int section;
		bytes = ov_read(&voice->file, (char *)voice->samples, sizeof(voice->samples), 0, 2, 1, &section);
		if (bytes > 0) {
			voice->pos = 0;
			voice->count = (int)bytes / 4;
			break;
		}
		if (++tries > 8 || (bytes == 0 && (!voice->loop || ov_pcm_seek(&voice->file, 0) != 0))) {
			voice->decode_done = 1;
			return 0;
		}
	}
	*left = voice->samples[voice->pos * 2];
	*right = voice->samples[voice->pos * 2 + 1];
	voice->pos++;
	return 1;
}

/* El hilo de audio solo consume PCM ya decodificado. Vorbis y la lectura de la Memory Stick corren fuera del
 * callback, que en PPSSPP/PSP debe devolver 2048 cuadros cada ~46 ms. */
static void ring_write_frames(int slot, const int16_t *samples, int frames)
{
	Stream *voice = &voices[slot];
	int i;
	if (frames <= 0)
		return;
	SDL_LockMutex(ring_mutex);
	for (i = 0; i < frames && voice->ring_count < RING_FRAMES; i++) {
		int at = voice->ring_write * 2;
		audio_ring[slot][at] = samples[i * 2];
		audio_ring[slot][at + 1] = samples[i * 2 + 1];
		voice->ring_write = (voice->ring_write + 1) % RING_FRAMES;
		voice->ring_count++;
	}
	SDL_UnlockMutex(ring_mutex);
}

static int SDLCALL decode_worker(void *userdata)
{
	int16_t block[BUFFER_FRAMES * 2];
	int running = 1;
	(void)userdata;
	while (running) {
		int i, worked = 0;
		SDL_LockMutex(decode_mutex);
		for (i = 0; i < VOICES; i++) {
			Stream *voice = &voices[i];
			int available, n = 0, l, r;
			if (!voice->open || voice->clip || voice->finished || voice->decode_done)
				continue;
			SDL_LockMutex(ring_mutex);
			available = voice->ring_count;
			SDL_UnlockMutex(ring_mutex);
			if (available > RING_FRAMES - BUFFER_FRAMES)
				continue;
			while (n < BUFFER_FRAMES && read_frame(voice, &l, &r)) {
				block[n * 2] = (int16_t)l;
				block[n * 2 + 1] = (int16_t)r;
				n++;
			}
			if (n)
				ring_write_frames(i, block, n);
			worked = 1;
		}
		running = decode_running;
		SDL_UnlockMutex(decode_mutex);
		if (!worked && running)
			SDL_Delay(2);
	}
	return 0;
}

static void prime_stream(int slot)
{
	Stream *voice = &voices[slot];
	int16_t block[BUFFER_FRAMES * 2];
	int n = 0, l, r;
	/* open_stream ya precargo un bloque; publicamos ese bloque antes de soltar el audio para evitar silencio inicial. */
	while (n < BUFFER_FRAMES && read_frame(voice, &l, &r)) {
		block[n * 2] = (int16_t)l;
		block[n * 2 + 1] = (int16_t)r;
		n++;
	}
	ring_write_frames(slot, block, n);
}

static void mix(void *userdata, Uint8 *data, int bytes)
{
#ifdef VN_TRACE
	Uint32 begin = SDL_GetTicks();
#endif
	int16_t *out = (int16_t *)data;
	int frame, i, starts[VOICES], available[VOICES], consumed[VOICES] = {0};
	int frames = bytes / 4;
	(void)userdata;
	SDL_LockMutex(ring_mutex);
	for (i = 0; i < VOICES; i++) {
		starts[i] = voices[i].ring_read;
		available[i] = voices[i].ring_count;
	}
	SDL_UnlockMutex(ring_mutex);
	for (frame = 0; frame < frames; frame++) {
		int left = 0, right = 0, volume[6];
		int music_gain = gain_next(&level_gain[0]);
		int sfx_gain = gain_next(&level_gain[1]);
		for (i = 0; i < 6; i++)
			volume[i] = gain_next(&buses[i]);
		for (i = 0; i < VOICES; i++) {
			Stream *voice = &voices[i];
			int l = 0, r = 0, gain, level, have_sample = 0;
			if (voice->paused || voice->finished)
				continue;
			if (voice->clip) {
				if (voice->pos < voice->clip_frames) {
					l = voice->clip[voice->pos * 2];
					r = voice->clip[voice->pos * 2 + 1];
					voice->pos++;
					have_sample = 1;
				} else
					voice->finished = 1;
			} else if (voice->open && consumed[i] < available[i]) {
				int at = ((starts[i] + consumed[i]) % RING_FRAMES) * 2;
				l = audio_ring[i][at];
				r = audio_ring[i][at + 1];
				consumed[i]++;
				have_sample = 1;
			} else if (voice->open && voice->decode_done && consumed[i] >= available[i]) {
				if (i == current && queued >= 0) {
					voices[queued].paused = 0;
					current = queued;
					queued = -1;
				} else
					voice->finished = 1;
			} else if (voice->open) {
#ifdef VN_TRACE
				mix_underruns++;
#endif
			}
			if (!have_sample)
				continue;
			gain = gain_next(&voice->gain);
			level = voice->channel == 0 ? music_gain : sfx_gain;
#ifdef VN_TRACE
			if (l > mix_raw_peak || -l > mix_raw_peak)
				mix_raw_peak = l < 0 ? -l : l;
			if (gain > mix_gain_peak)
				mix_gain_peak = gain;
			if (volume[voice->channel] > mix_bus_peak)
				mix_bus_peak = volume[voice->channel];
#endif
			l = (l * gain >> 16) * volume[voice->channel] / 255;
			r = (r * gain >> 16) * volume[voice->channel] / 255;
			left += l * level >> 16;
			right += r * level >> 16;
			if (!voice->gain.left && voice->gain.target == 0)
				voice->finished = 1;
		}
		out[frame * 2] = (int16_t)(left < -32768 ? -32768 : left > 32767 ? 32767 : left);
		out[frame * 2 + 1] = (int16_t)(right < -32768 ? -32768 : right > 32767 ? 32767 : right);
#ifdef VN_TRACE
		mix_clipped += left < -32768 || left > 32767;
#endif
	}
	SDL_LockMutex(ring_mutex);
	for (i = 0; i < VOICES; i++)
		if (consumed[i] > 0) {
			voices[i].ring_read = (voices[i].ring_read + consumed[i]) % RING_FRAMES;
			voices[i].ring_count -= consumed[i];
		}
	SDL_UnlockMutex(ring_mutex);
#ifdef VN_TRACE
	{
		Uint32 elapsed = SDL_GetTicks() - begin;
		mix_calls++;
		mix_frames += bytes / 4;
		mix_ms += elapsed;
		if (elapsed > mix_max_ms)
			mix_max_ms = elapsed;
		if (elapsed * RATE > (Uint32)(bytes / 4) * 1000)
			mix_late++;
	}
#endif
}

static int load_ui_clip(UiClip *clip, int id)
{
	Stream source;
	int l, r, frames = 0;
	ogg_int64_t total;
	memset(&source, 0, sizeof(source));
	if (!open_stream(&source, id, 0, AUDIO_CH_SFX))
		return 0;
	/* UI assets are short; bound decoded storage to one second per clip. */
	if (source.pcm_format) {
		if (fseek(source.pcm_file, 0, SEEK_END) != 0) {
			close_stream(&source);
			return 0;
		}
		total = (ftell(source.pcm_file) / (ogg_int64_t)sizeof(int16_t)) * (RATE / PCM_RATE);
		if (fseek(source.pcm_file, source.count * (long)sizeof(int16_t), SEEK_SET) != 0) {
			close_stream(&source);
			return 0;
		}
	} else {
		total = ov_pcm_total(&source.file, -1);
	}
	if (total <= 0 || total > RATE) {
		close_stream(&source);
		return 0;
	}
	clip->samples = malloc((size_t)total * 4);
	if (!clip->samples) {
		close_stream(&source);
		return 0;
	}
	while (frames < (int)total && read_frame(&source, &l, &r)) {
		clip->samples[frames * 2] = (int16_t)l;
		clip->samples[frames * 2 + 1] = (int16_t)r;
		frames++;
	}
	close_stream(&source);
	if (frames != (int)total) {
		free(clip->samples);
		clip->samples = NULL;
		return 0;
	}
	clip->frames = frames;
	return 1;
}

int audio_init(void)
{
	char path[640];
	uint8_t header[12];
	FILE *file;
	SDL_AudioSpec spec;
	int i;
	snprintf(path, sizeof(path), "%s/NATIVE.AUD", cd_root());
	file = fopen(path, "rb");
	if (!file)
		return -1;
	if (fread(header, 1, 12, file) != 12 || memcmp(header, "VNA1", 4) != 0) {
		fclose(file);
		return -1;
	}
	entry_count = header[4] | (header[5] << 8);
	title_id = header[6] | (header[7] << 8);
	click_id = header[8] | (header[9] << 8);
	back_id = header[10] | (header[11] << 8);
	if (entry_count > 256 || fread(loops, 1, entry_count, file) != (size_t)entry_count) {
		fclose(file);
		return -1;
	}
	for (i = 0; i < entry_count; i++) {
		pcm_ids[i] = (loops[i] & 2) != 0;
		loops[i] &= 1;
	}
	fclose(file);
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return -1;
	decode_mutex = SDL_CreateMutex();
	ring_mutex = SDL_CreateMutex();
	if (!decode_mutex || !ring_mutex)
		return -1;
	if (!load_ui_clip(&ui_clips[0], click_id) || !load_ui_clip(&ui_clips[1], back_id))
		return -1;
	for (i = 0; i < VOICES; i++)
		voices[i].id = -1;
	for (i = 0; i < 6; i++)
		buses[i].value = buses[i].target = 255;
	for (i = 0; i < 2; i++)
		level_gain[i].value = level_gain[i].target = 65536;
	SDL_zero(spec);
	spec.freq = RATE;
	spec.channels = 2;
	spec.format = AUDIO_S16SYS;
	spec.samples = 2048;
	spec.callback = mix;
	device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
	if (!device)
		return -1;
	decode_running = 1;
	decode_thread = SDL_CreateThread(decode_worker, "snoot-audio-decode", NULL);
	if (!decode_thread) {
		decode_running = 0;
		SDL_CloseAudioDevice(device);
		device = 0;
		return -1;
	}
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
	SDL_PauseAudioDevice(device, 0);
#ifdef VN_TRACE
	printf("audio: native OGG streaming 44100 stereo, %d entries, title=%d\n", entry_count, title_id);
#endif
	return 0;
}

void audio_music_play(int id, int fade, int flags)
{
	int old, slot;
	ogg_int64_t position = 0;
	if (!device || id < 0 || id >= entry_count)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
	old = current;
	slot = current == 0 ? 1 : 0;
	if (old >= 0 && (flags & 2) && voices[old].open) {
		int buffered;
		position = ov_pcm_tell(&voices[old].file) - (voices[old].count - voices[old].pos);
		SDL_LockMutex(ring_mutex);
		buffered = voices[old].ring_count;
		SDL_UnlockMutex(ring_mutex);
		position -= buffered;
		if (position < 0)
			position = 0;
	}
	if (open_stream(&voices[slot], id, loops[id], 0)) {
		ogg_int64_t total = ov_pcm_total(&voices[slot].file, -1);
		if (position > 0 && total > 0) {
			ov_pcm_seek(&voices[slot].file, position % total);
			voices[slot].pos = voices[slot].count = 0;
		}
		prime_stream(slot);
		if (fade > 0) {
			voices[slot].gain.value = 0;
			gain_set(&voices[slot].gain, 65536, fade);
		}
		if (old >= 0) {
			if (fade > 0)
				gain_set(&voices[old].gain, 0, fade);
			else
				close_stream(&voices[old]);
		}
		current = slot;
		queued = -1;
	}
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}

void audio_update(void)
{
	int i;
	if (!device)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
#ifdef VN_TRACE
	{
		static Uint32 last;
		Uint32 now = SDL_GetTicks();
		if (now - last >= 2000) {
			printf("audio: timing time=%lu calls=%lu frames=%lu cpu_ms=%lu max_ms=%lu late=%lu underrun=%lu\n",
			       (unsigned long)now, (unsigned long)mix_calls, (unsigned long)mix_frames,
			       (unsigned long)mix_ms, (unsigned long)mix_max_ms, (unsigned long)mix_late,
			       (unsigned long)mix_underruns);
			printf("audio: signal raw=%d gain=%d bus=%d music=%d sfx=%d clips=%lu\n", mix_raw_peak,
			       mix_gain_peak, mix_bus_peak, music_level, sfx_level, (unsigned long)mix_clipped);
			last = now;
		}
	}
#endif
	if (current >= 0 && voices[current].finished) {
		queued = -1;
		current = -1;
	}
	for (i = 0; i < VOICES; i++)
		if (voices[i].finished)
			close_stream(&voices[i]);
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}

void audio_music_queue(int id)
{
	int slot;
	if (!device || id < 0 || id >= entry_count)
		return;
	SDL_LockAudioDevice(device);
	if (current < 0) {
		SDL_UnlockAudioDevice(device);
		audio_music_play(id, 0, 0);
		return;
	}
	SDL_LockMutex(decode_mutex);
	slot = current == 0 ? 1 : 0;
	if (open_stream(&voices[slot], id, loops[id], 0)) {
		prime_stream(slot);
		queued = slot;
		voices[slot].paused = 1;
		voices[current].loop = 0;
	}
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}

void audio_music_stop(int fade)
{
	int i;
	if (!device)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
	for (i = 0; i < 2; i++) {
		if (fade > 0)
			gain_set(&voices[i].gain, 0, fade);
		else
			close_stream(&voices[i]);
	}
	current = queued = -1;
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}

void audio_ambient_play(int slot, int id, int fade)
{
	if (!device || slot < 0 || slot >= 4)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
	if (open_stream(&voices[AMBIENT_A + slot], id, 1, slot + 1)) {
		prime_stream(AMBIENT_A + slot);
		if (fade > 0) {
			voices[AMBIENT_A + slot].gain.value = 0;
			gain_set(&voices[AMBIENT_A + slot].gain, 65536, fade);
		}
	}
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}

void audio_ambient_stop(int slot, int fade)
{
	if (!device || slot < 0 || slot >= 4)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
	if (fade > 0)
		gain_set(&voices[AMBIENT_A + slot].gain, 0, fade);
	else
		close_stream(&voices[AMBIENT_A + slot]);
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}

void audio_sfx_play(int id)
{
	if (!device)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
	if (open_stream(&voices[SFX_A + next_sfx], id, 0, AUDIO_CH_SFX))
		prime_stream(SFX_A + next_sfx);
	next_sfx = (next_sfx + 1) % 8;
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}
void audio_sfx_stop_fade(int fade)
{
	int i;
	if (!device)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
	for (i = SFX_A; i < UI_VOICE; i++) {
		if (fade > 0)
			gain_set(&voices[i].gain, 0, fade);
		else
			close_stream(&voices[i]);
	}
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}
void audio_sfx_stop(void)
{
	audio_sfx_stop_fade(0);
}
void audio_stop_all(void)
{
	int i;
	audio_music_stop(0);
	audio_sfx_stop();
	for (i = 0; i < 4; i++)
		audio_ambient_stop(i, 0);
}
void audio_volume(int channel, int volume, int time)
{
	if (!device || channel < 0 || channel > 5)
		return;
	SDL_LockAudioDevice(device);
	gain_set(&buses[channel], volume, time);
	SDL_UnlockAudioDevice(device);
}
void audio_set_levels(int music, int effects)
{
	int targets[2];
	int i;
	if (!device)
		return;
	if (music < 0)
		music = 0;
	else if (music > 10)
		music = 10;
	if (effects < 0)
		effects = 0;
	else if (effects > 10)
		effects = 10;
	targets[0] = music * 65536 / 10;
	targets[1] = effects * 65536 / 10;
	SDL_LockAudioDevice(device);
	music_level = music;
	sfx_level = effects;
	for (i = 0; i < 2; i++)
		if (level_gain[i].target != targets[i])
			gain_set(&level_gain[i], targets[i], 1);
	SDL_UnlockAudioDevice(device);
}
int audio_music_current(void)
{
	int id;
	if (!device)
		return -1;
	SDL_LockAudioDevice(device);
	id = current >= 0 ? voices[current].id : -1;
	SDL_UnlockAudioDevice(device);
	return id;
}
int audio_ambient_current(int slot)
{
	int id;
	if (!device || slot < 0 || slot >= 4)
		return -1;
	SDL_LockAudioDevice(device);
	id = voices[AMBIENT_A + slot].id;
	SDL_UnlockAudioDevice(device);
	return id;
}
int audio_ambient_missing(int slot, int id)
{
	return audio_ambient_current(slot) != id;
}
int audio_sfx_missing(int id)
{
	(void)id;
	return 0;
}
void audio_preload(int id)
{
	(void)id;
}
int audio_loading(void)
{
	return 0;
}
void audio_cd_begin(void)
{
}
void audio_cd_end(void)
{
}
void audio_batch_begin(void)
{
}
void audio_batch_end(void)
{
}
int audio_cd_paused(void)
{
	return 0;
}
void audio_video_begin(void)
{
}
void audio_video_end(int frames)
{
	(void)frames;
}
void audio_native_title(void)
{
	audio_music_play(title_id, 0, 0);
}
void audio_native_ui(int back)
{
	if (!device)
		return;
	SDL_LockAudioDevice(device);
	SDL_LockMutex(decode_mutex);
	close_stream(&voices[UI_VOICE]);
	voices[UI_VOICE].clip = ui_clips[back != 0].samples;
	voices[UI_VOICE].clip_frames = ui_clips[back != 0].frames;
	voices[UI_VOICE].id = back ? back_id : click_id;
	voices[UI_VOICE].channel = AUDIO_CH_SFX;
	voices[UI_VOICE].gain.value = voices[UI_VOICE].gain.target = 65536;
	SDL_UnlockMutex(decode_mutex);
	SDL_UnlockAudioDevice(device);
}
#endif
