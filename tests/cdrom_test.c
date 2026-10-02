/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/* CDROM drive unit tests (docs/TESTING_PLAN_2026-08-20.md, layer 1): sector
 * routing, the drive under a pending interrupt, ADPBUSY, AutoPause, Read after
 * Pause and the audio output stage. cdrom.c, cdrom_commands.c and
 * cdrom_audio.c are compiled in whole; the disc, the reader thread and the
 * event queue are fakes below. Line numbers are psx-spx cdr/cdromdrive.md
 * unless noted.
 */
#include "log.h"

#include "../src/cdrom/cdrom.c"
#include "../src/cdrom/cdrom_commands.c"
#include "../src/cdrom/cdrom_audio.c"

#include <stdio.h>

/* ---- stubs ---------------------------------------------------------------- */

/* The LOG_* macros test this at the call site (include/log.h). */
LogLevel current_log_level = LOG_LEVEL_INFO;
void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)category; (void)level; (void)format;
}

void frame_events_record(FrameEventType type, uint32_t detail) { (void)type; (void)detail; }

static int g_int1_events, g_irqs;
void lua_debug_notify(const char* event_name) {
    if (strcmp(event_name, "cdrom_int1") == 0) g_int1_events++;
}
void interconnect_trigger_cdrom_irq(Interconnect* inter) { (void)inter; g_irqs++; }

static bool     g_evq_armed[EVQ_EVENT_COUNT];
static uint32_t g_evq_delay[EVQ_EVENT_COUNT];
void eventq_schedule(struct Interconnect* sys, EventQueueType event, uint32_t cycles_from_now) {
    (void)sys;
    g_evq_armed[event] = true;
    g_evq_delay[event] = cycles_from_now;
}

/* A disc: track 1 data at 0, track 2 audio at 1000, track 3 audio at 1100. */
uint8_t cdrom_disc_get_track_at_lba(CdromDisc* disc, uint32_t lba) {
    if (!disc || disc->last_track == 0) return 1;
    for (int i = disc->first_track; i <= disc->last_track; i++)
        if (i == disc->last_track || disc->tracks[i + 1].start_lba > lba) return (uint8_t)i;
    return disc->last_track;
}
static uint32_t g_sbi_lba = 0xFFFFFFFFu;
bool cdrom_disc_sbi_covers(const CdromDisc* disc, uint32_t lba) { (void)disc; return lba == g_sbi_lba; }
SubQ cdrom_disc_get_subq(CdromDisc* disc, uint32_t lba) {
    SubQ q;
    memset(&q, 0, sizeof(q));
    q.track_bcd  = cdrom_to_bcd(cdrom_disc_get_track_at_lba(disc, lba));
    q.index_bcd  = 1;
    q.abs_ff_bcd = cdrom_to_bcd((uint8_t)((lba + 150) % 75));
    q.abs_ss_bcd = cdrom_to_bcd((uint8_t)(((lba + 150) / 75) % 60));
    q.abs_mm_bcd = cdrom_to_bcd((uint8_t)((lba + 150) / 75 / 60));
    return q;
}
uint32_t cdrom_disc_get_seek_ticks(uint32_t from, uint32_t to) { (void)from; (void)to; return 1000; }
bool cdrom_disc_load(CdromDisc* disc, const char* path) { (void)disc; (void)path; return false; }
void cdrom_disc_unload(CdromDisc* disc) { (void)disc; }
char cdrom_disc_detect_region(CdromDisc* disc) { (void)disc; return 'E'; }

