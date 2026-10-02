/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * CDROM command handlers — first and second responses, drive loop.
 * Called from cdrom.c event callbacks.
 */

#include "cdrom.h"
#include "interconnect.h"
#include "log.h"
#include "lua_debug.h"
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Name tables
 * ========================================================================= */

static const char* cdrom_cmd_name(uint8_t cmd) {
    static const char* const names[32] = {
        "Sync","Getstat","Setloc","Play","Forward","Backward","ReadN","MotorOn",
        "Stop","Pause","Init","Mute","Demute","Setfilter","Setmode","Getparam",
        "GetlocL","GetlocP","ReadT","GetTN","GetTD","SeekL","SeekP","SetClock",
        "GetClock","Test","GetID","ReadS","Reset","GetQ","ReadTOC","VideoCD"
    };
    return (cmd < 32) ? names[cmd] : (cmd == 0xFF ? "None" : "Unknown");
}

static const char* cdrom_drive_state_name(int state) {
    switch (state) {
        case 0: return "IDLE";
        case 1: return "SPINUP";
        case 2: return "SEEKING";
        case 3: return "READING";
        case 4: return "PLAYING";
        case 5: return "PAUSING";
        case 6: return "STOPPING";
        default: return "UNKNOWN";
    }
}

/* =========================================================================
 * Internal helpers (shared with cdrom.c via extern)
 * ========================================================================= */

void cdrom_send_ack(Cdrom *cdrom);
void cdrom_send_complete(Cdrom *cdrom);
void cdrom_send_error(Cdrom *cdrom, uint8_t err, uint8_t reason);
void cdrom_push_response(Cdrom *cdrom, uint8_t v);
uint8_t cdrom_pop_param(Cdrom *cdrom);
uint8_t cdrom_get_stat_byte(Cdrom *cdrom);
void cdrom_schedule_command_event(Cdrom *cdrom, uint32_t cycles);
void cdrom_schedule_drive_event(Cdrom *cdrom, uint32_t cycles);
void cdrom_schedule_second_response_event(Cdrom *cdrom, uint32_t cycles);

/* Packed-BCD validity: both nibbles must be 0..9. Setloc and GetTD both reject
 * anything else (cdromdrive.md:627-628, :929-930). */
static inline bool cdrom_is_bcd(uint8_t v) { return (v & 0x0Fu) <= 9u && (v >> 4) <= 9u; }

/* =========================================================================
 * Begin helpers
 * ========================================================================= */

static void begin_reading(Cdrom *cdrom) {
    bool location_changed = false;
    uint32_t from_lba = cdrom->head_lba;
    if (cdrom->setloc_pending) {
        location_changed      = (cdrom->setloc_lba != cdrom->current_lba);
        cdrom->current_lba    = cdrom->setloc_lba;
        cdrom->setloc_pending = false;
    } else if (cdrom->drive_state == DRIVE_IDLE || cdrom->drive_state == DRIVE_PAUSING) {
        /* No Setloc pending and no read in progress: "If Reading was Paused,
         * then reading resumes at the most recently received sector (ie.
         * returning that sector once another time)" (psx-spx
         * cdr/cdromdrive.md:811-814). That is head_lba, the last sector
         * transferred. It used to resume one past it. After a SeekL the two
         * are the same sector, so a Setloc/SeekL/ReadN sequence is unchanged.
         * A Read while reading "just continues reading" (:811-812). */
        cdrom->current_lba = cdrom->head_lba;
    }
    LOG_CDROM_DEBUG("[CDROM] Drive state: %s -> READING (LBA %u)",
                    cdrom_drive_state_name(cdrom->drive_state), cdrom->current_lba);
    /* A read that starts somewhere new, or that starts at all from a stopped
     * drive, carries an implicit seek; a Read that merely continues an ongoing
     * one does not (cdromdrive.md:588, :809-814). */
    if (location_changed || cdrom->drive_state != DRIVE_READING)
        cdrom->seek_phase = true;
    cdrom->drive_state = DRIVE_READING;
    cdrom_async_reader_queue(&cdrom->async_reader, cdrom->current_lba);
    /* Location changed: the head has to get there before the first sector. That
     * is a seek, so it costs what a seek of that distance costs — a flat 30
     * sectors (400ms) regardless of distance both overcharged short moves and
     * undercharged long ones, and stacked on top of the SeekL the game had
     * usually already issued. */
    uint32_t base = cdrom->double_speed ? CDROM_READ_DELAY_2X : CDROM_READ_DELAY_1X;
    uint32_t delay = location_changed
        ? base + cdrom_disc_get_seek_ticks(from_lba, cdrom->current_lba)
        : base;
    /* A speed change owed by Setmode is paid here too, not only on a seek. The
     * boot sequence is Setloc, SeekL, Setmode 0x80, ReadN — the Setmode lands
     * after the seek, so charging it only in begin_seeking() let the first read
     * skip the whole spin-up and deliver its sector ~600ms early. */
    uint32_t speed = cdrom->pending_speed_change;
    delay += speed;
    cdrom->pending_speed_change = 0;
    LOG_CDROM_DEBUG("[CDROM] Read start LBA %u: %u (%.3f ms)%s%s",
                    cdrom->current_lba, delay, delay / 33868.8,
                    location_changed ? " (location changed)" : "",
                    speed ? " (incl. speed change)" : "");
    cdrom_schedule_drive_event(cdrom, delay);
}

static void begin_playing(Cdrom *cdrom, uint8_t track_param) {
    bool location_changed = false;
    if (track_param > 0 && cdrom->disc.last_track > 0) {
        uint8_t t = track_param;
        if (t > cdrom->disc.last_track) t = cdrom->disc.last_track;
        location_changed      = (cdrom->disc.tracks[t].start_lba != cdrom->current_lba);
        cdrom->current_lba    = cdrom->disc.tracks[t].start_lba;
        cdrom->setloc_pending = false;
    } else if (cdrom->setloc_pending) {
        location_changed      = (cdrom->setloc_lba != cdrom->current_lba);
        cdrom->current_lba    = cdrom->setloc_lba;
        cdrom->setloc_pending = false;
    }
    LOG_CDROM_DEBUG("[CDROM] Drive state: %s -> PLAYING (LBA %u)",
                    cdrom_drive_state_name(cdrom->drive_state), cdrom->current_lba);
    if (location_changed || cdrom->drive_state != DRIVE_PLAYING)
        cdrom->seek_phase = true;   /* implicit seek, same rule as begin_reading */
    cdrom->drive_state = DRIVE_PLAYING;
    cdrom->cdda_speed  = 1;
    cdrom_async_reader_queue(&cdrom->async_reader, cdrom->current_lba);
    uint32_t base = cdrom->double_speed ? CDROM_READ_DELAY_2X : CDROM_READ_DELAY_1X;
    uint32_t delay = location_changed ? CDROM_SEEK_CHANGE_DELAY : base;
    delay += cdrom->pending_speed_change;   /* same reason as begin_reading() */
    cdrom->pending_speed_change = 0;
    cdrom_schedule_drive_event(cdrom, delay);
}

