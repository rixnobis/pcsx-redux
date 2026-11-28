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

// GetlocP's SubQ comes from CDRIso::getLocP. A sector listed in the image's SBI file must not
// report its position, which is what LibCrypt reads its key from.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "cdrom/cdriso.h"
#include "core/arguments.h"
#include "core/commandline.h"
#include "core/psxemulator.h"
#include "core/system.h"
#include "gtest/gtest.h"
#include "support/uvfile.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {

const char* const c_argv[] = {"pcsx-redux", "-testmode"};

class TestSystem final : public PCSX::System {
  public:
    TestSystem() : m_commandLine(2, c_argv), m_args(m_commandLine) {}
    void softReset() override {}
    void hardReset() override {}
    void biosPutc(int) override {}
    const PCSX::Arguments& getArgs() const override { return m_args; }
    void printf(std::string&&) override {}
    void log(PCSX::LogClass, std::string&&) override {}
    void message(std::string&&) override {}
    void luaMessage(const std::string&, bool) override {}
    void update(bool) override {}
    void close() override {}
    void purgeAllEvents() override {}
    void testQuit(int) override {}

  private:
    PCSX::CommandLine m_commandLine;
    PCSX::Arguments m_args;
};

class CDRIsoSBI : public ::testing::Test {
  protected:
    void SetUp() override {
        m_oldSystem = PCSX::g_system;
        m_oldEmulator = PCSX::g_emulator;
        PCSX::g_system = &m_system;
        m_emulator = std::make_unique<PCSX::Emulator>();
        PCSX::g_emulator = m_emulator.get();
        m_dir = std::filesystem::temp_directory_path() / ("pcsx-redux-sbi-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir);
        // 300 raw sectors of a single data track, from 00:02:00.
        std::ofstream(m_dir / "disc.bin", std::ios::binary) << std::string(300 * 2352, '\0');
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
        m_emulator.reset();
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }
    // One redump-style entry: BCD MSF, type 1, ten bytes of replacement SubQ.
    void writeSBI(uint8_t m, uint8_t s, uint8_t f) {
        std::string sbi("SBI\0", 4);
        sbi += std::string{char(m), char(s), char(f), char(1)};
        sbi += std::string(10, '\x41');
        std::ofstream(m_dir / "disc.sbi", std::ios::binary) << sbi;
    }

    // CDRIso reads through UvFile, which needs the uv thread.
    PCSX::UvThreadOp::UvThread m_uvThread;
    TestSystem m_system;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    std::filesystem::path m_dir;
};

}  // namespace

TEST_F(CDRIsoSBI, ListedSectorReportsNoPosition) {
    writeSBI(0x00, 0x02, 0x16);
    PCSX::CDRIso iso(m_dir / "disc.bin");
    ASSERT_FALSE(iso.failed());

    uint8_t locP[8];
    ASSERT_TRUE(iso.getLocP(PCSX::IEC60908b::MSF{0, 2, 16}, locP));
    EXPECT_EQ(locP[0], 0x01);  // track
    EXPECT_EQ(locP[1], 0x01);  // index
    for (unsigned i = 2; i < 8; i++) EXPECT_EQ(locP[i], 0) << "byte " << i;

    // The sectors around it keep their position.
    ASSERT_TRUE(iso.getLocP(PCSX::IEC60908b::MSF{0, 2, 15}, locP));
    EXPECT_EQ(locP[5], 0x00);
    EXPECT_EQ(locP[6], 0x02);
    EXPECT_EQ(locP[7], 0x15);
    EXPECT_EQ(locP[4], 0x15);  // relative frame, track 1 starts at 00:02:00
    ASSERT_TRUE(iso.getLocP(PCSX::IEC60908b::MSF{0, 2, 17}, locP));
    EXPECT_EQ(locP[7], 0x17);
}

TEST_F(CDRIsoSBI, NoSBIFileReportsEveryPosition) {
    PCSX::CDRIso iso(m_dir / "disc.bin");
    ASSERT_FALSE(iso.failed());

    uint8_t locP[8];
    ASSERT_TRUE(iso.getLocP(PCSX::IEC60908b::MSF{0, 2, 16}, locP));
    EXPECT_EQ(locP[5], 0x00);
    EXPECT_EQ(locP[6], 0x02);
    EXPECT_EQ(locP[7], 0x16);
}