/* The reader: every LBA is ready at once, built from g_kind[]. */
enum { K_DATA, K_XA0, K_XA1, K_MODE1_LOOKALIKE, K_XA0_NO_RT };
static int g_kind[2048];
static int g_unpolls, g_polls;
static void make_sector(uint32_t lba, uint8_t* raw) {
    memset(raw, 0, 2352);
    uint8_t mm, ss, ff;
    cdrom_lba_to_msf(lba, &mm, &ss, &ff);
    raw[12] = cdrom_to_bcd(mm); raw[13] = cdrom_to_bcd(ss); raw[14] = cdrom_to_bcd(ff);
    raw[15] = 2;
    raw[16] = 1;                                     /* file 1 */
    switch (g_kind[lba]) {
    case K_DATA:            raw[17] = 0; raw[18] = 0x08; break;
    case K_XA0:             raw[17] = 0; raw[18] = 0x64; raw[19] = 0x01; break;
    case K_XA1:             raw[17] = 1; raw[18] = 0x64; raw[19] = 0x01; break;
    case K_XA0_NO_RT:       raw[17] = 0; raw[18] = 0x24; raw[19] = 0x01; break;
    case K_MODE1_LOOKALIKE: raw[15] = 1; raw[17] = 0; raw[18] = 0x64; break;
    }
    for (int i = 24; i < 2352; i++) raw[i] = (uint8_t)(i * 7 + lba);
}
void cdrom_async_reader_init(CdromAsyncReader* r, CdromDisc* disc) { (void)r; (void)disc; }
void cdrom_async_reader_shutdown(CdromAsyncReader* r) { (void)r; }
void cdrom_async_reader_queue(CdromAsyncReader* r, uint32_t lba) { (void)r; (void)lba; }
CdromSectorStatus cdrom_async_reader_poll(CdromAsyncReader* r, uint8_t* out, uint32_t lba) {
    (void)r;
    g_polls++;
    make_sector(lba, out);
    return CDROM_SECTOR_READY;
}
void cdrom_async_reader_unpoll(CdromAsyncReader* r, uint32_t lba) { (void)r; (void)lba; g_unpolls++; }

/* ---- harness ---------------------------------------------------------------- */

static Interconnect g_inter;
static int g_fail, g_checks;

#define CHECK(cond, ...) do {                                               \
    g_checks++;                                                             \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
                   printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

static Cdrom* cd(void) { return &g_inter.cdrom; }

static void setup(void) {
    memset(&g_inter, 0, sizeof(g_inter));
    cdrom_init(cd(), &g_inter);
    cd()->disc_present = true;
    cd()->shell_open = false;
    cd()->motor_on = true;
    cd()->disc.first_track = 1;
    cd()->disc.last_track = 3;
    cd()->disc.tracks[1].start_lba = 0;    cd()->disc.tracks[1].is_audio = false;
    cd()->disc.tracks[2].start_lba = 1000; cd()->disc.tracks[2].is_audio = true;
    cd()->disc.tracks[3].start_lba = 1100; cd()->disc.tracks[3].is_audio = true;
    cd()->disc.total_sectors = 1200;
    memset(g_kind, 0, sizeof(g_kind));
    memset(g_evq_armed, 0, sizeof(g_evq_armed));
    g_int1_events = g_irqs = g_unpolls = g_polls = 0;
    g_sbi_lba = 0xFFFFFFFFu;
}

static void ack_all(void) {
    cdrom_write8(cd(), 0x1F801800, 1);       /* bank 1 */
    cdrom_write8(cd(), 0x1F801803, 0x1F);    /* acknowledge INT1-5 */
    cdrom_write8(cd(), 0x1F801800, 0);
}

static void run_command(uint8_t cmd, const uint8_t* params, int n) {
    fifo_clear(&cd()->param_fifo);
    for (int i = 0; i < n; i++) fifo_push(&cd()->param_fifo, params[i]);
    cd()->pending_command = (CdromCommand)cmd;
    cd()->pending_param_count = (uint8_t)n;
    for (int i = 0; i < n; i++) cd()->pending_params[i] = params[i];
    cdrom_execute_command(cd());
    cd()->interrupt_flag = 0;                /* the guest acknowledges at once */
}

static void set_xa_mode(bool adpcm, bool filter, uint8_t file, uint8_t ch) {
    cd()->xa_adpcm_enable = adpcm;
    cd()->xa_filter_enable = filter;
    cd()->xa_filter_file = file;
    cd()->xa_filter_channel = ch;
    cd()->mode = (uint8_t)((adpcm ? 0x40 : 0) | (filter ? 0x08 : 0));
}

/* ---- A1: routing against a transcription of :595-608 ----------------------- */

static CdromSectorRoute doc_route(bool cdda, uint8_t mode_byte, bool adpcm, bool filter,
                                  bool fc_match, uint8_t submode) {
    bool audio_rt = (submode & 0x44) == 0x44;
    bool mode2 = mode_byte == 2;
    /* try_deliver_as_adpcm_sector (:595-601) */
    if (!cdda && mode2 && adpcm && !(filter && !fc_match) && audio_rt)
        return CDROM_ROUTE_ADPCM;
    /* try_deliver_as_data_sector (:602-604). The submode is a Mode 2 subheader
     * byte (cdromformat.md:486-503, :643-653): not there on CD-DA or Mode 1. */
    if (filter && audio_rt && mode2 && !cdda)
        return CDROM_ROUTE_DISCARD;
    return CDROM_ROUTE_DATA;
}

