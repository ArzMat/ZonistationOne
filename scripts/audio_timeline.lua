-- Audio timeline across the whole boot: voice audio, CD (XA) audio, and the
-- two queues between the emulator and the sound device.
--
-- Columns:
--   samples/cycles : SPU production against emulated time (must stay at 0%)
--   ring           : SPU -> SDL queue depth (empty = the device hears silence)
--   drop           : samples generated but discarded because that ring was full
--   cdfifo         : XA queue depth, with pushed/popped/starved since the last line
--   SPUCNT         : bit15 enable, bit7 reverb, bit0 CD audio
--   mb             : MDEC macroblocks — non-zero means the FMV is playing
local CYC = 768
local vb, mbs = 0, 0
local last = nil

emu.on_event(function(name)
  if name == "mdec_macroblock" then mbs = mbs + 1; return end
  if name ~= "vblank" then return end
  vb = vb + 1
  if vb % 50 ~= 0 then return end

  local cyc = emu.cycles()
  local gen, drop, ring, cap, keyon = emu.spu_stats()
  -- emu.cd_audio() returns count, pushed, popped, dropped, starved, SPUCNT,
  -- sectors read, XA sectors (src/core/lua_debug.c, l_emu_cd_audio). This read
  -- skipped "starved", so SPUCNT, sectors and XA sectors were each one column off.
  local cdcount, cdpush, cdpop, cddrop, cdstarve, ctrl, sec, xasec = emu.cd_audio()
  if last then
    local dc = (cyc - last.c) & 0xffffffff
    local ds, dpush, dpop = gen - last.g, cdpush - last.push, cdpop - last.pop
    local dstarve = cdstarve - last.starve
    local dsec, dxa = sec - last.sec, xasec - last.xa
    local secs = dc / 33868800.0
    local exp = dc // CYC
    emu.log(string.format(
      "[AUD vb=%4d mb=%6d] spu %6d/%6d (%+.2f%%) ring=%4d drop=%5d | cd fifo=%6d push=%6d pop=%6d drop=%5d starve=%5d"
      .. " | sectors=%5.1f/s xa=%5.2f/s | SPUCNT=%04x rev=%d cd=%d",
      vb, mbs, ds, exp, exp > 0 and (ds - exp) * 100.0 / exp or 0, ring, drop,
      cdcount, dpush, dpop, cddrop, dstarve,
      dsec / secs, dxa / secs,
      ctrl, (ctrl >> 7) & 1, ctrl & 1))
  end
  last = { c = cyc, g = gen, push = cdpush, pop = cdpop, starve = cdstarve,
           sec = sec, xa = xasec }
end)
emu.log("[AUDIO TIMELINE] armed")
