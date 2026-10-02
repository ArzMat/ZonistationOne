-- Emulated time against host time: the ratio that decides whether the audio
-- device can be fed at the rate it drains. 100% means one emulated second per
-- real second; below that the SPU ring runs dry and the callback plays silence.
--
-- Samples once every 300 vblanks (~6 s) on purpose. A per-vblank probe with
-- ZS1_LOG_STDERR on costs more than the thing it measures — that mistake once
-- produced a "85-95% of real time" figure that the emulator does not actually
-- have when run normally.
--
-- Subscribes to the two notifications it counts and nothing else. Without the
-- names, emu.on_event delivers every notification (one per GTE op, two per
-- polygon, ...) into Lua, which made this probe slow down the emulator it was
-- measuring; with them, the rest are dropped in C before Lua is entered.
local vb, mbs, last = 0, 0, nil
emu.on_event(function(name)
  if name == "mdec_macroblock" then mbs = mbs + 1; return end
  if name ~= "vblank" then return end
  vb = vb + 1
  if vb % 300 ~= 0 then return end
  local cyc, ms = emu.cycles(), emu.host_ms()
  local gen, drop, ring = emu.spu_stats()
  if last then
    local dc = (cyc - last.c) & 0xffffffff
    local dms = ms - last.ms
    local emu_s = dc / 33868800.0
    local host_s = dms / 1000.0
    local prod = (gen - last.g) / host_s
    emu.log(string.format("[SPEED vb=%4d mb=%6d] %.1f%% realtime | %.0f samples/host-sec (need 44100) | ring=%d drop=%d",
      vb, mbs, host_s > 0 and emu_s * 100.0 / host_s or 0, prod, ring, drop))
  end
  last = { c = cyc, ms = ms, g = gen }
end, "vblank", "mdec_macroblock")
emu.log("[HOST SPEED] armed")
