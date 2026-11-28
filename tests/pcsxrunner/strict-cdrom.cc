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

#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "core/hwevents.h"
#include "gtest/gtest.h"
#include "main/main.h"

// The strict hardware CD-ROM read error injector. Each run boots the test disc
// through OpenBIOS, as the cdrom disc boot test does, and records the data
// sectors read and the ones the injector picked as unreadable.

namespace {

struct StrictSeen {
    uint64_t sectors = 0;
    std::vector<uint32_t> injected;
};

StrictSeen s_strict;

void observeStrict(const PCSX::HWEvent& e) {
    switch (e.type) {
        case PCSX::HWEvents::CDSector:
            s_strict.sectors++;
            break;
        case PCSX::HWEvents::CDReadError:
            s_strict.injected.push_back(e.payload[0]);
            break;
        default:
            break;
    }
}

struct StrictObserverScope {
    StrictObserverScope() {
        s_strict = StrictSeen{};
        PCSX::HWEvents::s_recordObserver = observeStrict;
    }
    ~StrictObserverScope() { PCSX::HWEvents::s_recordObserver = nullptr; }
};

StrictSeen bootWith(bool strict, uint32_t rate, uint32_t seed) {
    StrictObserverScope scope;
    // MainInvoker probes each argument as a path, so a slash comes early.
    std::string lua = "-- /\nlocal emu = PCSX.settings.emulator\n";
    lua += std::string("emu.StrictHardware = ") + (strict ? "true" : "false") + "\n";
    lua += "emu.StrictCDReadErrorRate = " + std::to_string(rate) + "\n";
    lua += "emu.StrictCDReadErrorSeed = " + std::to_string(seed) + "\n";
    lua += "PCSX.HWEvents.arm('CDROM')\n";
    MainInvoker invoker("-no-ui", "-run", "-bios", "src/mips/openbios/openbios.bin", "-testmode", "-interpreter",
                        "-iso", "test.cue", "-exec", lua.c_str(), "-dofile", "tests/lua/cdrom-disc-boot.lua");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0) << "the disc did not boot";
    return s_strict;
}

constexpr uint32_t c_rate = 3;
constexpr uint32_t c_seed = 0x2832;
constexpr uint32_t c_otherSeed = 0x2418;

}  // namespace

// Strict hardware off, with a rate that would pick every sector: nothing is injected.
TEST(StrictCDRom, NothingInjectedWhenOff) {
    auto seen = bootWith(false, 1, c_seed);
    // The OpenBIOS boot of the test disc reads 15 data sectors.
    EXPECT_GE(seen.sectors, 10u) << "the boot read too few sectors for the check to mean anything";
    EXPECT_TRUE(seen.injected.empty()) << seen.injected.size() << " sectors injected with strict hardware off";
}

// Strict hardware on with a rate of 0: nothing is injected either.
TEST(StrictCDRom, NothingInjectedAtRateZero) {
    auto seen = bootWith(true, 0, c_seed);
    EXPECT_GE(seen.sectors, 10u);
    EXPECT_TRUE(seen.injected.empty()) << seen.injected.size() << " sectors injected at rate 0";
}

// The controller's answer to an unreadable sector, driven by tests/lua/strict-cdrom-stall.lua
// from a polling loop in RAM: Setloc 00:02:20, ReadN, with rate 8 and seed 59, whose first pick
// is the fifth fetch, 00:02:24, and whose next one is past the sixteenth.