static void begin_seeking(Cdrom *cdrom, bool read_after, bool play_after) {
    cdrom->read_after_seek = read_after;
    cdrom->play_after_seek = play_after;
    if (cdrom->setloc_pending) {
        cdrom->target_lba     = cdrom->setloc_lba;
        cdrom->setloc_pending = false;
    }
    LOG_CDROM_DEBUG("[CDROM] Drive state: %s -> SEEKING (LBA %u -> %u)",
                    cdrom_drive_state_name(cdrom->drive_state), cdrom->head_lba, cdrom->target_lba);
    cdrom->drive_state = DRIVE_SEEKING;
    cdrom->seek_phase  = true;
    /* Distance from where the head is, not from the next sector queued. */
    uint32_t delay = (cdrom->target_lba == cdrom->head_lba)
        ? CDROM_SEEK_FAST_DELAY
        : cdrom_disc_get_seek_ticks(cdrom->head_lba, cdrom->target_lba);
    uint32_t speed = cdrom->pending_speed_change;   /* see Setmode */
    delay += speed;
    cdrom->pending_speed_change = 0;
    LOG_CDROM_DEBUG("[CDROM] Seek time %u -> %u (%d LBA): %u (%.3f ms)%s",
                    cdrom->head_lba, cdrom->target_lba,
                    (int)cdrom->target_lba - (int)cdrom->head_lba,
                    delay, delay / 33868.8,
                    speed ? " (incl. speed change)" : "");
    cdrom_schedule_second_response_event(cdrom, delay);
}

/* =========================================================================
 * First Response (cdrom_execute_command)
 * ========================================================================= */

