-- Enter the speech scene from a savestate and record 30 s of output.
--
-- The pop has never been reproduced from a fixed point: every run had to boot to
-- it, so the defect moved. This loads savestates/slot0.zst on the first vblank
-- and then reports, once per half second, the stages that sit before the final
-- mix — so the raw dump written by ZS1_AUDIO_DUMP can be lined up against what
-- the reverb network and the delivery counters were doing at that moment.
--
-- Run:  ZS1_LUA_SCRIPT=scripts/spu_pop_capture.lua ZS1_AUDIO_DUMP=/path/out.raw \
--         ./ZoniStation_One roms/bios-pal.bin --game="games/game.bin"
-- A/B:  add ZS1_SPU_NO_REVERB=1 to take the reverb network out of the path.

local STATE       = "savestates/slot0.zst"
local NEAR_RAIL   = 30000   -- |sample| above this counts as riding the limit
local REPORT_EVERY = 25     -- vblanks; PAL field rate, so about half a second

local frames  = 0
local loaded  = false
local base_gen = nil        -- spu_generated at the first report after the load

local win = {
  in_peak = 0, out_peak = 0, in_rail = 0, out_rail = 0,
  samples = 0, reverb_on_frames = 0,
}

local function absmax(a, b)
  a = a < 0 and -a or a
  b = b < 0 and -b or b
  return a > b and a or b
end

local function reset()
  win.in_peak, win.out_peak = 0, 0
  win.in_rail, win.out_rail = 0, 0
  win.samples, win.reverb_on_frames = 0, 0
end

emu.on_event(function(name)
  if name ~= "vblank" then return end
  frames = frames + 1

  -- The load is deferred to between frames by lua_debug.c, so asking on the
  -- first vblank means the machine is in the scene from frame 2 onwards.
  if not loaded then
    loaded = true
    emu.load_state(STATE)
    emu.log("[pop] load_state requested: " .. STATE)
    return
  end

  local _, enabled, vol_l, vol_r, eon, _, _,
        in_l, in_r, out_l, out_r = emu.reverb()

  local ip = absmax(in_l,  in_r)
  local op = absmax(out_l, out_r)

  if ip > win.in_peak  then win.in_peak  = ip end
  if op > win.out_peak then win.out_peak = op end
  if ip >= NEAR_RAIL   then win.in_rail  = win.in_rail  + 1 end
  if op >= NEAR_RAIL   then win.out_rail = win.out_rail + 1 end
  if enabled           then win.reverb_on_frames = win.reverb_on_frames + 1 end
  win.samples = win.samples + 1

  if frames % REPORT_EVERY ~= 0 then return end

  local gen, dropped, used, size, keys = emu.spu_stats()
  -- Fourth value: total_dropped. The third is total_popped, which this used to
  -- report as the XA drop count.
  local _, _, _, cd_drop = emu.cd_audio()
  local _, _, _, _, _, ring_drop, under_ev = emu.audio_stats()

  -- gen is the sample index into the ZS1_AUDIO_DUMP file: the dump is written
  -- one stereo frame per generated sample, so (gen - base_gen) locates this
  -- window in the raw file even though the dump also caught the pre-load boot.
  if not base_gen then
    base_gen = gen
    emu.log(string.format("[pop] dump offset at scene entry: sample %d", gen))
  end

  emu.log(string.format(
    "[pop] t=%5.1fs s=%8d rvb %s eon=%04x vol=%d/%d | in peak=%6d rail=%2d | out peak=%6d rail=%2d | keys=%d ring=%d/%d drop=%d xa_drop=%d ring_drop=%d under=%d",
    frames / 50.0, gen - base_gen,
    win.reverb_on_frames > 0 and "ON " or "off", eon, vol_l, vol_r,
    win.in_peak, win.in_rail, win.out_peak, win.out_rail,
    keys, used, size, dropped, cd_drop, ring_drop, under_ev))

  reset()
end)