namespace {

struct StallEvent {
    uint64_t cycle;
    uint16_t type;
    uint8_t value;  // command, or cause
    std::vector<uint8_t> bytes;
    uint32_t position = 0;
};

std::vector<StallEvent> s_stall;

void observeStall(const PCSX::HWEvent& e) {
    StallEvent ev{e.cycle, e.type, 0, {}};
    switch (e.type) {
        case PCSX::HWEvents::CDCommand:
        case PCSX::HWEvents::CDResponse: {
            ev.value = e.payload[0] & 0xff;
            const unsigned count = (e.payload[0] >> 8) & 0xff;
            for (unsigned i = 0; (i < count) && (i < 12); i++)
                ev.bytes.push_back(e.payload[1 + i / 4] >> ((i % 4) * 8));
            break;
        }
        case PCSX::HWEvents::CDSector:
        case PCSX::HWEvents::CDReadError:
            ev.position = e.payload[0];
            break;
        default:
            return;
    }
    s_stall.push_back(std::move(ev));
}

constexpr uint32_t c_stallRate = 8;
constexpr uint32_t c_stallSeed = 59;
constexpr uint32_t c_stallSector = (2 * 75) + 24;  // 00:02:24
constexpr uint64_t c_cyclesPerSecond = 33868800;

std::vector<StallEvent> runStall(const char* scenario, uint32_t rate = c_stallRate, uint32_t seed = c_stallSeed) {
    s_stall.clear();
    PCSX::HWEvents::s_recordObserver = observeStall;
    std::string lua = "-- /\nPCSX.HWEvents.arm('CDROM')\n";
    lua += std::string("StrictScenario = '") + scenario + "'\n";
    lua += "StrictRate = " + std::to_string(rate) + "\n";
    lua += "StrictSeed = " + std::to_string(seed) + "\n";
    lua += "StrictGoodSectors = 4\n";
    MainInvoker invoker("-no-ui", "-run", "-stdout", "-bios", "src/mips/openbios/openbios.bin", "-testmode",
                        "-interpreter", "-iso", "test.cue", "-exec", lua.c_str(), "-dofile",
                        "tests/lua/strict-cdrom-stall.lua");
    int ret = invoker.invoke();
    PCSX::HWEvents::s_recordObserver = nullptr;
    EXPECT_EQ(ret, 0) << "the stall script failed";
    return s_stall;
}

// The index of the first event at or after `from` matching the type and value, or the size.
size_t findEvent(const std::vector<StallEvent>& events, size_t from, uint16_t type, int value = -1) {
    for (size_t i = from; i < events.size(); i++) {
        if ((events[i].type == type) && ((value < 0) || (events[i].value == value))) return i;
    }
    return events.size();
}

// The ReadN right before the injected sector, and the injected sector.
void findStall(const std::vector<StallEvent>& events, size_t& readN, size_t& injected) {
    injected = findEvent(events, 0, PCSX::HWEvents::CDReadError);
    readN = events.size();
    for (size_t i = 0; i < injected; i++) {
        if ((events[i].type == PCSX::HWEvents::CDCommand) && (events[i].value == 6)) readN = i;
    }
}

struct TimeoutRun {
    uint64_t errorCycle = 0;
    uint64_t sinceReadN = 0;
};

TimeoutRun checkTimeout() {
    TimeoutRun run;
    auto events = runStall("timeout");
    size_t readN, injected;
    findStall(events, readN, injected);
    EXPECT_LT(injected, events.size()) << "nothing injected";
    EXPECT_LT(readN, events.size()) << "no ReadN before the injected sector";
    if ((injected >= events.size()) || (readN >= events.size())) return run;
    EXPECT_EQ(events[injected].position, c_stallSector);
    // The four sectors before it are delivered.
    std::vector<uint32_t> delivered;
    for (size_t i = readN; i < injected; i++) {
        if (events[i].type == PCSX::HWEvents::CDSector) delivered.push_back(events[i].position);
        if ((events[i].type == PCSX::HWEvents::CDResponse) && (events[i].value == 1)) {
            EXPECT_EQ(events[i].bytes, std::vector<uint8_t>{0x22});
        }
    }
    EXPECT_EQ(delivered,
              (std::vector<uint32_t>{c_stallSector - 4, c_stallSector - 3, c_stallSector - 2, c_stallSector - 1}));
    const size_t error = findEvent(events, injected, PCSX::HWEvents::CDResponse, 5);
    EXPECT_LT(error, events.size()) << "no INT5";
    if (error >= events.size()) return run;
    // Silent until the error: no sector and no INT1.
    for (size_t i = injected + 1; i < error; i++) {
        EXPECT_NE(events[i].type, PCSX::HWEvents::CDSector) << "sector " << events[i].position << " during the stall";
        if (events[i].type == PCSX::HWEvents::CDResponse) {
            EXPECT_NE(events[i].value, 1) << "INT1 during the stall";
        }
    }
    EXPECT_EQ(events[error].bytes, (std::vector<uint8_t>{0x06, 0x04}));
    run.errorCycle = events[error].cycle;
    run.sinceReadN = events[error].cycle - events[readN].cycle;
    EXPECT_GE(run.sinceReadN, 30 * c_cyclesPerSecond);
    EXPECT_LE(run.sinceReadN, 51 * c_cyclesPerSecond);
    printf("strict cd: INT5 06 04 at cycle %llu, %.3f s after the ReadN\n", (unsigned long long)run.errorCycle,
           double(run.sinceReadN) / c_cyclesPerSecond);
    // GetlocL has the header of the sector 6 before: 00:02:18.
    const size_t getlocL = findEvent(events, error, PCSX::HWEvents::CDCommand, 16);
    const size_t locL = findEvent(events, getlocL, PCSX::HWEvents::CDResponse, 3);
    EXPECT_LT(locL, events.size());
    if (locL < events.size()) {
        EXPECT_GE(events[locL].bytes.size(), 3u);
        EXPECT_EQ(std::vector<uint8_t>(events[locL].bytes.begin(), events[locL].bytes.begin() + 3),
                  (std::vector<uint8_t>{0x00, 0x02, 0x18}));
    }
    // A Nop right after still has the motor on.
    const size_t nop = findEvent(events, locL, PCSX::HWEvents::CDCommand, 1);
    const size_t nopAck = findEvent(events, nop, PCSX::HWEvents::CDResponse, 3);
    EXPECT_LT(nopAck, events.size());
    if (nopAck < events.size()) EXPECT_EQ(events[nopAck].bytes, std::vector<uint8_t>{0x02});
    // Then the spindle is stopped: Pause 00h then 00h, Setloc and ReadN acknowledge 00h.
    const size_t pause = findEvent(events, nopAck, PCSX::HWEvents::CDCommand, 9);
    const size_t pauseAck = findEvent(events, pause, PCSX::HWEvents::CDResponse, 3);
    const size_t pauseDone = findEvent(events, pause, PCSX::HWEvents::CDResponse, 2);
    EXPECT_LT(pauseDone, events.size());
    if (pauseDone < events.size()) {
        EXPECT_EQ(events[pauseAck].bytes, std::vector<uint8_t>{0x00});
        EXPECT_EQ(events[pauseDone].bytes, std::vector<uint8_t>{0x00});
    }
    const size_t setloc = findEvent(events, pauseDone, PCSX::HWEvents::CDCommand, 2);
    const size_t setlocAck = findEvent(events, setloc, PCSX::HWEvents::CDResponse, 3);
    const size_t readAgain = findEvent(events, setlocAck, PCSX::HWEvents::CDCommand, 6);
    const size_t readAck = findEvent(events, readAgain, PCSX::HWEvents::CDResponse, 3);
    EXPECT_LT(readAck, events.size());
    if (readAck < events.size()) {
        EXPECT_EQ(events[setlocAck].bytes, std::vector<uint8_t>{0x00});
        EXPECT_EQ(events[readAck].bytes, std::vector<uint8_t>{0x00});
    }
    // And the read after it delivers.
    const size_t sector = findEvent(events, readAck, PCSX::HWEvents::CDSector);
    EXPECT_LT(sector, events.size()) << "no sector after the error";
    if (sector < events.size()) EXPECT_EQ(events[sector].position, c_stallSector - 4);
    return run;
}

}  // namespace