void cdrom_execute_command(Cdrom *cdrom) {
    CdromCommand cmd = cdrom->pending_command;
    cdrom->pending_command = CDC_NONE;
    cdrom->current_command = cmd;

    /* restore parameters */
    fifo_clear(&cdrom->param_fifo);
    for (int i = 0; i < cdrom->pending_param_count; i++)
        fifo_push(&cdrom->param_fifo, cdrom->pending_params[i]);

    LOG_CDROM_DEBUG("[CDROM] Execute command %s (0x%02X)", cdrom_cmd_name((uint8_t)cmd), (uint8_t)cmd);
    fifo_clear(&cdrom->response_fifo);

    /* Commands that need a disc: return NOT_READY immediately if none present */
    if (!cdrom->disc_present) {
        switch (cmd) {
            case CDC_SETLOC:
            case CDC_PLAY:
            case CDC_READN: case CDC_READS:
            case CDC_SEEKL: case CDC_SEEKP:
                cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, 0x80);
                return;
            default: break;
        }
    }

    switch (cmd) {

    /* --- 0x00 Sync — not a command. The opcode table lists it as unused, and
     * the description says outright that it "returns error code 40h = Invalid
     * Command" (cdromdrive.md:380, :501-504). It used to answer INT3(stat). --- */
    case CDC_SYNC:
        cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, ERROR_INVALID_COMMAND);
        break;

    /* --- 0x01 Getstat --- */
    case CDC_GETSTAT:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x02 Setloc --- */
    case CDC_SETLOC: {
        uint8_t mm = cdrom_pop_param(cdrom);
        uint8_t ss = cdrom_pop_param(cdrom);
        uint8_t ff = cdrom_pop_param(cdrom);
        /* All three parameters are packed BCD, with ss < 60h and ff < 75h;
         * anything else answers INT5(stat,10h) and leaves the target alone
         * (cdromdrive.md:627-628). Nothing validated them before, so a garbage
         * Setloc silently seeked somewhere nonsensical instead of being refused,
         * and the guest never learned its parameters were wrong. */
        if (!cdrom_is_bcd(mm) || !cdrom_is_bcd(ss) || !cdrom_is_bcd(ff) ||
            ss >= 0x60 || ff >= 0x75) {
            LOG_CDROM_WARN("[CDROM] Setloc rejected: %02x:%02x:%02x not valid BCD msf", mm, ss, ff);
            cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR,
                             ERROR_INVALID_ARGUMENT);
            break;
        }
        uint8_t m  = cdrom_from_bcd(mm);
        uint8_t s  = cdrom_from_bcd(ss);
        uint8_t f  = cdrom_from_bcd(ff);
        cdrom->setloc_lba    = (uint32_t)((m*60 + s)*75 + f) - 150;
        cdrom->setloc_pending = true;
        LOG_CDROM_DEBUG("[CDROM] Setloc %02u:%02u:%02u -> LBA %u", m, s, f, cdrom->setloc_lba);
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;
    }

    /* --- 0x03 Play --- */
    case CDC_PLAY: {
        uint8_t track_param = cdrom_pop_param(cdrom);
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        begin_playing(cdrom, track_param);
        break;
    }

    /* --- 0x04 Forward --- */
    case CDC_FORWARD:
        if (cdrom->drive_state == DRIVE_PLAYING) {
            cdrom->cdda_speed = (cdrom->cdda_speed < 4) ? cdrom->cdda_speed * 2 : 4;
        }
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x05 Backward --- */
    case CDC_BACKWARD:
        if (cdrom->drive_state == DRIVE_PLAYING) {
            if (cdrom->current_lba > 4) cdrom->current_lba -= 4;
            else cdrom->current_lba = 0;
            cdrom_async_reader_queue(&cdrom->async_reader, cdrom->current_lba);
        }
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x06 ReadN / 0x1B ReadS --- */
    case CDC_READN:
    case CDC_READS:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        begin_reading(cdrom);
        break;

    /* --- 0x07 MotorOn --- */
    case CDC_MOTORON:
        cdrom->motor_on = true;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        cdrom->second_response_cmd = CDC_MOTORON;
        cdrom_schedule_second_response_event(cdrom, CDROM_SPINUP_DELAY);
        break;

    /* --- 0x08 Stop --- */
    case CDC_STOP: {
        uint32_t stop_delay = CDROM_STOP_IDLE_DELAY;
        if (cdrom->motor_on && cdrom->drive_state != DRIVE_IDLE)
            stop_delay = cdrom->double_speed ? CDROM_STOP_2X_DELAY : CDROM_STOP_1X_DELAY;
        cdrom->drive_state = DRIVE_STOPPING;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        cdrom->second_response_cmd = CDC_STOP;
        cdrom_schedule_second_response_event(cdrom, stop_delay);
        break;
    }

    /* --- 0x09 Pause --- */
    case CDC_PAUSE: {
        /* Pause fails with INT5(stat,80h) during a seek phase — the explicit
         * SeekL/SeekP kind and the implicit one at the start of
         * ReadN/ReadS/Play alike (cdromdrive.md:586-588). */
        if (cdrom->seek_phase) {
            LOG_CDROM_DEBUG("[CDROM] Pause refused: seek in progress");
            cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, ERROR_NOT_READY);
            break;
        }
        /* The first response carries the status as it was — still with bit5 set
         * if a read was running; only the second response has it cleared
         * (cdromdrive.md:583-585). The state change therefore happens after the
         * status byte has been taken, not before it. */
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        /* pcsx-redux hardware-tested: 7ms if already idle, else 1s/2s by speed */
        uint32_t pause_delay;
        if (cdrom->drive_state == DRIVE_IDLE) {
            pause_delay = CDROM_PAUSE_IDLE_DELAY;
        } else {
            pause_delay = cdrom->double_speed ? CDROM_PAUSE_2X_DELAY : CDROM_PAUSE_1X_DELAY;
            if (cdrom->drive_state == DRIVE_READING || cdrom->drive_state == DRIVE_PLAYING)
                cdrom->drive_state = DRIVE_PAUSING;
        }
        cdrom->second_response_cmd = CDC_PAUSE;
        cdrom_schedule_second_response_event(cdrom, pause_delay);
        break;
    }

    /* --- 0x0A Init --- */
    case CDC_INIT: {
        /* The drive takes ~740 ms over an Init (see CDROM_INIT_DELAY), which is
         * longer than the ~415 ms after which the BIOS re-issues the command.
         * A retry must not restart the work: rescheduling on each one pushed the
         * deadline forward for as long as the BIOS kept asking, so the reply
         * never came and boot sat in an endless Init loop.
         *
         * And the retry gets *no answer at all*: "If an Init command is already
         * in progress (its second response is still pending), a new Init command
         * is silently dropped with no response (neither INT3 nor INT5)"
         * (cdromdrive.md:538-540). We used to acknowledge it with INT3, which is
         * an interrupt the drive never sends. */
        bool init_already_owed = cdrom->second_event_pending &&
                                 cdrom->second_response_cmd == CDC_INIT &&
                                 cdrom->inter &&
                                 (int32_t)(cdrom->second_deadline -
                                           cdrom->inter->cpu_cycle_counter) > 0;
        if (init_already_owed) {
            LOG_CDROM_DEBUG("[CDROM] Init dropped: one is already owed");
            break;
        }
        /* "Multiple effects at once. Sets mode=20h, activates drive motor,
         * Standby, abort all commands" (cdromdrive.md:536-537). None of that
         * happened here: the mode kept whatever the last Setmode left — the
         * game's own Setmode then arrived 50 fields later, so an Init issued
         * while the drive was at double speed left it there — and an ongoing
         * read carried on straight through a command whose whole purpose is to
         * stop everything. */
        cdrom->mode            = 0x20;
        cdrom->double_speed    = false;
        cdrom->xa_adpcm_enable = false;
        cdrom->whole_sector    = true;
        cdrom->xa_filter_enable= false;
        cdrom->report_enable   = false;
        cdrom->auto_pause      = false;
        cdrom->cdda_enable     = false;
        if (cdrom->drive_state == DRIVE_READING ||
            cdrom->drive_state == DRIVE_PLAYING ||
            cdrom->drive_state == DRIVE_SEEKING) {
            cdrom->drive_state = DRIVE_IDLE;    /* Standby: motor on, head parked */
            cdrom->seek_phase  = false;
        }
        cdrom->read_after_seek = false;
        cdrom->play_after_seek = false;
        cdrom->motor_on        = true;

        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        cdrom->second_response_cmd = CDC_INIT;
        uint32_t delay = CDROM_INIT_DELAY;
        /* Back to single speed, and the head re-homes to the start. */
        if (cdrom->double_speed) delay += CDROM_SPEED_DOWN_DELAY;
        if (cdrom->head_lba != 0)
            delay += cdrom_disc_get_seek_ticks(cdrom->head_lba, 0);
        cdrom_schedule_second_response_event(cdrom, delay);
        break;
    }

    /* --- 0x0B Mute --- */
    case CDC_MUTE:
        cdrom->muted = true;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x0C Demute --- */
    case CDC_DEMUTE:
        cdrom->muted = false;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x0D Setfilter --- */
    case CDC_SETFILTER:
        cdrom->xa_filter_file    = cdrom_pop_param(cdrom);
        cdrom->xa_filter_channel = cdrom_pop_param(cdrom);
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x0E Setmode --- */
    case CDC_SETMODE: {
        uint8_t m = cdrom_pop_param(cdrom);
        bool new_double_speed = (m & 0x80) != 0;
        /* Changing the read speed spins the drive up or down; hardware needs
         * about 0.6 s to go 1x->2x and 0.7 s the other way (DOCS/cdromdrive.md:
         * 1896-1908). Setmode itself only answers INT3, so the cost is carried
         * and charged to the next seek. */
        if (new_double_speed != cdrom->double_speed)
            cdrom->pending_speed_change = new_double_speed
                ? CDROM_SPEED_UP_DELAY : CDROM_SPEED_DOWN_DELAY;
        cdrom->mode           = m;
        cdrom->double_speed   = new_double_speed;
        cdrom->xa_adpcm_enable= (m & 0x40) != 0;
        cdrom->whole_sector   = (m & 0x20) != 0;
        cdrom->xa_filter_enable= (m & 0x08) != 0;
        cdrom->report_enable  = (m & 0x04) != 0;
        cdrom->auto_pause     = (m & 0x02) != 0;
        cdrom->cdda_enable    = (m & 0x01) != 0;
        /* The trailing "@ 0x%08x" here had no argument and printed whatever
         * was next on the stack — four format specifiers, three values. */
        LOG_CDROM_DEBUG("[CDROM] Setmode 0x%02X (2x=%d whole=%d xa=%d)",
                        m, cdrom->double_speed, cdrom->whole_sector, cdrom->xa_adpcm_enable);
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;
    }

    /* --- 0x0F Getparam --- */
    case CDC_GETPARAM:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_push_response(cdrom, cdrom->mode);
        cdrom_push_response(cdrom, 0x00);  /* reserved */
        cdrom_push_response(cdrom, cdrom->xa_filter_file);
        cdrom_push_response(cdrom, cdrom->xa_filter_channel);
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x10 GetlocL --- */
    case CDC_GETLOCL: {
        /* Header (MM:SS:FF:mode) + subheader (file,ch,submode,coding) of the
         * newest sector processed — see Cdrom.last_header.
         *
         * Three documented failures, all error 80h: the drive is not spun up
         * (:396), the drive is in the seek phase (:896-901 — during a seek only
         * subchannel position is decoded, so there is no header to report, and
         * the guest is expected to retry until the seek finishes), and the head
         * is on an audio track, which has no header at all (:892-895). */
        if (cdrom->seek_phase) {
            LOG_CDROM_DEBUG("[CDROM] GetlocL refused: seek in progress");
            cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, ERROR_NOT_READY);
            break;
        }
        if (cdrom->disc_present && cdrom->disc.last_track) {
            uint8_t trk = cdrom_disc_get_track_at_lba(&cdrom->disc, cdrom->head_lba);
            if (trk >= cdrom->disc.first_track && trk <= cdrom->disc.last_track &&
                cdrom->disc.tracks[trk].is_audio) {
                LOG_CDROM_DEBUG("[CDROM] GetlocL refused: track %u is audio", trk);
                cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, ERROR_NOT_READY);
                break;
            }
        }
        if (cdrom->last_header_valid && cdrom->motor_on) {
            for (int i = 0; i < 8; i++)
                cdrom_push_response(cdrom, cdrom->last_header[i]);
            /* The answer itself, because a wrong-but-valid location sends the
             * game back to Setloc/SeekL instead of stopping it dead: msf is
             * BCD as it comes off the disc, then mode, file, channel, submode,
             * coding info. */
            LOG_CDROM_DEBUG("[CDROM] GetlocL -> %02x:%02x:%02x mode=%02x file=%02x ch=%02x sm=%02x ci=%02x",
                            cdrom->last_header[0], cdrom->last_header[1], cdrom->last_header[2],
                            cdrom->last_header[3], cdrom->last_header[4], cdrom->last_header[5],
                            cdrom->last_header[6], cdrom->last_header[7]);
        } else {
            cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, 0x80);
            break;
        }
        cdrom_send_ack(cdrom);
        break;
    }

    /* --- 0x11 GetlocP --- */
    case CDC_GETLOCP: {
        SubQ *q = &cdrom->last_subq;
        /* Printed in the same shape a DuckStation Devel run prints it, so the
         * two logs can be put side by side without reformatting either. */
        LOG_CDROM_DEBUG("[CDROM] GetlocP T%02X I%02X R[%02X:%02X:%02X] A[%02X:%02X:%02X] (lba %u)",
                        q->track_bcd, q->index_bcd,
                        q->rel_mm_bcd, q->rel_ss_bcd, q->rel_ff_bcd,
                        q->abs_mm_bcd, q->abs_ss_bcd, q->abs_ff_bcd,
                        cdrom->current_lba);
        cdrom_push_response(cdrom, q->track_bcd);
        cdrom_push_response(cdrom, q->index_bcd);
        cdrom_push_response(cdrom, q->rel_mm_bcd);
        cdrom_push_response(cdrom, q->rel_ss_bcd);
        cdrom_push_response(cdrom, q->rel_ff_bcd);
        cdrom_push_response(cdrom, q->abs_mm_bcd);
        cdrom_push_response(cdrom, q->abs_ss_bcd);
        cdrom_push_response(cdrom, q->abs_ff_bcd);
        cdrom_send_ack(cdrom);
        break;
    }

    /* --- 0x12 ReadT (session info) --- */
    case CDC_READT:
        (void)cdrom_pop_param(cdrom);  /* session number, ignored */
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        /* second response: session 1 starts at 00:02:00 */
        cdrom->second_response_data[0] = cdrom_get_stat_byte(cdrom);
        cdrom->second_response_data[1] = 0x00;  /* complete flag */
        cdrom->second_response_data[2] = 0x02;  /* start MM */
        cdrom->second_response_data[3] = 0x00;
        cdrom->second_response_size = 4;
        cdrom->second_response_cmd  = CDC_READT;
        cdrom_schedule_second_response_event(cdrom, CDROM_ACK_DELAY);
        break;

    /* --- 0x13 GetTN --- */
    case CDC_GETTN:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_push_response(cdrom, cdrom_to_bcd(cdrom->disc.first_track ? cdrom->disc.first_track : 1));
        cdrom_push_response(cdrom, cdrom_to_bcd(cdrom->disc.last_track  ? cdrom->disc.last_track  : 1));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x14 GetTD --- */
    case CDC_GETTD: {
        /* The track parameter is packed BCD; non-BCD values and values above the
         * last track both answer INT5(stat,10h) (cdromdrive.md:926-930). Reading
         * it as binary put every track from 10 upwards on the wrong LBA. */
        uint8_t param = cdrom_pop_param(cdrom);
        if (!cdrom_is_bcd(param)) {
            cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR,
                             ERROR_INVALID_ARGUMENT);
            break;
        }
        uint8_t tnum = cdrom_from_bcd(param);
        if (tnum > cdrom->disc.last_track) {
            cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR,
                             ERROR_INVALID_ARGUMENT);
            break;
        }
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        uint32_t lba = 0;
        if (tnum == 0) {
            lba = cdrom->disc.total_sectors; /* lead-out */
        } else if (tnum >= cdrom->disc.first_track && tnum <= cdrom->disc.last_track) {
            lba = cdrom->disc.tracks[tnum].start_lba;
        }
        uint8_t mm, ss, ff;
        cdrom_lba_to_msf(lba, &mm, &ss, &ff);
        cdrom_push_response(cdrom, cdrom_to_bcd(mm));
        cdrom_push_response(cdrom, cdrom_to_bcd(ss));
        cdrom_send_ack(cdrom);
        break;
    }

    /* --- 0x15 SeekL / 0x16 SeekP --- */
    case CDC_SEEKL:
    case CDC_SEEKP:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        cdrom->second_response_cmd = cmd;
        begin_seeking(cdrom, false, false);
        break;

    /* --- 0x17/0x18 — unused opcodes, INT5(11h,40h) (cdromdrive.md:403).
     * There is no SetClock/GetClock on this hardware; the names are ours. --- */
    case CDC_SETCLOCK:
    case CDC_GETCLOCK:
        cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, ERROR_INVALID_COMMAND);
        break;

    /* --- 0x19 Test --- */
    case CDC_TEST: {
        uint8_t sub = cdrom_pop_param(cdrom);
        switch (sub) {
        case 0x20:
            /* HC05 controller firmware date/version, BCD (DOCS/cdromdrive.md:1136-1155).
             * 95h,05h,16h,C1h is the LATE-PU-8, 16 May 1995.
             *
             * What this value is decides which boot path the BIOS takes, so it is
             * not cosmetic. It was 94h,09h,19h,C0h — a PU-7 from Sep 1994 — and
             * DOCS:1170 says vC0 cannot answer Test 19h,22h at all, so the BIOS
             * never asked this machine what region it was and the region gate
             * simply never ran. That is why a PAL disc booted regardless of what
             * the drive claimed.
             *
             * vC2 (PU-18, 97h,01h,10h) is the generation that actually shipped
             * the SCPH-7002/7502 BIOS, but it sends the BIOS down a ReadTOC-based
             * identification path that stops here: the command is a stub that
             * never builds a TOC, and boot waits on it forever. vC1 is the oldest
             * version that supports the region string (DOCS:1170) and keeps the
             * boot path we do serve. Moving to vC2 means implementing ReadTOC. */
            cdrom_push_response(cdrom, 0x95);
            cdrom_push_response(cdrom, 0x05);
            cdrom_push_response(cdrom, 0x16);
            cdrom_push_response(cdrom, 0xC1);
            cdrom_send_ack(cdrom);
            break;
        case 0x04:  /* reset SCEx, force motor on */
            cdrom->motor_on = true;
            cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
            cdrom_send_ack(cdrom);
            break;
        case 0x05:  /* SCEx counters */
            cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
            cdrom_push_response(cdrom, 0x00);
            cdrom_push_response(cdrom, 0x00);
            cdrom_send_ack(cdrom);
            break;
        case 0x22: {
            /* Region ID string (DOCS/cdromdrive.md:1168-1181). This is the
             * drive's own region — it decides which SCEx discs the machine
             * accepts — so it has to follow the BIOS that is running, not the
             * disc that happens to be in the tray. It was hardcoded to "for U/C"
             * (North America), which no BIOS ever read because we also reported
             * a vC0 controller, and vC0 cannot answer this at all. */
            const char *s;
            switch (cdrom->console_region) {
                case 'E': s = "for Europe"; break;
                case 'I':
                case 'J': s = "for Japan";  break;
                default:  s = "for U/C";    break;   /* 'A', North America */
            }
            for (const char *p = s; *p; p++)
                cdrom_push_response(cdrom, (uint8_t)*p);
            cdrom_send_ack(cdrom);
            break;
        }
        default:
            cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
            cdrom_send_ack(cdrom);
            break;
        }
        break;
    }

    /* --- 0x1A GetID --- */
    case CDC_GETID:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        cdrom->second_response_cmd = CDC_GETID;
        cdrom_schedule_second_response_event(cdrom, CDROM_ID_READ_DELAY);
        break;

    /* --- 0x1C Reset — INT3 only. There is no completion interrupt: software
     * must wait 1/8 s (400000h cycles) by itself before sending anything else
     * (cdromdrive.md:542-551). We used to send an INT2 nothing asks for. --- */
    case CDC_RESET:
        cdrom->drive_state = DRIVE_IDLE;
        cdrom->mode        = 0;
        cdrom->motor_on    = cdrom->disc_present;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        break;

    /* --- 0x1D GetQ --- */
    case CDC_GETQ: {
        /* Returns last SubQ data (10 bytes) */
        SubQ *q = &cdrom->last_subq;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_push_response(cdrom, q->control_adr);
        cdrom_push_response(cdrom, q->track_bcd);
        cdrom_push_response(cdrom, q->index_bcd);
        cdrom_push_response(cdrom, q->rel_mm_bcd);
        cdrom_push_response(cdrom, q->rel_ss_bcd);
        cdrom_push_response(cdrom, q->rel_ff_bcd);
        cdrom_push_response(cdrom, q->reserved);
        cdrom_push_response(cdrom, q->abs_mm_bcd);
        cdrom_push_response(cdrom, q->abs_ss_bcd);
        cdrom_push_response(cdrom, q->abs_ff_bcd);
        cdrom_send_ack(cdrom);
        break;
    }

    /* --- 0x1E ReadTOC --- */
    case CDC_READTOC:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_ack(cdrom);
        cdrom->second_response_cmd = CDC_READTOC;
        cdrom_schedule_second_response_event(cdrom, CDROM_READTOC_DELAY);
        break;

    /* --- 0x1F VideoCD (not emulated) --- */
    case CDC_VIDEOCD:
        cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, ERROR_INVALID_COMMAND);
        break;

    default:
        LOG_CDROM_WARN("[CDROM] Unknown cmd 0x%02X (drive: %s)", (unsigned)cmd, cdrom_drive_state_name(cdrom->drive_state));
        cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, ERROR_INVALID_COMMAND);
        break;
    }

    fifo_clear(&cdrom->param_fifo);
    cdrom->current_command = CDC_NONE;
}

