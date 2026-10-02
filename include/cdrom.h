/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef CDROM_H
#define CDROM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "cdrom_disc.h"
#include "cdrom_audio.h"

struct Interconnect;

/* =========================================================================
 * Constants
 * ========================================================================= */

#define CDROM_PARAM_FIFO_SIZE    16
#define CDROM_RESPONSE_FIFO_SIZE 16
#define CDROM_SECTOR_BUFFERS      8

/* Timing (CPU cycles @ 33.8688 MHz) — calibrated against pcsx-redux */
#define CDROM_CLK_HZ             33868800u

/* Sector read: 75 sectors/sec → PSX_CLK/75 cycles per sector */
#define CDROM_SECTOR_TIME        (CDROM_CLK_HZ / 75u)       /* 451584 — 1x */
#define CDROM_SECTOR_TIME_2X     (CDROM_CLK_HZ / 75u / 2u)  /* 225792 — 2x */

/* First response (INT3), measured on a PAL PSone (cdromdrive.md:1877-1894):
 * Nop averages 0xc4e1 with the motor running and 0x5cf4 when stopped, while
 * Init — and ReadTOC, which runs the same initialisation — takes 0x13cce before
 * it even acknowledges. This used to be a flat 25000 for every command, about
 * half the running average. */
#define CDROM_ACK_DELAY          50401   /* 0xc4e1  — average, motor running */
#define CDROM_ACK_DELAY_STOPPED  23796   /* 0x5cf4  — motor stopped */
#define CDROM_ACK_DELAY_INIT     81102   /* 0x13cce — Init / ReadTOC */
#define CDROM_ID_READ_DELAY      18944   /* 0x4a00 — GetID second response (:1899) */
/* Init's second response is base + the drive's work: it drops back to single
 * speed and re-homes the head, and DOCS/cdromdrive.md:1894-1896 says the reply
 * depends on that seek (and spin-up if the motor was off). Measured on a
 * DuckStation Devel run of the same disc: ~740 ms both times Init is issued
 * during boot, which is base + a speed change + the seek. The BIOS re-issues
 * Init after ~415 ms of waiting, so this delay is longer than the retry
 * interval by design — the drive must answer the first request at its own
 * deadline and ignore the retries (cdrom_commands.c, CDC_INIT). */
#define CDROM_INIT_DELAY       4100000   /* base, before speed change + seek */
#define CDROM_RESET_DELAY      4100000

/* Seek delays */
/* A same-location "instant" seek is never scheduled sooner than this on real
 * hardware; BIOS's own ISR for the first (INT3 command-ack) response needs
 * enough real cycles to fully run and re-enable interrupts before the second
 * response's IRQ fires, or the second IRQ can land while interrupts are still
 * disabled and get silently dropped. The old 0x800 (2048-cycle) value only
 * worked because instructions were costed at a flat 1 cycle/instruction; now
 * that BIOS ROM fetches cost real wait-states (cpu_icache.c), 2048 cycles is
 * no longer enough headroom. */
#define CDROM_SEEK_FAST_DELAY    30000  /* setloc_pending, no head movement */
#define CDROM_SEEK_DELAY         (CDROM_SECTOR_TIME * 4u)      /* real seek base */
/* Per-sector cost of a short head move, ~6.93 ms at 33.8688 MHz. Calibrated
 * against observed drive behaviour, not documented — see
 * cdrom_disc_get_seek_ticks(). */
#define CDROM_SEEK_FINE_PER_LBA  234700u
#define CDROM_SEEK_CHANGE_DELAY  (CDROM_SECTOR_TIME * 30u)     /* post-location-change first read */

/* Spinup */
#define CDROM_SPINUP_DELAY       (CDROM_SECTOR_TIME * 125u / 2u)  /* ~28M cycles */

/* Stop — second response, measured (cdromdrive.md:1903-1905). The drive takes
 * nearly twice as long to stop from double speed as from single, and the two
 * used to be one speed-independent value at about half the 1x figure. */
#define CDROM_STOP_IDLE_DELAY    7547        /* 0x1d7b   — already stopped */
#define CDROM_STOP_1X_DELAY      13863114    /* 0xd38aca — spinning at 1x */
#define CDROM_STOP_2X_DELAY      25845878    /* 0x18a6076 — spinning at 2x */