// An unreadable sector is not delivered, the drive is silent until INT5 06h 04h 30 to 51 s after
// the ReadN, the same number of cycles after it for the same seed, then GetlocL reports 6 sectors
// back and the motor stops. The ReadN's own cycle is not compared: the script starts at a vsync
// once the disc has booted, and the boot can end a frame apart after other tests in the process.
TEST(StrictCDRom, UnreadableSectorTimesOut) {
    auto first = checkTimeout();
    auto second = checkTimeout();
    EXPECT_NE(first.sinceReadN, 0u);
    EXPECT_EQ(first.sinceReadN, second.sinceReadN) << "the same seed gave the error at a different time";
}

// The same seed and rate over the same read sequence pick the same sectors, and a different
// seed picks a different list. A pick stalls the drive, so this reads from a Setloc rather
// than through a disc boot, which an unreadable sector now stops.
TEST(StrictCDRom, SeedPicksTheSameSectors) {
    auto picks = [](uint32_t seed) {
        StrictSeen seen;
        for (auto& e : runStall("pick", c_rate, seed)) {
            if (e.type == PCSX::HWEvents::CDSector) seen.sectors++;
            if (e.type == PCSX::HWEvents::CDReadError) seen.injected.push_back(e.position);
        }
        return seen;
    };
    auto first = picks(c_seed);
    auto second = picks(c_seed);
    auto other = picks(c_otherSeed);
    ASSERT_FALSE(first.injected.empty()) << "no sector injected over " << first.sectors << " reads";
    std::string list;
    for (auto lba : first.injected) list += " " + std::to_string(lba);
    printf("strict cd: %llu sectors read, injected at%s\n", (unsigned long long)first.sectors, list.c_str());
    EXPECT_EQ(first.sectors, second.sectors) << "the two runs did not read the same sequence";
    EXPECT_EQ(first.injected, second.injected) << "the same seed picked different sectors";
    EXPECT_FALSE(other.injected.empty());
    EXPECT_NE(first.injected, other.injected) << "a different seed picked the same sectors";
}

// A Pause during the stall is refused with 43h 80h, and the drive stays silent.
TEST(StrictCDRom, PauseDuringStallIsRefused) {
    auto events = runStall("pause");
    size_t readN, injected;
    findStall(events, readN, injected);
    ASSERT_LT(injected, events.size()) << "nothing injected";
    EXPECT_EQ(events[injected].position, c_stallSector);
    const size_t pause = findEvent(events, injected, PCSX::HWEvents::CDCommand, 9);
    ASSERT_LT(pause, events.size()) << "no Pause";
    const size_t answer = findEvent(events, pause, PCSX::HWEvents::CDResponse);
    ASSERT_LT(answer, events.size()) << "no answer to the Pause";
    EXPECT_EQ(events[answer].value, 5);
    EXPECT_EQ(events[answer].bytes, (std::vector<uint8_t>{0x43, 0x80}));
    for (size_t i = injected + 1; i < events.size(); i++) {
        EXPECT_NE(events[i].type, PCSX::HWEvents::CDSector) << "sector " << events[i].position << " after the stall";
        if ((i != answer) && (events[i].type == PCSX::HWEvents::CDResponse)) {
            ADD_FAILURE() << "cause " << int(events[i].value) << " after the Pause was refused";
        }
    }
}