static void test_route_table(void) {
    static const uint8_t submodes[] = { 0x00, 0x04, 0x40, 0x44, 0x64, 0x08, 0x48, 0x24 };
    int n = 0;
    for (int cdda = 0; cdda < 2; cdda++)
    for (int mode = 1; mode <= 2; mode++)
    for (int adpcm = 0; adpcm < 2; adpcm++)
    for (int filter = 0; filter < 2; filter++)
    for (int match = 0; match < 2; match++)
    for (unsigned s = 0; s < sizeof(submodes); s++) {
        uint8_t raw[2352];
        memset(raw, 0, sizeof(raw));
        raw[15] = (uint8_t)mode;
        raw[16] = 3; raw[17] = match ? 5 : 6; raw[18] = submodes[s];
        CdromSectorRoute got = cdrom_route_sector(raw, cdda, adpcm, filter, 3, 5);
        CdromSectorRoute want = doc_route(cdda, (uint8_t)mode, adpcm, filter, match, submodes[s]);
        CHECK(got == want, "route cdda=%d mode=%d adpcm=%d filter=%d match=%d sm=%02x: got %d want %d",
              cdda, mode, adpcm, filter, match, submodes[s], got, want);
        n++;
    }
    /* The named cases from the audit (D_audio_cinematiche_3d.md X5-X7). */
    uint8_t raw[2352];
    memset(raw, 0, sizeof(raw));
    raw[15] = 2; raw[16] = 1; raw[17] = 1; raw[18] = 0x64;
    CHECK(cdrom_route_sector(raw, false, true, true, 1, 0) == CDROM_ROUTE_DISCARD,
          "X6: audio+RT of another channel with the filter on is discarded (:604)");
    CHECK(cdrom_route_sector(raw, false, false, true, 1, 0) == CDROM_ROUTE_DISCARD,
          "X7: ADPCM off, filter on: discarded too (:598, :604)");
    CHECK(cdrom_route_sector(raw, false, false, false, 1, 0) == CDROM_ROUTE_DATA,
          "ADPCM and filter off: an audio sector is plain data (:605)");
    CHECK(cdrom_route_sector(raw, false, true, true, 1, 1) == CDROM_ROUTE_ADPCM,
          "matching channel goes to the decoder (:601)");
    raw[15] = 1;
    CHECK(cdrom_route_sector(raw, false, true, false, 1, 1) == CDROM_ROUTE_DATA,
          "X5: a Mode 1 sector is never ADPCM (:597)");
    raw[15] = 2;
    CHECK(cdrom_route_sector(raw, true, true, false, 1, 1) == CDROM_ROUTE_DATA,
          "X5: a CD-DA sector is never ADPCM (:596)");
}

/* ---- A1 + X19 in the drive loop ------------------------------------------- */

