/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#pragma once

#include <stdint.h>

#include <limits>

#include "core/gpu.h"
#include "imgui.h"
#include "support/eventbus.h"

namespace PCSX {
class GPULogger;
namespace Widgets {

class GPULogger {
  public:
    GPULogger(bool& show);
    // replayAllowed: whether the GPU the logger records is stopped, so a replay will not be overwritten.
    void draw(PCSX::GPULogger* logger, const char* title, bool replayAllowed);
    // Called from inside the Output window, right after the frame image was drawn at pos with the given size.
    // Feeds the pixel probe from the mouse and outlines the logged commands over the image.
    void drawOutputOverlay(PCSX::GPULogger* logger, ImVec2 pos, ImVec2 size, bool hovered);

    bool& m_show;

  private:
    // State gathered while drawing the node list, shared by the per-node helpers.
    struct NodeFlags {
        bool collapseAll = false;
        bool expandAll = false;
        bool disableFromHere = false;
        bool removeHighlight = false;
        bool setHighlightRange = false;
        GPU::Logged* tempHighlight = nullptr;
        bool hasHighlight = false;
    };
    void drawMenuBar();
    void drawLoggerOptions(PCSX::GPULogger* logger);
    void drawNodeButtons(NodeFlags& flags);
    void drawHighlightOptions(NodeFlags& flags);
    void drawVramBreakpoints(PCSX::GPULogger* logger);
    void drawFrameStats(PCSX::GPULogger* logger);
    void drawOrigin(GPU::Logged& logged);
    void drawNode(PCSX::GPULogger* logger, GPU::Logged& logged, int n, NodeFlags& flags);
    void drawOutputOptions();
    // The VRAM rectangle the Output window image stands for, in 16 bits pixels.
    struct OutputMapping {
        float x, y, w, h;
    };
    OutputMapping getOutputMapping(PCSX::GPULogger* logger);

    bool m_replay = false;
    bool m_showOrigins = false;
    bool m_expandAll = false;
    bool m_collapseAll = false;
    bool m_setHighlightRange = false;
    bool m_hoverHighlight = false;
    uint64_t m_frameCounterOrigin = 0;
    unsigned m_beginHighlight = 0;
    unsigned m_endHighlight = std::numeric_limits<unsigned>::max();
    union {
        int raw[2];
        struct {
            int x, y;
        };
    } m_filter = {0, 0};
    bool m_filterEnabled = false;
    bool m_filterProbing = false;
    enum class OutputOutlines { Off, Listed, Highlighted };
    OutputOutlines m_outputOutlines = OutputOutlines::Off;
    // Display area: the image shows the VRAM area being displayed. Drawing area: the same spot of the buffer the
    // logged frame drew into, for double buffered games that display one buffer while drawing the other. Auto picks
    // the drawing area when it does not overlap the displayed area.
    enum class OutputMap { Auto, Display, Drawing };
    OutputMap m_outputMap = OutputMap::Auto;
    // Only compared against live nodes, never dereferenced: the list can change between two GUI frames.
    const GPU::Logged* m_hoveredNode = nullptr;
    EventBus::Listener m_listener;
    GPU::Logged::DrawLogSettings m_settings;
};

}  // namespace Widgets
}  // namespace PCSX