/* =========================================================================
 * Second Response
 * ========================================================================= */

void cdrom_execute_second_response(Cdrom *cdrom) {
    CdromCommand cmd = cdrom->second_response_cmd;
    cdrom->second_response_cmd = CDC_NONE;
    fifo_clear(&cdrom->response_fifo);
    LOG_CDROM_DEBUG("[CDROM] Second response: %s (0x%02X)", cdrom_cmd_name((uint8_t)cmd), (uint8_t)cmd);

    switch (cmd) {

    case CDC_GETID:
        LOG_CDROM_DEBUG("[CDROM] GetID second: disc_present=%d motor_on=%d shell_open=%d region='%c'",
                        cdrom->disc_present, cdrom->motor_on, cdrom->shell_open,
                        cdrom->disc_region ? cdrom->disc_region : '?');
        if (!cdrom->disc_present) {
            cdrom_send_error(cdrom, 0x08, 0x40);
        } else {
            cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
            cdrom_push_response(cdrom, 0x00);  /* licensed */
            cdrom_push_response(cdrom, 0x20);  /* mode 2 */
            cdrom_push_response(cdrom, 0x00);
            cdrom_push_response(cdrom, 'S');
            cdrom_push_response(cdrom, 'C');
            cdrom_push_response(cdrom, 'E');
            /* Reflects the disc's real licence region (cdrom_disc_detect_region),
             * not hardcoded — see DOCS/cdromdrive.md, GetID's 4th SCEx byte. */
            cdrom_push_response(cdrom, cdrom->disc_region ? cdrom->disc_region : 'A');
            cdrom_send_complete(cdrom);
        }
        break;

    case CDC_INIT:
        cdrom->motor_on    = cdrom->disc_present;
        cdrom->drive_state = DRIVE_IDLE;
        cdrom->seek_phase  = false;
        /* "Sets mode=20h, activates drive motor, Standby, abort all commands"
         * (DOCS/cdromdrive.md:535-537). Setting the register without deriving
         * the flags from it left double_speed set, so the Setmode 0x80 the
         * BIOS sends next was a no-op and never charged its 1x->2x spin-up —
         * the boot then ran most of a second ahead of the drive. */
        cdrom->mode             = 0x20;
        cdrom->double_speed     = false;
        cdrom->xa_adpcm_enable  = false;
        cdrom->whole_sector     = true;   /* bit 5 */
        cdrom->xa_filter_enable = false;
        cdrom->report_enable    = false;
        cdrom->auto_pause       = false;
        cdrom->cdda_enable      = false;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_complete(cdrom);
        break;

    case CDC_STOP:
        cdrom->motor_on    = false;
        cdrom->drive_state = DRIVE_IDLE;
        cdrom->seek_phase  = false;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_complete(cdrom);
        break;

    case CDC_PAUSE:
        cdrom->drive_state = DRIVE_IDLE;
        cdrom->seek_phase  = false;
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_complete(cdrom);
        break;

    case CDC_MOTORON:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_complete(cdrom);
        break;

    case CDC_SEEKL:
    case CDC_SEEKP:
        cdrom->current_lba = cdrom->target_lba;
        cdrom->head_lba    = cdrom->target_lba;   /* the head has arrived */
        cdrom->seek_phase  = false;               /* …so GetlocL/Pause work again */
        cdrom->drive_state = DRIVE_IDLE;
        /* update SubQ for new position */
        cdrom->last_subq = cdrom_disc_get_subq(&cdrom->disc, cdrom->current_lba);
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_complete(cdrom);
        if (cdrom->read_after_seek) {
            cdrom->read_after_seek = false;
            begin_reading(cdrom);
        } else if (cdrom->play_after_seek) {
            cdrom->play_after_seek = false;
            begin_playing(cdrom, 0);
        }
        break;

    case CDC_READT:
        for (int i = 0; i < cdrom->second_response_size; i++)
            cdrom_push_response(cdrom, cdrom->second_response_data[i]);
        cdrom_send_complete(cdrom);
        break;

    case CDC_READTOC:
        cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
        cdrom_send_complete(cdrom);
        break;

    default:
        break;
    }
}

