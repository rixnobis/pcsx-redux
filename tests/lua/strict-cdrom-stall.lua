-- Drives the CD-ROM controller from a polling loop poked into RAM, once the test disc has booted.
-- The loop logs every response (cause, byte count, bytes) and acknowledges it, and sends the
-- command Lua leaves in its mailbox. Strict hardware is turned on right before the ReadN, so the
-- injector's fetch count starts at that read. StrictScenario (set through -exec) is 'timeout'
-- 'pause' or 'pick'; strict-cdrom.cc checks the responses through the hardware events.
--   timeout: Setloc, ReadN, wait for the INT5, then GetlocL, Nop, Pause, Setloc, ReadN, one INT1.
--   pause:   Setloc, ReadN, wait for the stall, Pause, then two seconds with nothing.
--   pick:    Setloc, ReadN, then sectors until half a second goes by without any.
local ram = ffi.cast('uint8_t*', PCSX.getMemPtr())
local words = ffi.cast('uint32_t*', PCSX.getMemPtr())
local box = 0x110000
local logCount = (box + 0x100) / 4
local logBase = box + 0x200

local loop = [[
    mfc0  $t1, $sr
    nop
    addiu $t2, $zero, -2
    and   $t1, $t1, $t2
    mtc0  $t1, $sr
    nop
    lui   $t0, 0xbf80
    lui   $s0, 0x8011
loop:
    li    $t1, 1
    sb    $t1, 0x1800($t0)
    lbu   $t2, 0x1803($t0)
    nop
    andi  $t2, $t2, 7
    beq   $t2, $zero, mailbox
    nop
    lw    $t3, 0x100($s0)
    nop
    sll   $t4, $t3, 4
    addu  $t4, $t4, $s0
    addiu $t4, $t4, 0x200
    sb    $t2, 0($t4)
    li    $t5, 0
bytes:
    lbu   $t6, 0x1800($t0)
    nop
    andi  $t6, $t6, 0x20
    beq   $t6, $zero, logged
    nop
    lbu   $t7, 0x1801($t0)
    addu  $t8, $t4, $t5
    sb    $t7, 2($t8)
    addiu $t5, $t5, 1
    sltiu $t9, $t5, 14
    bne   $t9, $zero, bytes
    nop
logged:
    sb    $t5, 1($t4)
    li    $t1, 1
    sb    $t1, 0x1800($t0)
    li    $t1, 0x1f
    sb    $t1, 0x1803($t0)
    addiu $t3, $t3, 1
    sw    $t3, 0x100($s0)
mailbox:
    lw    $t1, 0($s0)
    nop
    beq   $t1, $zero, loop
    nop
    sb    $zero, 0x1800($t0)
    lw    $t2, 4($s0)
    addiu $t3, $s0, 8
params:
    beq   $t2, $zero, send
    nop
    lbu   $t4, 0($t3)
    addiu $t3, $t3, 1
    sb    $t4, 0x1802($t0)
    b     params
    addiu $t2, $t2, -1
send:
    sb    $t1, 0x1801($t0)
    sw    $zero, 0($s0)
    b     loop
    nop
]]