/* Pause — speed-dependent. Measured on hardware (DOCS/cdromdrive.md:1888-1889):
 * 2 168 860 cycles at 1x and 1 097 107 at 2x, i.e. a faster drive pauses
 * sooner. These two used to be the other way round. */
#define CDROM_PAUSE_IDLE_DELAY    7000      /* already idle/standby */
#define CDROM_PAUSE_1X_DELAY      2168860
#define CDROM_PAUSE_2X_DELAY      1097107

/* Read-speed change: the drive has to spin up or down before the next seek
 * completes (DOCS/cdromdrive.md:1896-1908). */
#define CDROM_SPEED_UP_DELAY      20321280  /* 1x -> 2x, 0.6 s */
#define CDROM_SPEED_DOWN_DELAY    23708160  /* 2x -> 1x, 0.7 s */

/* ReadTOC re-reads the whole lead-in, which takes far longer than an Init.
 * It was sharing CDROM_INIT_DELAY (121 ms) and delivering its INT2 about five
 * times too early — early enough that the BIOS had not armed its wait yet, so
 * the completion was lost and boot sat forever on a response already sent.
 * 180/4 sector times matches pcsx-redux's CdlReadToc (src/core/cdrom.cc:954). */
#define CDROM_READTOC_DELAY      (CDROM_SECTOR_TIME * 180u / 4u)  /* ~0.6 s */

/* Aliases for readability */
#define CDROM_READ_DELAY_1X      CDROM_SECTOR_TIME
#define CDROM_READ_DELAY_2X      CDROM_SECTOR_TIME_2X
/* How soon the drive re-checks when the reader has not produced the sector
 * yet. Short against a sector time (6.67 ms at 2x) so a late disc costs
 * latency on the CD data alone, and never stalls the emulation thread. */
#define CDROM_READ_RETRY_DELAY   (CDROM_SECTOR_TIME / 64u)   /* ~0.21 ms */

#define CDROM_MIN_INT_DELAY       1000

#define IRQ_CDROM 2

/* =========================================================================
 * Interrupt Types (INT1-5)
 * ========================================================================= */

typedef enum {
    CDROM_INT_NONE       = 0,
    CDROM_INT_DATA_READY = 1,   /* INT1 */
    CDROM_INT_COMPLETE   = 2,   /* INT2 */
    CDROM_INT_ACK        = 3,   /* INT3 */
    CDROM_INT_DATA_END   = 4,   /* INT4 */
    CDROM_INT_ERROR      = 5    /* INT5 */
} CdromInterrupt;

/* =========================================================================
 * Command Codes
 * ========================================================================= */

typedef enum {
    CDC_SYNC      = 0x00,
    CDC_GETSTAT   = 0x01,
    CDC_SETLOC    = 0x02,
    CDC_PLAY      = 0x03,
    CDC_FORWARD   = 0x04,
    CDC_BACKWARD  = 0x05,
    CDC_READN     = 0x06,
    CDC_MOTORON   = 0x07,
    CDC_STOP      = 0x08,
    CDC_PAUSE     = 0x09,
    CDC_INIT      = 0x0A,
    CDC_MUTE      = 0x0B,
    CDC_DEMUTE    = 0x0C,
    CDC_SETFILTER = 0x0D,
    CDC_SETMODE   = 0x0E,
    CDC_GETPARAM  = 0x0F,
    CDC_GETLOCL   = 0x10,
    CDC_GETLOCP   = 0x11,
    CDC_READT     = 0x12,
    CDC_GETTN     = 0x13,
    CDC_GETTD     = 0x14,
    CDC_SEEKL     = 0x15,
    CDC_SEEKP     = 0x16,
    CDC_SETCLOCK  = 0x17,
    CDC_GETCLOCK  = 0x18,
    CDC_TEST      = 0x19,
    CDC_GETID     = 0x1A,
    CDC_READS     = 0x1B,
    CDC_RESET     = 0x1C,
    CDC_GETQ      = 0x1D,
    CDC_READTOC   = 0x1E,
    CDC_VIDEOCD   = 0x1F,
    CDC_NONE      = 0xFF
} CdromCommand;

/* =========================================================================
 * Drive State
 * ========================================================================= */

typedef enum {
    DRIVE_IDLE,
    DRIVE_SPINUP,
    DRIVE_SEEKING,
    DRIVE_READING,
    DRIVE_PLAYING,
    DRIVE_PAUSING,
    DRIVE_STOPPING
} DriveState;