static void test_drive_routing(void) {
    setup();
    set_xa_mode(true, true, 1, 0);
    g_kind[10] = K_DATA; g_kind[11] = K_XA0; g_kind[12] = K_XA1;
    g_kind[13] = K_MODE1_LOOKALIKE; g_kind[14] = K_XA0_NO_RT;
    cd()->drive_state = DRIVE_READING;
    cd()->current_lba = 10;

    cdrom_execute_drive(cd());                               /* 10: data */
    CHECK(cd()->interrupt_flag == CDROM_INT_DATA_READY && g_int1_events == 1, "data sector: INT1");
    CHECK(cd()->last_header[2] == cdrom_to_bcd((10 + 150) % 75), "GetlocL latch = LBA 10");
    CHECK(cd()->current_subq_lba == 10, "SubQ follows");
    ack_all();

    memset(g_evq_armed, 0, sizeof(g_evq_armed));
    cdrom_execute_drive(cd());                               /* 11: XA channel 0 */
    CHECK(cd()->interrupt_flag == 0 && g_int1_events == 1, "ADPCM sector raises no INT1 (:1132-1133)");
    CHECK(cd()->xa_sectors_total == 1 && cd()->audio_fifo.count > 0, "decoded into the audio FIFO");
    CHECK(cd()->last_header[2] == cdrom_to_bcd((10 + 150) % 75), "GetlocL latch untouched by ADPCM");
    CHECK(cd()->current_subq_lba == 11, "X19: SubQ follows an ADPCM sector too (:904-905)");
    CHECK(g_evq_armed[EVQ_CDROM_DRIVE] && g_evq_delay[EVQ_CDROM_DRIVE] == CDROM_READ_DELAY_1X,
          "the drive schedules its own next sector");

    uint32_t fifo_before = cd()->audio_fifo.count;
    memset(g_evq_armed, 0, sizeof(g_evq_armed));
    cdrom_execute_drive(cd());                               /* 12: XA channel 1 */
    CHECK(cd()->interrupt_flag == 0 && g_int1_events == 1, "X6: filtered sector raises no INT1 (:604)");
    CHECK(cd()->xa_sectors_total == 1 && cd()->audio_fifo.count == fifo_before, "and is not decoded");
    CHECK(cd()->last_header[2] == cdrom_to_bcd((10 + 150) % 75), "and does not move the GetlocL latch");
    CHECK(cd()->sector_buffers[cd()->current_read_buffer].lba == 10, "nor the data buffer");
    CHECK(cd()->current_subq_lba == 12, "SubQ still follows the head");
    CHECK(g_evq_armed[EVQ_CDROM_DRIVE], "next sector scheduled");
    CHECK(cd()->current_lba == 13 && cd()->head_lba == 12, "head advanced");

    cdrom_execute_drive(cd());                               /* 13: Mode 1 lookalike */
    CHECK(cd()->interrupt_flag == CDROM_INT_DATA_READY && g_int1_events == 2,
          "X5: a Mode 1 sector is data, whatever bytes 16-19 hold");
    ack_all();
    cdrom_execute_drive(cd());                               /* 14: audio without RT */
    CHECK(g_int1_events == 3, "audio without the realtime bit is data (:600, :604)");
}

/* LibCrypt: no SubQ update on an SBI-covered sector, on any route. */
static void test_sbi_rule_kept(void) {
    setup();
    set_xa_mode(true, true, 1, 0);
    g_kind[10] = K_DATA; g_kind[11] = K_XA0; g_kind[12] = K_XA1;
    g_sbi_lba = 11;
    cd()->drive_state = DRIVE_READING;
    cd()->current_lba = 10;
    cdrom_execute_drive(cd()); ack_all();
    cdrom_execute_drive(cd());
    CHECK(cd()->current_subq_lba == 10, "SBI sector keeps the previous Q (ADPCM route)");
    g_sbi_lba = 12;
    cdrom_execute_drive(cd());
    CHECK(cd()->current_subq_lba == 10, "SBI sector keeps the previous Q (discard route)");
}

/* ---- A5: the drive under a pending interrupt ------------------------------- */

static void test_int_pending(void) {
    setup();
    set_xa_mode(true, true, 1, 0);
    g_kind[20] = K_XA0; g_kind[21] = K_XA1; g_kind[22] = K_DATA; g_kind[23] = K_XA0;
    cd()->drive_state = DRIVE_READING;
    cd()->current_lba = 20;
    cd()->interrupt_flag = CDROM_INT_ACK;                    /* an INT3 nobody acknowledged */
    cd()->drive_deadline = 5000;
    g_inter.cpu_cycle_counter = 5000;

    cdrom_drive_event_tick(&g_inter);                        /* 20: ADPCM */
    CHECK(cd()->xa_sectors_total == 1 && cd()->current_lba == 21,
          "ADPCM sector handled although an INT is pending (:759-762)");
    CHECK(cd()->interrupt_flag == CDROM_INT_ACK, "the pending INT3 is left alone");
    cdrom_drive_event_tick(&g_inter);                        /* 21: filtered */
    CHECK(cd()->current_lba == 22 && g_int1_events == 0, "discarded sector handled too");

    uint32_t deadline = cd()->drive_deadline;
    g_inter.cpu_cycle_counter = deadline;
    memset(g_evq_armed, 0, sizeof(g_evq_armed));
    cdrom_drive_event_tick(&g_inter);                        /* 22: data, INT pending */
    CHECK(cd()->current_lba == 22 && g_int1_events == 0 && g_unpolls == 1,
          "data sector held while an INT is pending (:605)");
    CHECK(cd()->drive_deadline == deadline, "its deadline is kept");
    CHECK(!g_evq_armed[EVQ_CDROM_DRIVE], "no new event: the acknowledge re-arms");

    g_inter.cpu_cycle_counter = deadline + 100000;
    ack_all();
    CHECK(g_evq_armed[EVQ_CDROM_DRIVE] && g_evq_delay[EVQ_CDROM_DRIVE] == CDROM_MIN_INT_DELAY,
          "acknowledge re-arms on what is left of the deadline (here: overdue)");
    cdrom_drive_event_tick(&g_inter);
    CHECK(cd()->interrupt_flag == CDROM_INT_DATA_READY && g_int1_events == 1, "data delivered after the ack");
    CHECK(g_evq_armed[EVQ_CDROM_DRIVE] && g_evq_delay[EVQ_CDROM_DRIVE] == CDROM_READ_DELAY_1X,
          "and the next sector is armed one period on");
    cdrom_drive_event_tick(&g_inter);                        /* 23: ADPCM under the INT1 */
    CHECK(cd()->xa_sectors_total == 2, "the following ADPCM sector does not wait for the INT1 ack");
}

