/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef CDROM_AUDIO_H
#define CDROM_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_FIFO_CAPACITY (44100 * 4)   /* ~2 s stereo at 44100 Hz */
/* Standing latency allowed in the XA queue. Sectors arrive in interleave bursts
 * (a movie's audio sectors come a few at a time between video sectors), so some
 * buffering is required, but letting the queue grow to its full capacity puts
 * hundreds of milliseconds between the picture and its sound. Four sectors'
 * worth (2352 output frames each) rides out the interleave and keeps the delay
 * around 200 ms worst case. */
#define AUDIO_FIFO_MAX_LATENCY (2352 * 4)

typedef struct {
    uint32_t data[AUDIO_FIFO_CAPACITY];   /* packed: lo16=left, hi16=right */
    /* Health counters: pushed/popped tell whether the drive is feeding the SPU
     * at the rate the SPU consumes, dropped means the FIFO overflowed. */
    uint32_t total_pushed, total_popped, total_dropped;
    /* Pops taken with the FIFO empty: a *missing* frame rather than a discarded
     * one, so total_dropped never counts it. The CD input falls to zero for that
     * sample and jumps back when the next sector lands, which is a click. */
    uint32_t total_starved;
    uint32_t head, tail, count;
} AudioFifo;

typedef struct {
    int32_t prev1[2];       /* IIR state [0]=L [1]=R */
    int32_t prev2[2];
    int16_t ring[2][32];    /* zigzag resampler ring buffers [L/R][sample] */
    uint8_t ring_p;
    uint8_t sixstep;
    int16_t ring18[2][32];  /* 18900 Hz resampler ring buffers */
    uint8_t ring18_p;
    uint8_t sixstep18;      /* 18900 Hz: input credit, 3 per emitted output */
    uint8_t phase18;        /* 18900 Hz: which of the 7 zigzag phases is next */
} XaAdpcmState;

void cdrom_audio_init(AudioFifo *fifo, XaAdpcmState *xa);
void cdrom_audio_fifo_push(AudioFifo *fifo, int16_t left, int16_t right);
bool cdrom_audio_fifo_pop(AudioFifo *fifo, int16_t *left, int16_t *right);
bool cdrom_audio_fifo_empty(const AudioFifo *fifo);

/* Decode one XA-ADPCM sector (18 chunks × 128 bytes starting at sector byte 24).
 * Muting is not a parameter: it belongs to the output stage, see
 * cdrom_audio_apply_output(). */
void cdrom_audio_decode_xa(XaAdpcmState *xa, AudioFifo *fifo,
                            const uint8_t *xa_data,
                            bool stereo, bool bits8, bool rate_18900);

/* Process one CDDA raw sector (2352 bytes, audio starts at byte 0) */
void cdrom_audio_process_cdda(AudioFifo *fifo, const uint8_t *raw_sector);

/* Pop one stereo frame for SPU/SDL output */
void cdrom_audio_get_frame(AudioFifo *fifo, int16_t *left, int16_t *right);

/* The CD controller's output stage on one stereo frame: muted forces both
 * outputs to 0, otherwise the ATV0-ATV3 matrix (ll = ATV0 L->L, lr = ATV3 R->L,
 * rl = ATV1 L->R, rr = ATV2 R->R; 80h = unity) with saturation. Pure. */
void cdrom_audio_apply_output(int16_t *left, int16_t *right, bool muted,
                              uint8_t ll, uint8_t lr, uint8_t rl, uint8_t rr);

#endif /* CDROM_AUDIO_H */