/* =========================================================================
 * Status Register Bits
 * ========================================================================= */

#define STAT_INDEX_MASK  0x03
#define STAT_ADPBUSY     0x04
#define STAT_PRMEMPT     0x08
#define STAT_PRMWRDY     0x10
#define STAT_RSLRRDY     0x20
#define STAT_DRQSTS      0x40
#define STAT_BUSYSTS     0x80

/* Secondary status byte flags */
#define STAT_BYTE_ERROR      0x01
#define STAT_BYTE_MOTOR_ON   0x02
#define STAT_BYTE_SEEK_ERROR 0x04
#define STAT_BYTE_ID_ERROR   0x08
#define STAT_BYTE_SHELL_OPEN 0x10
#define STAT_BYTE_READING    0x20
#define STAT_BYTE_SEEKING    0x40
#define STAT_BYTE_PLAYING    0x80

/* Error codes */
#define ERROR_INVALID_ARGUMENT  0x10
#define ERROR_WRONG_NUM_PARAMS  0x20
#define ERROR_INVALID_COMMAND   0x40
#define ERROR_NOT_READY         0x80

/* =========================================================================
 * FIFO
 * ========================================================================= */

typedef struct {
    uint8_t data[CDROM_RESPONSE_FIFO_SIZE];
    uint8_t head, tail, count;
} CdromFifo;

/* =========================================================================
 * Sector Buffer Ring
 * ========================================================================= */

typedef struct {
    uint8_t  raw[2352];     /* full raw sector */
    uint32_t data_start;    /* offset for data FIFO */
    uint32_t data_size;     /* exposed byte count */
    uint32_t position;      /* current read position (0..data_size) */
    uint32_t lba;
    bool     valid;
} SectorBuffer;

/* =========================================================================
 * Main CDROM Struct
 * ========================================================================= */