/* ---- A6: ADPBUSY ------------------------------------------------------------ */

static void test_adpbusy(void) {
    setup();
    set_xa_mode(true, false, 0, 0);
    cd()->drive_state = DRIVE_READING;
    CHECK(!(cdrom_read8(cd(), 0x1F801800) & STAT_ADPBUSY), "no XA queued: ADPBUSY clear");
    cdrom_audio_fifo_push(&cd()->audio_fifo, 1, 1);
    CHECK(cdrom_read8(cd(), 0x1F801800) & STAT_ADPBUSY, "XA playing: HSTS.2 set (:62)");
    cd()->muted = true;
    CHECK(cdrom_read8(cd(), 0x1F801800) & STAT_ADPBUSY, "still set under Mute (:1020-1021)");
    cd()->drive_state = DRIVE_PLAYING;
    CHECK(!(cdrom_read8(cd(), 0x1F801800) & STAT_ADPBUSY), "CD-DA is not XA-ADPCM");
    cd()->drive_state = DRIVE_READING;
    cd()->xa_adpcm_enable = false;
    CHECK(!(cdrom_read8(cd(), 0x1F801800) & STAT_ADPBUSY), "ADPCM off: clear");
}

/* ---- A8: AutoPause ---------------------------------------------------------- */

static void play_sectors(int n) {
    for (int i = 0; i < n && cd()->drive_state == DRIVE_PLAYING; i++) cdrom_execute_drive(cd());
}

static void test_autopause(void) {
    setup();
    cd()->auto_pause = true;
    cd()->mode = 0x02;
    uint8_t p[3] = { 0x00, 0x16, 0x47 };     /* 00:16:47 = LBA 1097, track 2 */
    run_command(CDC_SETLOC, p, 3);
    run_command(CDC_PLAY, NULL, 0);
    CHECK(cd()->drive_state == DRIVE_PLAYING && cd()->current_lba == 1097, "playing from 1097");
    play_sectors(3);                         /* 1097, 1098, 1099: track 2 */
    CHECK(cd()->drive_state == DRIVE_PLAYING, "no pause inside the track");
    cdrom_execute_drive(cd());               /* 1100: track 3 */
    CHECK(cd()->drive_state == DRIVE_IDLE && cd()->interrupt_flag == CDROM_INT_DATA_END,
          "INT4 and pause at the track transition (:1100, :1103-1104)");
    CHECK(cd()->current_lba == 1099 && cd()->head_lba == 1099,
          "D7: the disc stays at the end of the old track (:1104-1106): %u", cd()->current_lba);
    cd()->interrupt_flag = 0;

    /* D7: Play again without Setloc pauses again at once (:1106-1107). */
    run_command(CDC_PLAY, NULL, 0);
    play_sectors(1);
    CHECK(cd()->drive_state == DRIVE_PLAYING, "first sector back in the old track plays");
    cdrom_execute_drive(cd());
    CHECK(cd()->drive_state == DRIVE_IDLE && cd()->interrupt_flag == CDROM_INT_DATA_END,
          "and the transition pauses again");
    cd()->interrupt_flag = 0;

    /* D6: a new Play on another track does not pause at its first sector, even
     * though the last Q seen was track 2. */
    p[0] = 0x00; p[1] = 0x16; p[2] = 0x50;   /* LBA 1100 */
    run_command(CDC_SETLOC, p, 3);
    run_command(CDC_PLAY, NULL, 0);
    play_sectors(5);
    CHECK(cd()->drive_state == DRIVE_PLAYING && cd()->current_lba == 1105,
          "D6: Play on track 3 runs on (no stale previous track): state=%d lba=%u",
          cd()->drive_state, cd()->current_lba);
}