/* =========================================================================
 * Drive Loop (sector reading / CDDA playing)
 * ========================================================================= */

/* ZS1_CD_XA_HOLD=1: the drive waits for every acknowledge, XA included (see
 * cdrom_drive_event_tick). Read once. */
bool cdrom_xa_hold_legacy(void) {
    static int s_hold = -1;
    if (s_hold < 0) {
        const char *env = getenv("ZS1_CD_XA_HOLD");
        s_hold = (env && env[0] == '1') ? 1 : 0;
        if (s_hold)
            LOG_CDROM_INFO("[CDROM] ZS1_CD_XA_HOLD=1: XA sectors wait for interrupt acknowledges");
    }
    return s_hold == 1;
}

/* Where a sector read by ReadN/ReadS goes. "The PSX CDROM BIOS is first trying
 * to send sectors to the ADPCM decoder, and, if that didn't work out, then it's
 * trying to send them to the main CPU (and if that didn't work out either, then
 * it's silently ignoring the sector)" (psx-spx cdr/cdromdrive.md:590-593):
 *
 *   try_deliver_as_adpcm_sector:                                   (:595-601)
 *    reject if CD-DA AUDIO format
 *    reject if sector isn't MODE2 format
 *    reject if adpcm_disabled(setmode.6)
 *    reject if filter_enabled(setmode.3) AND selected file/channel doesn't match
 *    reject if submode isn't audio+realtime (bit2 and bit6 must be both set)
 *   try_deliver_as_data_sector:                                    (:602-608)
 *    reject if filter_enabled(setmode.3) AND submode is audio+realtime
 *    1st delivery attempt: send INT1+data, unless there's another INT pending
 *
 * The code this replaces skipped the first two ADPCM checks, so a Mode 1 sector
 * whose bytes 16-19 happened to look like an audio subheader went to the
 * decoder; and it had no DISCARD at all. Every audio sector of another
 * file/channel became an INT1 with ADPCM in the data FIFO and moved the GetlocL
 * location onto a channel the game was not playing. An FMV, with one audio
 * channel and a demuxer that skips sectors without a video header, never
 * noticed; an XA bank of dialogue, 8 to 32 channels in one file
 * (cdr/cdromformat.md:698), got N-1 spurious INT1s in N.
 *
 * The subheader exists only on a Mode 2 sector (bytes 10h-13h, cdr/
 * cdromformat.md:486-503; a Mode 1 sector has user data there, :477-485), so
 * "audio+realtime" (submode bits 2 and 6, :643-653) is read from byte 18 only
 * there; a CD-DA or Mode 1 sector never counts as audio.
 * The second, file/channel-checked data delivery attempt (:606-612) is not
 * modelled, as before. */