typedef struct Cdrom {
    struct Interconnect *inter;

    /* --- Register state --- */
    uint8_t index;
    uint8_t interrupt_enable;
    uint8_t interrupt_flag;

    /* --- Command state --- */
    CdromCommand pending_command;
    CdromCommand current_command;
    CdromCommand second_response_cmd;
    uint8_t      pending_params[16];
    uint8_t      pending_param_count;
    bool         cmd_event_pending;     /* guard: command event slot active */
    bool         second_event_pending;  /* guard: second_response event slot active */
    /* Cycle each of those two is actually due at. A pending interrupt blocks
     * delivery, and the controller does not restart its clock when the guest
     * finally acknowledges — it delivers what was already due. Without these,
     * the deferral path re-armed at CDROM_MIN_INT_DELAY and threw the real
     * deadline away, so every seek, spin-up and ReadTOC completed ~30 us after
     * its command instead of tens or hundreds of ms. */
    uint32_t     cmd_deadline;          /* cpu_cycle_counter the ack is due at */
    uint32_t     second_deadline;       /* cpu_cycle_counter the INT2/INT5 is due at */
    /* Same idea for the drive: the head reaches the next sector on disc time.
     * The INT1 acknowledge used to re-arm the drive event at one sector period
     * from the ack, which also overwrote a longer pending deadline — a ReadN
     * that had just charged 606 ms of spin-up delivered its first sector 6.6 ms
     * later, because the guest acknowledged the command's INT3 in between. */
    uint32_t     drive_deadline;        /* cpu_cycle_counter the next sector is due at */

    /* Second response data buffer (for async responses) */
    uint8_t second_response_data[16];
    uint8_t second_response_size;

    /* --- Drive state --- */
    DriveState drive_state;
    bool       motor_on;
    bool       disc_present;
    bool       shell_open;
    bool       read_after_seek;
    bool       play_after_seek;
    /* True from the moment a seek starts until the head has arrived — both the
     * explicit SeekL/SeekP kind and the implicit one at the start of
     * ReadN/ReadS/Play. GetlocL and Pause both refuse with error 80h during it
     * (cdromdrive.md:586-588, :896-901); the guest is expected to retry, which
     * is what the CD command churn measured against DuckStation looks like. */
    bool       seek_phase;
    char       disc_region;   /* 'A'/'E'/'I' from the disc's real licence string
                                  (cdrom_disc_detect_region), 0 if none loaded yet.
                                  GetID's SCEx response byte reflects this. */
    char       console_region;/* 'A'/'E'/'I'/'J', copied from the loaded BIOS.
                                  This is the drive's own region, reported to the
                                  BIOS by Test 19h,22h, and it decides which SCEx
                                  discs the machine will accept. Distinct from
                                  disc_region: a mismatch between the two is the
                                  region check, and on hardware it stops boot. */

    /* --- Position --- */
    uint32_t current_lba;
    /* Where the pickup physically sits: the last sector actually transferred.
     * current_lba runs ahead of it — it is advanced to the next sector to fetch
     * as soon as one is delivered, and jumped to the Setloc target before the
     * head has moved. Measuring seek distance from it made almost every seek
     * come out as zero distance and cost 0.9ms instead of ~20ms, which is most
     * of why the boot ran seconds ahead of the drive. */
    uint32_t head_lba;
    uint32_t target_lba;
    uint32_t setloc_lba;
    bool     setloc_pending;
    uint32_t current_subq_lba;
    SubQ     last_subq;

    /* CDDA fast-forward/rewind speed (1=normal, 2/4=ff/rew) */
    uint8_t cdda_speed;

    /* --- Mode --- */
    uint8_t mode;
    bool double_speed;
    bool xa_adpcm_enable;
    bool whole_sector;
    bool xa_filter_enable;
    uint8_t xa_filter_file;
    uint8_t xa_filter_channel;
    bool report_enable;
    bool auto_pause;
    bool cdda_enable;
    bool muted;
    /* ADPCTL bit0 (cdromdrive.md:249-255). Mutes XA-ADPCM specifically, where
     * the Mute command mutes both CD-DA and XA. Both are applied at the output
     * stage (cdrom_apply_output_volume). The audio FIFO carries one stream and
     * does not tag which sector a frame came from, so ADPMUTE is applied
     * whenever the drive is not playing CD-DA. */
    bool xa_mute;

    /* --- FIFOs --- */
    CdromFifo param_fifo;
    CdromFifo response_fifo;

    /* --- Sector buffer ring --- */
    SectorBuffer sector_buffers[CDROM_SECTOR_BUFFERS];
    uint8_t current_read_buffer;
    uint8_t current_write_buffer;
    /* Header + subheader of the newest sector the drive has processed, which is
     * what GetlocL answers with (DOCS/cdromdrive.md:871-880 — INT1 hands over
     * the oldest buffered sector, GetlocL reports the newest). Latched for every
     * sector including XA audio, which never enters the data ring above. */
    uint8_t last_header[8];
    bool    last_header_valid;
    bool    data_buffer_armed;     /* request register bit 7 */

    /* --- Disc & async reader --- */
    CdromDisc        disc;
    CdromAsyncReader async_reader;

    /* --- Audio --- */
    AudioFifo    audio_fifo;
    /* Sector pacing counters: the XA stream only stays in sync with the SPU if
     * audio sectors arrive at the rate their sample count implies. */
    uint32_t     pending_speed_change;  /* cycles owed for a Setmode speed change */
    uint32_t     sectors_read_total;
    uint32_t     xa_sectors_total;
    XaAdpcmState xa_adpcm_state;

    /* Volume matrix (L←CDL, L←CDR, R←CDL, R←CDR) — default 0x80 each.
     * _t = temp staging registers; committed to working regs on write3[idx3] bit5. */
    uint8_t vol_ll,   vol_lr,   vol_rl,   vol_rr;
    uint8_t vol_ll_t, vol_lr_t, vol_rl_t, vol_rr_t;

} Cdrom;

/* =========================================================================
 * Public API
 * ========================================================================= */

void    cdrom_init(Cdrom *cdrom, struct Interconnect *inter);
void    cdrom_reset(Cdrom *cdrom);
bool    cdrom_load_disc(Cdrom *cdrom, const char *cue_path);
void    cdrom_eject_disc(Cdrom *cdrom);

/* Event-scheduler tick handlers — installed into event_scheduler.c's
 * EVQ_CDROM_COMMAND / EVQ_CDROM_DRIVE / EVQ_CDROM_SECOND_RESPONSE slots. */
void    cdrom_command_event_tick(struct Interconnect *inter);
void    cdrom_drive_event_tick(struct Interconnect *inter);
void    cdrom_second_response_event_tick(struct Interconnect *inter);

uint8_t cdrom_read8(Cdrom *cdrom, uint32_t addr);
void    cdrom_write8(Cdrom *cdrom, uint32_t addr, uint8_t value);