/* ---- R8: Read after Pause --------------------------------------------------- */

static void test_read_after_pause(void) {
    setup();
    g_kind[30] = g_kind[31] = g_kind[32] = K_DATA;
    uint8_t p[3] = { 0x00, 0x02, 0x30 };     /* 00:02:30 = LBA 30 */
    run_command(CDC_SETLOC, p, 3);
    run_command(CDC_READN, NULL, 0);
    cdrom_execute_drive(cd()); ack_all();    /* 30 */
    cdrom_execute_drive(cd()); ack_all();    /* 31 */
    CHECK(cd()->head_lba == 31 && cd()->current_lba == 32, "read 30 and 31");
    run_command(CDC_PAUSE, NULL, 0);
    cd()->second_response_cmd = CDC_PAUSE;
    cdrom_execute_second_response(cd());
    cd()->interrupt_flag = 0;
    CHECK(cd()->drive_state == DRIVE_IDLE, "paused");
    run_command(CDC_READN, NULL, 0);
    CHECK(cd()->current_lba == 31,
          "Read after Pause resumes at the most recently received sector (:811-814): %u",
          cd()->current_lba);
    cdrom_execute_drive(cd());
    CHECK(cd()->sector_buffers[cd()->current_read_buffer].lba == 31, "sector 31 returned once more");
    ack_all();
    /* With a Setloc pending the target wins (:809-810). */
    run_command(CDC_PAUSE, NULL, 0);
    cd()->second_response_cmd = CDC_PAUSE;
    cdrom_execute_second_response(cd());
    cd()->interrupt_flag = 0;
    run_command(CDC_SETLOC, p, 3);
    run_command(CDC_READN, NULL, 0);
    CHECK(cd()->current_lba == 30, "Setloc pending: read from the Setloc target");
}

/* ---- A10: the output stage --------------------------------------------------- */

static void test_output_stage(void) {
    setup();
    int16_t l = 1000, r = -2000;
    cdrom_apply_output_volume(cd(), &l, &r);
    CHECK(l == 1000 && r == -2000, "default ATV (80h,0,0,80h) is unity");
    cd()->muted = true;
    l = 1000; r = 1000;
    cdrom_apply_output_volume(cd(), &l, &r);
    CHECK(l == 0 && r == 0, "Mute forces 0 (:1019-1022)");
    cd()->muted = false;
    cd()->xa_mute = true;
    cd()->drive_state = DRIVE_READING;
    l = 1000; r = 1000;
    cdrom_apply_output_volume(cd(), &l, &r);
    CHECK(l == 0 && r == 0, "ADPMUTE mutes XA (:251)");
    cd()->drive_state = DRIVE_PLAYING;
    l = 1000; r = 1000;
    cdrom_apply_output_volume(cd(), &l, &r);
    CHECK(l == 1000 && r == 1000, "ADPMUTE leaves CD-DA alone");
    cd()->xa_mute = false;
    /* ATV through the ports: ATV0..3 then CHNGATV (:227-230, :242, :253). */
    cdrom_write8(cd(), 0x1F801800, 2);
    cdrom_write8(cd(), 0x1F801802, 0x40);    /* ATV0 L->L */
    cdrom_write8(cd(), 0x1F801803, 0x40);    /* ATV1 L->R */
    cdrom_write8(cd(), 0x1F801800, 3);
    cdrom_write8(cd(), 0x1F801801, 0x40);    /* ATV2 R->R */
    cdrom_write8(cd(), 0x1F801802, 0x40);    /* ATV3 R->L */
    cdrom_write8(cd(), 0x1F801803, 0x20);    /* CHNGATV */
    l = 1000; r = 3000;
    cdrom_apply_output_volume(cd(), &l, &r);
    CHECK(l == 2000 && r == 2000, "40h x4 through the ports is mono (:231-233): %d %d", l, r);
    cdrom_audio_fifo_push(&cd()->audio_fifo, 4000, 0);
    cdrom_get_audio_frame(cd(), &l, &r);
    CHECK(l == 2000 && r == 2000, "cdrom_get_audio_frame applies the same stage");
}

int main(void) {
    test_route_table();
    test_drive_routing();
    test_sbi_rule_kept();
    test_int_pending();
    test_adpbusy();
    test_autopause();
    test_read_after_pause();
    test_output_stage();
    printf("cdrom_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