CdromSectorRoute cdrom_route_sector(const uint8_t *raw, bool cdda_sector,
                                    bool adpcm_enable, bool filter_enable,
                                    uint8_t filter_file, uint8_t filter_channel) {
    bool mode2    = !cdda_sector && raw[15] == 2;
    bool audio_rt = mode2 && (raw[18] & 0x44) == 0x44;
    bool fc_match = raw[16] == filter_file && raw[17] == filter_channel;

    if (mode2 && adpcm_enable && (!filter_enable || fc_match) && audio_rt)
        return CDROM_ROUTE_ADPCM;
    if (filter_enable && audio_rt)
        return CDROM_ROUTE_DISCARD;
    return CDROM_ROUTE_DATA;
}

/* Whether `lba` lies on a CD-DA track: such a sector carries audio samples,
 * not a header and subheader. */
static bool cdrom_lba_is_cdda(Cdrom *cdrom, uint32_t lba) {
    if (!cdrom->disc_present || cdrom->disc.last_track == 0) return false;
    uint8_t trk = cdrom_disc_get_track_at_lba(&cdrom->disc, lba);
    return trk >= cdrom->disc.first_track && trk <= cdrom->disc.last_track &&
           cdrom->disc.tracks[trk].is_audio;
}

/* After a sector the drive does not hand to the CPU: the head moves on, and the
 * drive schedules itself one sector period later, since no INT1 acknowledge
 * will do it. */
static void cdrom_drive_advance_unsignalled(Cdrom *cdrom) {
    cdrom->head_lba = cdrom->current_lba;   /* the sector just transferred */
    cdrom->current_lba++;
    if (cdrom->drive_state == DRIVE_READING) {
        cdrom_async_reader_queue(&cdrom->async_reader, cdrom->current_lba);
        uint32_t delay = cdrom->double_speed ? CDROM_READ_DELAY_2X : CDROM_READ_DELAY_1X;
        cdrom_schedule_drive_event(cdrom, delay);
    }
}

/* One sector under the head while reading. With `int_pending`, an interrupt is
 * still unacknowledged: ADPCM and discarded sectors go ahead, a data sector is
 * held where it is (see cdrom_drive_event_tick). */