/* Event-driven execution (called by interconnect event scheduler) */
void cdrom_execute_command(Cdrom *cdrom);
void cdrom_execute_drive(Cdrom *cdrom);
void cdrom_execute_second_response(Cdrom *cdrom);

bool cdrom_has_pending_command(Cdrom *cdrom);
bool cdrom_has_pending_interrupt(Cdrom *cdrom);

/* While the drive is reading and an interrupt is still unacknowledged:
 * process the sector under the head if it goes to the ADPCM decoder or is
 * discarded, and hold it if it is data (cdrom.c, cdrom_drive_event_tick). */
void cdrom_execute_drive_int_pending(Cdrom *cdrom);

/* Undo the consume of a READY cdrom_async_reader_poll() for `lba`, so the next
 * poll returns the same sector without another disc read: the drive looked at
 * the sector and has to hold it (data while an interrupt is pending). Lives in
 * cdrom_disc.c beside the reader. */
void cdrom_async_reader_unpoll(CdromAsyncReader *r, uint32_t lba);

/* ZS1_CD_XA_HOLD=1, read once: the drive waits for every acknowledge before
 * the next sector, XA included - the behaviour before the drive learned to
 * keep feeding the ADPCM decoder while an interrupt is pending. An A/B switch. */
bool cdrom_xa_hold_legacy(void);

/* Where one sector read with ReadN/ReadS goes (psx-spx cdr/cdromdrive.md:
 * 590-612). Pure: depends only on the sector bytes and the drive settings. */
typedef enum {
    CDROM_ROUTE_DATA,      /* INT1 + data FIFO, the GetlocL latch */
    CDROM_ROUTE_ADPCM,     /* to the XA-ADPCM decoder, no INT1 */
    CDROM_ROUTE_DISCARD    /* filtered audio: silently ignored */
} CdromSectorRoute;

CdromSectorRoute cdrom_route_sector(const uint8_t *raw, bool cdda_sector,
                                    bool adpcm_enable, bool filter_enable,
                                    uint8_t filter_file, uint8_t filter_channel);

/* HSTS.2 ADPBUSY, "1=playing XA-ADPCM" (cdromdrive.md:62). */
bool cdrom_xa_playing(const Cdrom *cdrom);

/* Audio frame for SPU/SDL (one stereo pair): popped and passed through
 * cdrom_apply_output_volume. */
void cdrom_get_audio_frame(Cdrom *cdrom, int16_t *left, int16_t *right);

/* The controller's output stage on one frame of the audio FIFO: Mute (both
 * sources), ADPMUTE (XA only) and the ATV0-ATV3 matrix. The SPU applies it to
 * every frame it takes from the FIFO. */
void cdrom_apply_output_volume(const Cdrom *cdrom, int16_t *left, int16_t *right);

/* DMA: read one 32-bit word from armed sector buffer (CDROM → RAM channel 3) */
uint32_t cdrom_dma_read_word(Cdrom *cdrom);

/* =========================================================================
 * FIFO Inline Functions
 * ========================================================================= */

static inline void fifo_init(CdromFifo *f) { f->head=f->tail=f->count=0; }
static inline void fifo_clear(CdromFifo *f) { f->head=f->tail=f->count=0; }
static inline bool fifo_is_empty(const CdromFifo *f) { return f->count==0; }
static inline bool fifo_is_full(const CdromFifo *f) { return f->count>=CDROM_RESPONSE_FIFO_SIZE; }

static inline void fifo_push(CdromFifo *f, uint8_t v) {
    if (f->count < CDROM_RESPONSE_FIFO_SIZE) {
        f->data[f->tail] = v;
        f->tail = (f->tail+1) % CDROM_RESPONSE_FIFO_SIZE;
        f->count++;
    }
}

static inline uint8_t fifo_pop(CdromFifo *f) {
    if (f->count > 0) {
        uint8_t v = f->data[f->head];
        f->head = (f->head+1) % CDROM_RESPONSE_FIFO_SIZE;
        f->count--;
        return v;
    }
    return 0;
}

static inline uint8_t fifo_peek(const CdromFifo *f, uint8_t i) {
    return (i < f->count) ? f->data[(f->head+i) % CDROM_RESPONSE_FIFO_SIZE] : 0;
}

#endif /* CDROM_H */
