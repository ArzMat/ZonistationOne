-- Which audio mechanism does a cutscene use, and which known defect is in play?
--
-- docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md, section 4.6. An in-engine scene
-- plays its sound one of four ways:
--   (a) XA-ADPCM read while the engine reads data   (Setmode XA + filter, ReadS)
--   (b) CD-DA                                       (Play, AutoPause)
--   (c) SPU-ADPCM streaming                         (DMA4, IRQA reloads, loops)
--   (d) voice sequences                             (KON/KOFF, sweeps, MVOL fades)
-- This tells them apart from a savestate taken a few seconds before the scene.
--
-- Run:  ZS1_LUA_SCRIPT=scripts/cutscene_audio_classify.lua \
--         ./ZoniStation_One roms/bios-pal.bin --game="games/game.bin"
-- The state loaded on the first vblank is ZS1_CLASSIFY_STATE, default
-- savestates/slot0.zst (F5 writes it); set ZS1_CLASSIFY_STATE= (empty) to skip
-- the load and classify from boot.
--
-- Register values come from emu.spu_irq() / emu.cd_state() / emu.spu_voice(),
-- read once per vblank: a write watchpoint fires *before* the store lands, so
-- the watches below only count writes, and the values are read afterwards.
--
-- Never quote a speed figure from a run with this probe loaded (CLAUDE.md).

local STATE = os.getenv("ZS1_CLASSIFY_STATE") or "savestates/slot0.zst"
local EVERY = 25                     -- vblanks per summary line (half a second PAL)

local loaded, f = false, 0
local int1, int1_audio = 0, 0        -- INT1s, and those carrying a filtered audio sector
local hits, prev = {}, {}

local WATCH = {                      -- KUSEG and KSEG1: watches compare the virtual address
  [0x1F801DA4] = "IRQA",   [0xBF801DA4] = "IRQA",
  [0x1F801D80] = "MVOLL",  [0xBF801D80] = "MVOLL",
  [0x1F801D82] = "MVOLR",  [0xBF801D82] = "MVOLR",
  [0x1F801DAA] = "SPUCNT", [0xBF801DAA] = "SPUCNT",
  [0x1F801DB0] = "AVOLL",  [0xBF801DB0] = "AVOLL",
  [0x1F801D88] = "KON",    [0xBF801D88] = "KON",
  [0x1F8010C8] = "DMA4",   [0xBF8010C8] = "DMA4",
  [0x1F801801] = "CDREG1", [0xBF801801] = "CDREG1",   -- command in bank 0, ATV2 in bank 3
}
for a in pairs(WATCH) do emu.add_write_watch(a) end

emu.on_break(function(reason)
  local wp = tonumber(reason:match("0x(%x+)") or "", 16)
  local name = wp and WATCH[wp]
  if name then hits[name] = (hits[name] or 0) + 1 end
  emu.resume()
end)

-- Difference since the previous summary line.
local function delta(key, value)
  local d = value - (prev[key] or value)
  prev[key] = value
  return d
end

-- Log a register the first time it is seen and whenever it changes.
local last_val = {}
local function track(name, value, fmt)
  if last_val[name] ~= value then
    if last_val[name] ~= nil then
      emu.log(string.format("[cls] f=%d %s " .. fmt .. " -> " .. fmt, f, name, last_val[name], value))
    end
    last_val[name] = value
  end
end

local function sample_registers()
  local s = emu.spu_irq()
  track("IRQA",   s.irq_reg, "%04x")
  track("MVOLL",  s.main_vol_left_reg, "%04x")    -- bit 15 set = sweep (A4)
  track("MVOLR",  s.main_vol_right_reg, "%04x")
  track("SPUCNT", s.control, "%04x")              -- bit 14 = 0 mutes voices only (A7)
  track("AVOLL",  s.cd_vol_left & 0xFFFF, "%04x") -- 0 = CD silent (A10)
  track("AVOLR",  s.cd_vol_right & 0xFFFF, "%04x")
  local c = emu.cd_state()
  track("CDMUTE", c.muted and 1 or 0, "%d")       -- Mute command 0Bh (A10)
  track("ADPMUTE", c.adpmute and 1 or 0, "%d")
  track("ATV", (c.atv_ll << 24) | (c.atv_lr << 16) | (c.atv_rl << 8) | c.atv_rr, "%08x")
  track("FILTER", (c.xa_filter and 0x10000 or 0) | (c.filter_file << 8) | c.filter_channel, "%05x")
  track("CDMODE", c.mode, "%02x")
end

emu.on_event(function(ev)
  if ev == "cdrom_int1" then
    int1 = int1 + 1
    -- An INT1 whose sector is audio+realtime with the XA filter on is one the
    -- hardware never raises (psx-spx cdr/cdromdrive.md:604): A1 in action.
    local c = emu.cd_state()
    if c.xa_filter and c.header_valid and (c.header[7] & 0x44) == 0x44 then
      int1_audio = int1_audio + 1
    end
    return
  end
  if ev ~= "vblank" then return end
  f = f + 1

  if not loaded then
    loaded = true
    if STATE ~= "" then
      emu.load_state(STATE)
      emu.log("[cls] load_state requested: " .. STATE)
      return
    end
  end

  sample_registers()
  if f % EVERY ~= 0 then return end

  local cnt, push, _, _, starve, ctrl, sect, xas = emu.cd_audio()
  local _, _, _, _, keys = emu.spu_stats()
  local irq = emu.spu_irq()

  -- Voices: audible, and silent but still reading (they can raise IRQs, A2).
  local on, silent_moving = 0, 0
  for v = 0, 23 do
    local sv = emu.spu_voice(v)
    if sv.on then
      on = on + 1
    elseif sv.pitch ~= 0 then
      local key = "va" .. v
      if prev[key] and prev[key] ~= sv.curr_addr then silent_moving = silent_moving + 1 end
      prev[key] = sv.curr_addr
    end
  end

  emu.log(string.format(
    "[cls] f=%d sect=%d xa=%d int1=%d int1_audio=%d push=%d starve=%d cdq=%d SPUCNT=%04x"
    .. " KON=%d IRQA_w=%d irq9=%s DMA4=%d MVOL_w=%d AVOL_w=%d CDREG1_w=%d voices_on=%d silent_reading=%d",
    f, delta("s", sect), delta("x", xas), delta("i", int1), delta("ia", int1_audio),
    delta("p", push), delta("st", starve), cnt, ctrl,
    delta("k", keys), delta("q", hits.IRQA or 0), irq.irq9_flag and "set" or "clr",
    delta("m", hits.DMA4 or 0), delta("v", (hits.MVOLL or 0) + (hits.MVOLR or 0)),
    delta("a", hits.AVOLL or 0), delta("c", hits.CDREG1 or 0), on, silent_moving))
end)

emu.log("[cls] cutscene audio classifier armed")

-- Reading the result (section 4.6):
--   xa > 0 and push > 0 during the line             -> (a) XA
--   int1_audio > 0                                   -> (a) with A1 (fixed: must be 0)
--   xa = 0, DMA4 and KON regular, IRQA_w steady      -> (c) SPU streaming
--   IRQA_w stops after one or two reloads            -> (c) with A2
--   MVOLL/MVOLR changes with bit 15 set at scene start -> A4 (sweep fade)
--   SPUCNT bit 14 = 0 while push grows               -> A7
--   AVOLL to 0000, CDMUTE 1 or ADPMUTE 1 at the cut   -> A10
--   starve grows during a line with xa > 0           -> A5
