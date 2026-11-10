/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "gtest/gtest.h"
#include "main/main.h"

// Switching the CPU (settings.emulator.Dynarec) or the GPU (settings.emulator.HardwareRenderer)
// backend while a guest runs, through PCSX.applyBackendSettings(). Each switch happens from a luv
// timer, outside Execute(), so it is applied on the spot. Around every switch the script checks
// that two saves in a row are identical (the instrument), that the backend actually changed, and
// that a save taken right after the switch is byte for byte the one taken before it.

// The guest is a xorshift32 loop planted at 0x80100000, keeping its state in $t0 and writing it
// and an iteration count to 0x80100100/0x80100104. Every 4 frames the script reads both, replays
// the same xorshift up to the count, and switches the CPU. A switch that loses a register, the pc
// or memory shows up as a wrong value or a count that stops moving. The value is stored before
// the count, so either step count matches.
static const char c_cpuSwap[] = R"(
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local base = 0x100000 / 4
local code = { 0x3c108010, 0x3c081234, 0x35085678, 0xae000104, 0x00084b40, 0x01094026, 0x00084c42,
    0x01094026, 0x00084940, 0x01094026, 0xae080100, 0x8e0a0104, 0, 0x254a0001, 0xae0a0104, 0x1000fff4, 0 }
local function step(x)
    x = bit.bxor(x, bit.lshift(x, 13))
    x = bit.bxor(x, bit.rshift(x, 17))
    return bit.bxor(x, bit.lshift(x, 5))
end
local n, x, last, swaps, frames = 0, bit.tobit(0x12345678), -1, 0, 0
local function fail(c, msg) print(msg) PCSX.quit(c) end
local function later(f)
    PCSX.pauseEmulator()
    local t = luv.new_timer()
    t:start(1, 0, function() t:close() f() end)
end
-- A timer can still fire from the update() that vsync runs inside Execute(), where the switch is
-- only queued. The emulator is paused, so nothing runs before the main loop applies it.
local function switched(cond, k, tries)
    tries = tries or 0
    if cond() then return k() end
    if tries == 100 then return fail(5, 'backend did not switch') end
    local t = luv.new_timer()
    t:start(1, 0, function() t:close() switched(cond, k, tries + 1) end)
end
local function swap()
    local c, v = tonumber(ram[base + 0x41]), bit.tobit(ram[base + 0x40])
    if c <= last then return fail(2, 'guest stopped at ' .. c) end
    last = c
    while n < c do x = step(x) n = n + 1 end
    if v ~= x and v ~= step(x) then return fail(3, string.format('guest value %08x wrong at %d', v, c)) end
    local want = not PCSX.CPU.isDynarec()
    local a, b = tostring(PCSX.createSaveState()), tostring(PCSX.createSaveState())
    if a ~= b then return fail(4, 'two saves in a row differ') end
    PCSX.settings.emulator.Dynarec = want
    if not PCSX.applyBackendSettings() then return fail(5, 'cpu switch refused') end
    switched(function() return PCSX.CPU.isDynarec() == want end, function()
        if tostring(PCSX.createSaveState()) ~= a then return fail(6, 'state changed across the switch') end
        swaps = swaps + 1
        if swaps == 16 then return PCSX.quit(0) end
        PCSX.resumeEmulator()
    end)
end
LiveSwapListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        for i, w in ipairs(code) do ram[base + i - 1] = w end
        PCSX.invalidateCache()
        later(function() PCSX.getRegisters().pc = 0x80100000 PCSX.resumeEmulator() end)
    elseif frames > 30 and frames % 4 == 0 then
        later(swap)
    end
end)
)";

TEST(LiveSwap, CPUFromInterpreter) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-exec", c_cpuSwap);
    EXPECT_EQ(invoker.invoke(), 0);
}

TEST(LiveSwap, CPUFromDynarec) {
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-dynarec",
                        "-exec", c_cpuSwap);
    EXPECT_EQ(invoker.invoke(), 0);
}

// The GPU half needs the GUI's OpenGL context, so this one runs with a window (xvfb-run in CI).
// Soft -> OpenGL, a blue rectangle drawn by the OpenGL GPU, OpenGL -> soft, a green one drawn by
// the soft GPU; both have to be in VRAM at the end, so VRAM crossed both switches.
static const char c_gpuSwap[] = R"(
local ram = ffi.cast('uint32_t*', PCSX.getMemPtr())
local frames = 0
local function fail(c, msg) print(msg) PCSX.quit(c) end
local function later(f)
    PCSX.pauseEmulator()
    local t = luv.new_timer()
    t:start(1, 0, function() t:close() f() end)
end
-- A timer can still fire from the update() that vsync runs inside Execute(), where the switch is
-- only queued. The emulator is paused, so nothing runs before the main loop applies it.
local function switched(cond, k, tries)
    tries = tries or 0
    if cond() then return k() end
    if tries == 100 then return fail(5, 'backend did not switch') end
    local t = luv.new_timer()
    t:start(1, 0, function() t:close() switched(cond, k, tries + 1) end)
end
local function plant(words)
    local i = 0x100000 / 4
    ram[i] = 0x3c081f80 i = i + 1
    for _, w in ipairs(words) do
        ram[i] = 0x3c090000 + bit.rshift(w, 16) i = i + 1
        ram[i] = 0x35290000 + bit.band(w, 0xffff) i = i + 1
        ram[i] = 0xad091810 i = i + 1
    end
    ram[i] = 0x1000ffff ram[i + 1] = 0
    PCSX.invalidateCache()
    later(function() PCSX.getRegisters().pc = 0x80100000 PCSX.resumeEmulator() end)
end
local function pixel(x, y)
    local v = tostring(PCSX.GPU.getVRAM())
    local o = (y * 1024 + x) * 2 + 1
    return v:byte(o) + v:byte(o + 1) * 256
end
local function swap(want)
    local a, b = tostring(PCSX.createSaveState()), tostring(PCSX.createSaveState())
    if a ~= b then return fail(4, 'two saves in a row differ') end
    PCSX.settings.emulator.HardwareRenderer = want
    if not PCSX.applyBackendSettings() then return fail(5, 'gpu switch refused') end
    switched(function() return PCSX.GPU.isOpenGL() == want end, function()
        if tostring(PCSX.createSaveState()) ~= a then return fail(6, 'state changed across the switch') end
        PCSX.resumeEmulator()
    end)
end
local env = { 0xe1000400, 0xe3000000, 0xe407ffff, 0xe5000000, 0xe6000000 }
LiveSwapListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if frames == 30 then
        later(function() swap(true) end)
    elseif frames == 32 then
        env[6], env[7], env[8] = 0x60ff0000, 0x012c0064, 0x00100010
        plant(env)
    elseif frames == 34 then
        if pixel(104, 304) ~= 0x7c00 then return fail(7, 'OpenGL GPU did not draw') end
        later(function() swap(false) end)
    elseif frames == 36 then
        plant({ 0x6000ff00, 0x012c00c8, 0x00100010 })
    elseif frames == 38 then
        if pixel(104, 304) ~= 0x7c00 then return fail(8, 'VRAM lost across OpenGL -> soft') end
        if pixel(204, 304) ~= 0x03e0 then return fail(9, 'soft GPU did not draw after the switch') end
        PCSX.quit(0)
    end
end)
)";

TEST(LiveSwap, GPUSoftOpenGLSoft) {
    MainInvoker invoker("-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-no-gui-log", "-interpreter",
                        "-softgpu", "-exec", c_gpuSwap);
    EXPECT_EQ(invoker.invoke(), 0);
}
