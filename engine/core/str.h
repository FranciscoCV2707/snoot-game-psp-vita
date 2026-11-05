/*
 * Demultiplexado de video STR (MDEC): reensambla el bitstream de cada cuadro a partir de sus sectores. Es logica
 * portable (sin hardware): el motor la alimenta con los sectores que lee del CD y la prueba corre en el PC.
 * Dos buffers: uno se llena con el cuadro nuevo mientras el otro espera al consumidor (el decodificador del MDEC).
 * Si el consumidor se atrasa, el cuadro viejo se pierde y se cuenta en `dropped`; nunca se corrompe un cuadro.
 */
#pragma once

#include <stdint.h>

#define STR_SECTOR 2048          /* datos de un sector de video (sin subcabecera ni EDC) */
#define STR_HEADER 32            /* cabecera de cada sector de video */
#define STR_PAYLOAD 2016         /* bitstream por sector */

typedef struct {
	uint8_t *buf[2];             /* cada uno de `cap` bytes */
	uint32_t cap;
	int fill;                    /* buffer donde se arma el cuadro actual */
	int ready;                   /* buffer con un cuadro completo esperando al consumidor; -1 si no hay */
	uint32_t ready_len, ready_frame;
	uint16_t ready_w, ready_h;
	uint32_t frame_id;           /* cuadro que se esta armando */
	int started, have, want;
	uint32_t bs_len;
	uint16_t width, height;
	int frames, dropped;         /* cuadros completos y cuadros perdidos */
} StrDemux;

void str_init(StrDemux *d, uint8_t *buf0, uint8_t *buf1, uint32_t cap);
/* Procesa un sector de 2048 bytes de datos. Devuelve 1 si era de video y se uso, 0 si no era de video (audio u
 * otro) o un cuadro viejo, y -1 si la cabecera no es de STR. */
int str_feed(StrDemux *d, const uint8_t *sector);
/* Saca el cuadro completo listo (1 si lo hubo): bitstream, largo, dimensiones y numero. Libera el buffer. */
int str_take(StrDemux *d, const uint8_t **bs, uint32_t *len, uint16_t *w, uint16_t *h, uint32_t *frame);
