-- Reference capture from MAME for the jak_batm harness.
-- Env:
--   CAP_DIR     output directory (must exist)
--   CAP_FRAMES  frames to run (default 600)
--   CAP_TRACE   if set, also write an instruction trace (trace.tr) for the whole run; needs -debug
-- Writes:
--   io.log      one line per peripheral access: "<frame> R|W <addr> <data>", where <frame> is
--               the number of the vblank that ends the frame the access happened in (from 1)
--   frames.bin  per vblank: u32 vblank number (time / 1/60 s, 0 = power-on), u64 time-in-27MHz-cycles, then words 0x0000-0x27FF (RAM) and
--               0x2900-0x2FFF (scroll/hcomp/palette/sprite RAM), raw from read_range
local dir = os.getenv("CAP_DIR") or "."
local nframes = tonumber(os.getenv("CAP_FRAMES") or "600")
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]

local io_out = assert(io.open(dir .. "/io.log", "w"))
local fr_out = assert(io.open(dir .. "/frames.bin", "wb"))
-- labels come from emulated time: MAME calls frame_done at t=0 only when -debug is on
local frame = 1

local ranges = { {0x2800, 0x28ff}, {0x3000, 0x37ff}, {0x3d00, 0x3e03} }
-- taps must stay referenced or they are garbage-collected and silently removed
TAPS = {}
for i, r in ipairs(ranges) do
  TAPS[#TAPS + 1] = space:install_read_tap(r[1], r[2], "capr" .. i, function(off, data, mask)
    io_out:write(string.format("%d R %04X %04X\n", frame, off, data))
  end)
  TAPS[#TAPS + 1] = space:install_write_tap(r[1], r[2], "capw" .. i, function(off, data, mask)
    io_out:write(string.format("%d W %04X %04X\n", frame, off, data))
  end)
end

local started = false
emu.register_frame_done(function()
  if not started then
    started = true
    if os.getenv("CAP_TRACE") then
      machine.debugger:command('trace ' .. dir .. '/trace.tr,maincpu,noloop,{tracelog "R1=%04X R2=%04X R3=%04X R4=%04X BP=%04X SP=%04X SR=%04X ",r1,r2,r3,r4,bp,sp,sr}')
      machine.debugger:command('go')
    end
  end
  local t = machine.time
  local cycles = t:as_ticks(27000000)
  local vbl = (cycles + 225000) // 450000
  fr_out:write(string.pack("<I4I8", vbl, cycles))
  fr_out:write(space:read_range(0x0000, 0x27ff, 16))
  fr_out:write(space:read_range(0x2900, 0x2fff, 16))
  frame = vbl + 1
  if vbl >= nframes then
    io_out:close()
    fr_out:close()
    machine:exit()
  end
end)
