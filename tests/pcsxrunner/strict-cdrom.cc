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

// The same seed and rate over the same read sequence pick the same sectors, and a
// different seed picks a different list.
TEST(StrictCDRom, SeedPicksTheSameSectors) {
    auto first = bootWith(true, c_rate, c_seed);
    auto second = bootWith(true, c_rate, c_seed);
    auto other = bootWith(true, c_rate, c_otherSeed);
    ASSERT_FALSE(first.injected.empty()) << "no sector injected over " << first.sectors << " reads";
    std::string list;
    for (auto lba : first.injected) list += " " + std::to_string(lba);
    printf("strict cd: %llu sectors read, injected at%s\n", (unsigned long long)first.sectors, list.c_str());
    EXPECT_EQ(first.sectors, second.sectors) << "the two boots did not read the same sequence";
    EXPECT_EQ(first.injected, second.injected) << "the same seed picked different sectors";
    EXPECT_FALSE(other.injected.empty());
    EXPECT_NE(first.injected, other.injected) << "a different seed picked the same sectors";
}