static void cdrom_drive_read_sector(Cdrom *cdrom, bool int_pending) {
    uint8_t raw[2352];
    CdromSectorStatus st = cdrom_async_reader_poll(&cdrom->async_reader, raw,
                                                   cdrom->current_lba);
    if (st == CDROM_SECTOR_PENDING) {
        /* The disc has not delivered yet. Waiting here stopped the whole
         * emulation thread on real file I/O: a seek to a cold part of a
         * 580 MB image froze it for 115-232ms, which is a dropped frame and
         * an audible gap in an audio ring that holds about 55ms. Come back
         * shortly instead: the drive is late, not the machine. */
        cdrom_schedule_drive_event(cdrom, CDROM_READ_RETRY_DELAY);
        return;
    }
    if (st == CDROM_SECTOR_FAILED) {
        /* The error is an interrupt as well; with one pending it waits for the
         * acknowledge like data (the re-armed event reads the sector again). */
        if (int_pending) return;
        /* No-disc → NOT_READY (0x80): BIOS retries. Disc error → SEEK_ERROR (0x04): BIOS aborts. */
        uint8_t err_reason = cdrom->disc_present ? 0x04 : 0x80;
        LOG_CDROM_DEBUG("[CDROM] Read failed at LBA %u (disc_present=%d, err=0x%02x)",
                        cdrom->current_lba, cdrom->disc_present, err_reason);
        cdrom_send_error(cdrom, cdrom_get_stat_byte(cdrom) | STAT_BYTE_ERROR, err_reason);
        cdrom->drive_state = DRIVE_IDLE;
        return;
    }

    CdromSectorRoute route = cdrom_route_sector(raw, cdrom_lba_is_cdda(cdrom, cdrom->current_lba),
                                                cdrom->xa_adpcm_enable, cdrom->xa_filter_enable,
                                                cdrom->xa_filter_file, cdrom->xa_filter_channel);
    if (route == CDROM_ROUTE_DATA && int_pending) {
        /* "send INT1+data, unless there's another INT pending" (:605). The
         * sector stays at the head; drive_deadline still says when it was due,
         * and the acknowledge re-arms on what is left of that. */
        cdrom_async_reader_unpoll(&cdrom->async_reader, cdrom->current_lba);
        return;
    }

    /* The head has reached the sector, so the implicit seek that started
     * this read is over and GetlocL/Pause answer normally again
     * (cdromdrive.md:896-901, :586-588). */
    cdrom->seek_phase = false;
    cdrom->sectors_read_total++;

    /* Position, for every sector the head passes, not only the ones delivered
     * as data: GetlocP "Retrieves 8 bytes of position information from
     * Subchannel Q" (cdromdrive.md:904-905), and the subchannel runs under XA
     * and filtered sectors too. This was updated in the data branch alone, and
     * GetlocP only kept moving during XA because the other channels' sectors
     * wrongly went that way.
     *
     * Not for a LibCrypt sector, whose Q carries a deliberately wrong CRC. The
     * controller discards such a sector's Q entirely and GetlocP keeps
     * answering with the previous one (cdromformat.md, CDROM Protection -
     * LibCrypt); that repeat is exactly the signal the protection counts, so
     * handing the guest the modified values instead produced a wrong 16-bit
     * key and Dino Crisis (E) walked into its own `j $` trap at 0x80029778. */
    if (!cdrom_disc_sbi_covers(&cdrom->disc, cdrom->current_lba)) {
        cdrom->current_subq_lba = cdrom->current_lba;
        cdrom->last_subq = cdrom_disc_get_subq(&cdrom->disc, cdrom->current_lba);
    }

    if (route == CDROM_ROUTE_ADPCM) {
        /* Coding info (DOCS/cdromformat.md:664-671, DOCS/cdromdrive.md:265-278):
         *   bits 0-1  mono / stereo
         *   bit  2    sample rate, 0 = 37800 Hz, 1 = 18900 Hz
         *   bit  4    bits per sample, 0 = 4-bit, 1 = 8-bit
         * The rate and depth bits used to be read as 0x04 and 0x08, so every
         * stream that was not 37800 Hz 4-bit decoded as noise. */
        uint8_t coding   = raw[19];
        bool xa_stereo   = (coding & 0x01) != 0;
        bool xa_18900    = (coding & 0x04) != 0;
        bool xa_8bit     = (coding & 0x10) != 0;

        cdrom->xa_sectors_total++;
        /* Sector sequence. The ADPCM filter is an IIR whose state carries
         * from one sector into the next, so a skipped or repeated sector does
         * not merely lose a moment of audio: it restarts the filter from the
         * wrong predecessor, and the error rings on until the signal decays.
         * XA sectors of one channel arrive interleaved with other channels'
         * sectors, so the LBA step is the interleave factor and what matters
         * is that it stays *constant*, not that it is 1. */
        {
            static uint32_t s_prev_lba = 0;
            static int32_t  s_step = 0;
            static uint32_t s_breaks = 0;
            if (s_prev_lba) {
                int32_t d = (int32_t)cdrom->current_lba - (int32_t)s_prev_lba;
                if (s_step == 0) {
                    s_step = d;
                    LOG_CDROM_INFO("[CDROM] XA interleave step = %d sectors", d);
                } else if (d != s_step) {
                    s_breaks++;
                    LOG_CDROM_WARN("[CDROM] XA sequence break #%u at LBA %u: step %d, expected %d",
                                   s_breaks, cdrom->current_lba, d, s_step);
                }
            }
            s_prev_lba = cdrom->current_lba;
        }
        /* Which (file, channel) the decoder is actually being fed. An XA file
         * interleaves several channels, and the decoder carries its ADPCM
         * filter state across sectors, so accepting two channels splices
         * unrelated audio into one stream and the state runs on from the wrong
         * predecessor. Logged only when the pair changes, which is silent on a
         * correctly filtered stream and noisy on the failure. */
        {
            static int  s_last_file = -1, s_last_ch = -1;
            static uint32_t s_switches = 0;
            if (raw[16] != s_last_file || raw[17] != s_last_ch) {
                s_switches++;
                LOG_CDROM_INFO("[CDROM] XA stream now file=%u channel=%u "
                               "(filter %s, want file=%u channel=%u), switch #%u at LBA %u",
                               raw[16], raw[17],
                               cdrom->xa_filter_enable ? "on" : "OFF",
                               cdrom->xa_filter_file, cdrom->xa_filter_channel,
                               s_switches, cdrom->current_lba);
                s_last_file = raw[16];
                s_last_ch   = raw[17];
            }
        }
        /* XA-ADPCM sector: decode to audio FIFO, NO INT1 (:1132-1133). */
        cdrom_audio_decode_xa(&cdrom->xa_adpcm_state, &cdrom->audio_fifo,
                               raw + 24, xa_stereo, xa_8bit, xa_18900);
        cdrom_drive_advance_unsignalled(cdrom);
        return;
    }

    if (route == CDROM_ROUTE_DISCARD) {
        /* An audio sector of another file/channel with the filter on: no INT1,
         * no data buffer, no GetlocL latch (:604). */
        LOG_CDROM_DEBUG("[CDROM] Sector LBA=%u discarded by the XA filter (file=%u ch=%u, want %u/%u)",
                        cdrom->current_lba, raw[16], raw[17],
                        cdrom->xa_filter_file, cdrom->xa_filter_channel);
        cdrom_drive_advance_unsignalled(cdrom);
        return;
    }

    /* Data sector. */

    /* What GetlocL answers with, latched here rather than read back out
     * of the ring below: the ring entry is cleared once the guest has
     * DMA'd the sector out, and GetlocL is asked *after* that, so
     * answering from the ring failed on every sector the game had
     * already consumed. Monsters & Co. then loops for good, re-issuing
     * Setloc/SeekL/GetlocL/ReadS twice a field waiting for a location it
     * never gets ("new game hangs").
     *
     * Only data sectors latch. A reference run over an XA section
     * interleaved 1 data : 3 audio answers GetlocL with the data
     * cadence, MSF stepping by exactly 4, so the ADPCM sectors that
     * pass to the audio decoder never become the reported location.
     * Latching those as well takes the game's demuxer off the video
     * stream and its speech never plays. */
    memcpy(cdrom->last_header, raw + 12, 8);
    cdrom->last_header_valid = true;

    /* Write into ring buffer */
    uint8_t widx = cdrom->current_write_buffer;
    SectorBuffer *sb = &cdrom->sector_buffers[widx];
    memcpy(sb->raw, raw, 2352);
    sb->lba   = cdrom->current_lba;
    sb->valid = true;

    /* Data sector: sector size from mode bits 4-5 (nocash PSX-SPX) */
    if (cdrom->mode & 0x20) {       /* bit5: 2340 bytes from sync header */
        sb->data_start = 12;
        sb->data_size  = 2340;
    } else if (cdrom->mode & 0x10) { /* bit4: 2328 bytes (with subheader, no sync) */
        sb->data_start = 24;
        sb->data_size  = 2328;
    } else {                         /* default: 2048 user data */
        sb->data_start = 24;
        sb->data_size  = 2048;
    }
    sb->position = 0;

    cdrom->current_read_buffer  = widx;
    cdrom->current_write_buffer = (widx + 1) % CDROM_SECTOR_BUFFERS;

    LOG_CDROM_DEBUG("[CDROM] Sector LBA=%u -> INT1", cdrom->current_lba);

    /* INT1: data ready. The deadline of the next sector is set here, when
     * this sector was delivered: the head reaches the next one a sector
     * period later whatever the guest does with the interrupt. The event is
     * armed for it too, so an ADPCM or filtered sector that follows is
     * handled on time even before this INT1 is acknowledged; the acknowledge
     * re-arms the same deadline. ZS1_CD_XA_HOLD=1 leaves the re-arm to the
     * acknowledge alone, as before. */
    {
        uint32_t period = cdrom->double_speed ? CDROM_READ_DELAY_2X : CDROM_READ_DELAY_1X;
        if (cdrom_xa_hold_legacy()) {
            if (cdrom->inter)
                cdrom->drive_deadline = cdrom->inter->cpu_cycle_counter + period;
        } else {
            cdrom_schedule_drive_event(cdrom, period);
        }
    }
    fifo_clear(&cdrom->response_fifo);
    cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
    cdrom->interrupt_flag = CDROM_INT_DATA_READY;
    if (cdrom->inter) interconnect_trigger_cdrom_irq(cdrom->inter);
    lua_debug_notify("cdrom_int1");

    cdrom->head_lba = cdrom->current_lba;   /* the sector just transferred */
    cdrom->current_lba++;
    if (cdrom->drive_state == DRIVE_READING)
        cdrom_async_reader_queue(&cdrom->async_reader, cdrom->current_lba);
}