-- The assembler takes no local labels: every line is one word here, so resolve them up front.
local function resolveLabels(src, base)
    local labels, lines, pc = {}, {}, base
    for line in src:gmatch('[^\n]+') do
        local label = line:match('^(%w+):$')
        if label then
            labels[label] = pc
        else
            lines[#lines + 1] = line
            pc = pc + 4
        end
    end
    for i, line in ipairs(lines) do
        lines[i] = line:gsub('(%w+)$', function(w)
            if labels[w] then return string.format('@0x%08x', labels[w]) end
        end)
    end
    return table.concat(lines, '\n')
end

local f = assert(io.open('src/mips/monitor/hosts/retail/monitor-retail.ps-exe', 'rb'))
local exe = f:read('*a')
f:close()
local function u32(o)
    local a, b, c, d = exe:byte(o + 1, o + 4)
    return a + b * 0x100 + c * 0x10000 + d * 0x1000000
end
local tAddr, tSize = u32(0x18), u32(0x1c)
local text = exe:sub(0x801, 0x800 + tSize)

local seen = 0
local function nextResponse()
    if words[logCount] <= seen then return nil end
    local e = logBase + seen * 16
    seen = seen + 1
    local r = { cause = ram[e], bytes = {} }
    for i = 1, ram[e + 1] do r.bytes[i] = ram[e + 1 + i] end
    return r
end

local function send(cmd, ...)
    local params = { ... }
    for i, p in ipairs(params) do ram[box + 7 + i] = p end
    words[box / 4 + 1] = #params
    words[box / 4] = cmd
end

local function fail(code, msg)
    print('strict cd stall: ' .. msg)
    PCSX.quit(code)
    while true do coroutine.yield() end
end

-- Waits up to `frames` vsyncs for a response with this cause, failing on any other cause.
local function expect(cause, frames, what)
    for _ = 1, frames do
        local r = nextResponse()
        if r then
            if r.cause ~= cause then fail(3, what .. ': cause ' .. r.cause .. ' instead of ' .. cause) end
            return r
        end
        coroutine.yield()
    end
    fail(4, what .. ': no response')
end

-- Waits `frames` vsyncs, failing if any response comes.
local function silence(frames, what)
    for _ = 1, frames do
        local r = nextResponse()
        if r then fail(5, what .. ': cause ' .. r.cause .. ' during the silence') end
        coroutine.yield()
    end
end

local script = coroutine.create(function()
    -- Read here rather than at load: the -exec chunk that sets these may run after this file.
    local scenario = StrictScenario or 'timeout'
    send(9)
    expect(3, 60, 'Pause')
    expect(2, 60, 'Pause complete')
    send(14, 0x80)
    expect(3, 60, 'Setmode')
    send(2, 0x00, 0x02, 0x20)
    expect(3, 60, 'Setloc')
    local emu = PCSX.settings.emulator
    emu.StrictHardware = true
    emu.StrictCDReadErrorRate = StrictRate
    emu.StrictCDReadErrorSeed = StrictSeed
    send(6)
    expect(3, 60, 'ReadN')
    if scenario == 'pick' then
        -- Whatever the seed picks: sectors until the drive goes quiet for half a second.
        local quiet = 0
        while quiet < 30 do
            local r = nextResponse()
            if r then
                if r.cause ~= 1 then fail(3, 'cause ' .. r.cause .. ' while reading') end
                quiet = 0
            else
                quiet = quiet + 1
                coroutine.yield()
            end
        end
        PCSX.quit(0)
        return
    end
    for i = 1, StrictGoodSectors do expect(1, 60, 'sector ' .. i) end
    if scenario == 'pause' then
        silence(30, 'stall')
        send(9)
        expect(5, 60, 'Pause during the stall')
        silence(120, 'after the Pause')
        PCSX.quit(0)
        return
    end
    expect(5, 60 * 60, 'timeout error')
    send(16)
    expect(3, 60, 'GetlocL')
    send(1)
    expect(3, 60, 'Nop')
    silence(30, 'after the error')
    send(9)
    expect(3, 60, 'Pause after the error')
    expect(2, 60, 'Pause complete after the error')
    send(2, 0x00, 0x02, 0x20)
    expect(3, 60, 'Setloc after the error')
    send(6)
    expect(3, 60, 'ReadN after the error')
    expect(1, 60 * 30, 'sector after the error')
    PCSX.quit(0)
end)

local frames = 0
local started = false
StrictStallListener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if not started then
        local pc = PCSX.getRegisters().pc
        if pc >= tAddr and pc < tAddr + tSize then
            if ffi.string(PCSX.getMemPtr() + bit.band(tAddr, 0x1fffff), tSize) == text then
                started = true
                for i = 0, 0x3ff do ram[box + i] = 0 end
                PCSX.Assembler.New():parse(resolveLabels(loop, 0x80100000)):compileToMemory(PCSX.getMemPtr(), 0x80100000, 0x80000000)
                PCSX.getRegisters().pc = 0x80100000
            end
        end
        if frames >= 6000 then fail(1, 'the disc did not boot') end
        return
    end
    local ok, err = coroutine.resume(script)
    if not ok then
        print('strict cd stall: ' .. tostring(err))
        PCSX.quit(2)
    end
end)