void cdrom_execute_drive_int_pending(Cdrom *cdrom) {
    if (cdrom->drive_state == DRIVE_READING)
        cdrom_drive_read_sector(cdrom, true);
}

void cdrom_execute_drive(Cdrom *cdrom) {
    if (cdrom->drive_state == DRIVE_READING) {
        cdrom_drive_read_sector(cdrom, false);

    } else if (cdrom->drive_state == DRIVE_PLAYING) {
        /* CDDA */
        uint8_t raw[2352];
        CdromSectorStatus st = cdrom_async_reader_poll(&cdrom->async_reader, raw,
                                                       cdrom->current_lba);
        if (st == CDROM_SECTOR_PENDING) {   /* same reason as the read path */
            cdrom_schedule_drive_event(cdrom, CDROM_READ_RETRY_DELAY);
            return;
        }
        if (st == CDROM_SECTOR_FAILED) {
            LOG_CDROM_ERROR("[CDROM] CDDA read failed at LBA %u", cdrom->current_lba);
            cdrom->drive_state = DRIVE_IDLE;
            cdrom->seek_phase  = false;
            return;
        }
        /* The first sector after the implicit seek of a Play (begin_playing)
         * is where the head lands; AutoPause below only compares from the
         * second one on. */
        bool first_after_seek = cdrom->seek_phase;
        cdrom->seek_phase = false;   /* head arrived, same rule as the read path */

        /* Check for lead-out (track AA) */
        uint32_t lba = cdrom->current_lba;
        uint8_t track_num = cdrom_disc_get_track_at_lba(&cdrom->disc, lba);
        bool is_lead_out  = (lba >= cdrom->disc.total_sectors);
        bool track_is_audio = (track_num >= cdrom->disc.first_track)
                               ? cdrom->disc.tracks[track_num].is_audio
                               : false;

        if (is_lead_out) {
            /* End of disc: INT4 DataEnd */
            fifo_clear(&cdrom->response_fifo);
            cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
            cdrom->interrupt_flag = CDROM_INT_DATA_END;
            cdrom->drive_state    = DRIVE_IDLE;
            if (cdrom->inter) interconnect_trigger_cdrom_irq(cdrom->inter);
            return;
        }

        /* This sector's subchannel Q, unless LibCrypt makes the controller
         * discard it (same rule as the read path). */
        bool q_valid = !cdrom_disc_sbi_covers(&cdrom->disc, lba);
        SubQ q = q_valid ? cdrom_disc_get_subq(&cdrom->disc, lba) : cdrom->last_subq;

        /* AutoPause: "Issue INT4(stat) and PAUSE at end of TRACK", where "End
         * of Track is determined by sensing a track number transition in SubQ
         * position info" (psx-spx cdr/cdromdrive.md:1097-1104): this sector's Q
         * against the previous one's, the drive's own state. It used to be a
         * function-static previous track, shared by every Play and kept across
         * Pause, Setloc and savestates, so a new Play on another track paused
         * at its first sector. The first sector after a Play's seek has no
         * predecessor to compare with.
         *
         * "After autopause, the disc stays at the <end> of the old track, NOT
         * at the <begin> of the next track (so trying to resume playing by
         * sending a new Play command without new Seek/Setloc command will
         * instantly pause again)" (:1104-1107): the position goes back to the
         * last sector of the old track, and the new track's first sector is
         * not played. */
        if (cdrom->auto_pause && !first_after_seek && q_valid &&
            q.track_bcd != cdrom->last_subq.track_bcd) {
            fifo_clear(&cdrom->response_fifo);
            cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
            cdrom->interrupt_flag = CDROM_INT_DATA_END;
            cdrom->drive_state    = DRIVE_IDLE;
            cdrom->current_lba    = lba ? lba - 1 : 0;
            cdrom->head_lba       = cdrom->current_lba;
            LOG_CDROM_DEBUG("[CDROM] AutoPause: track %02X -> %02X at LBA %u",
                            cdrom->last_subq.track_bcd, q.track_bcd, lba);
            if (cdrom->inter) interconnect_trigger_cdrom_irq(cdrom->inter);
            return;
        }

        if (track_is_audio) {
            cdrom_audio_process_cdda(&cdrom->audio_fifo, raw);
        }

        if (q_valid) {
            cdrom->current_subq_lba = lba;
            cdrom->last_subq = q;
        }
        /* The pickup is on this sector now. Seeks after a Play measured their
         * distance from wherever the last data read had left head_lba. */
        cdrom->head_lba = lba;

        /* Report: INT1(stat, track, index, mm/amm, ss+80h/ass, sect/asect,
         * peaklo, peakhi), eight bytes, and NOT on every sector. The packet
         * carries absolute time on asect 00/20/40/60h and time within the track
         * (with bit7 of ss set) on 10/30/50/70h (cdromdrive.md:1077-1094).
         *
         * This used to send nine bytes with both time bases at once, on every
         * single sector, which is neither the shape nor the rate a player
         * expects. The peak bytes are zero: we have no peak meter, and the
         * hardware's own is reset on each read so nine of every ten frames are
         * lost anyway (cdrominternalinfoonpsxcdromcontroller.md:1483-1492). */
        if (cdrom->report_enable && cdrom->interrupt_flag == CDROM_INT_NONE) {
            SubQ *sq = &cdrom->last_subq;
            uint8_t asect = sq->abs_ff_bcd;
            bool report_abs = (asect == 0x00 || asect == 0x20 || asect == 0x40 || asect == 0x60);
            bool report_rel = (asect == 0x10 || asect == 0x30 || asect == 0x50 || asect == 0x70);
            if (report_abs || report_rel) {
                fifo_clear(&cdrom->response_fifo);
                cdrom_push_response(cdrom, cdrom_get_stat_byte(cdrom));
                cdrom_push_response(cdrom, sq->track_bcd);
                cdrom_push_response(cdrom, sq->index_bcd);
                if (report_abs) {
                    cdrom_push_response(cdrom, sq->abs_mm_bcd);
                    cdrom_push_response(cdrom, sq->abs_ss_bcd);
                    cdrom_push_response(cdrom, sq->abs_ff_bcd);
                } else {
                    cdrom_push_response(cdrom, sq->rel_mm_bcd);
                    cdrom_push_response(cdrom, (uint8_t)(sq->rel_ss_bcd | 0x80));
                    cdrom_push_response(cdrom, sq->rel_ff_bcd);
                }
                cdrom_push_response(cdrom, 0x00);   /* peak lo */
                cdrom_push_response(cdrom, 0x00);   /* peak hi */
                cdrom->interrupt_flag = CDROM_INT_DATA_READY;
                if (cdrom->inter) interconnect_trigger_cdrom_irq(cdrom->inter);
            }
        }

        cdrom->current_lba += cdrom->cdda_speed;

        /* Queue next */
        if (cdrom->drive_state == DRIVE_PLAYING)
            cdrom_async_reader_queue(&cdrom->async_reader, cdrom->current_lba);

        /* Schedule next drive event */
        uint32_t delay = cdrom->double_speed ? CDROM_READ_DELAY_2X : CDROM_READ_DELAY_1X;
        cdrom_schedule_drive_event(cdrom, delay);
    }
}
