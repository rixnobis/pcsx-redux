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

#include "gui/widgets/gpulogger.h"

#include <algorithm>
#include <iterator>

#include "core/gpulogger.h"
#include "core/psxemulator.h"
#include "core/system.h"
#include "fmt/format.h"
#include "support/imgui-helpers.h"

namespace {

// Draws the outline of a command's write footprint. The footprint comes as triangles, and an edge shared by two
// of them is the inside diagonal of a quad or a rectangle, so only the edges seen once are drawn.
template <typename ToScreen>
void outlineNode(ImDrawList* drawList, PCSX::GPU::Logged& logged, ToScreen&& toScreen, ImU32 color) {
    struct Edge {
        PCSX::OpenGL::ivec2 a, b;
        bool same(const Edge& o) const {
            auto eq = [](PCSX::OpenGL::ivec2 p, PCSX::OpenGL::ivec2 q) { return p.x() == q.x() && p.y() == q.y(); };
            return (eq(a, o.a) && eq(b, o.b)) || (eq(a, o.b) && eq(b, o.a));
        }
    };
    // Large enough for the wrapped copies of a quad; anything past it is simply not deduplicated.
    Edge edges[24];
    unsigned count = 0;
    bool overflow = false;
    logged.getVertices(
        [&](auto v1, auto v2, auto v3) {
            for (auto& e : {Edge{v1, v2}, Edge{v2, v3}, Edge{v3, v1}}) {
                if (count < std::size(edges)) {
                    edges[count++] = e;
                } else {
                    overflow = true;
                    drawList->AddLine(toScreen(e.a), toScreen(e.b), color);
                }
            }
        },
        PCSX::GPU::Logged::PixelOp::WRITE);
    for (unsigned i = 0; i < count; i++) {
        bool shared = false;
        for (unsigned j = 0; j < count; j++) {
            if (i != j && edges[i].same(edges[j])) {
                shared = true;
                break;
            }
        }
        if (!shared || overflow) drawList->AddLine(toScreen(edges[i].a), toScreen(edges[i].b), color);
    }
}

}  // namespace

PCSX::Widgets::GPULogger::GPULogger(bool& show) : m_show(show), m_listener(g_system->m_eventBus) {
    m_listener.listen<Events::GUI::VRAMHover>([this](auto event) {
        if (!m_filterProbing) return;
        m_filter.x = event.x;
        m_filter.y = event.y;
    });
    m_listener.listen<Events::GUI::VRAMClick>([this](auto event) {
        if (!m_filterProbing) return;
        m_filter.x = event.x;
        m_filter.y = event.y;
        m_filterProbing = false;
    });
}

void PCSX::Widgets::GPULogger::draw(PCSX::GPULogger* logger, const char* title, bool replayAllowed) {
    if (!ImGui::Begin(title, &m_show, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    drawMenuBar();
    drawLoggerOptions(logger);
    NodeFlags flags;
    flags.setHighlightRange = m_setHighlightRange;
    m_setHighlightRange = false;
    drawNodeButtons(flags);
    drawHighlightOptions(flags);
    if (logger->followsEmulator()) drawOutputOptions();
    drawVramBreakpoints(logger);

    ImGui::Separator();
    drawFrameStats(logger);
    ImGui::Separator();
    ImGui::BeginChild("DrawCalls");

    GPU::Logged::Origin origin = GPU::Logged::Origin::REPLAY;
    uint32_t value = 0;
    uint32_t length = 0;
    int n = 0;

    for (auto& logged : logger->m_list) {
        if (m_filterEnabled && !logged.isInside(m_filter.x, m_filter.y)) {
            continue;
        }
        ImGui::PushID(n);
        if (m_showOrigins) {
            if ((logged.origin != GPU::Logged::Origin::REPLAY) &&
                ((origin != logged.origin) || (value != logged.value) || (length != logged.length))) {
                origin = logged.origin;
                value = logged.value;
                length = logged.length;
                drawOrigin(logged);
            }
        }
        drawNode(logger, logged, n, flags);
        n++;
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();

    if (m_replay && replayAllowed) {
        logger->replay(logger->gpu());
    }

    m_hoveredNode = flags.tempHighlight;
    logger->highlight(flags.tempHighlight, flags.tempHighlight && ImGui::GetIO().KeyCtrl);
}

void PCSX::Widgets::GPULogger::drawMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(_("Settings"))) {
            auto& colorFormat = m_settings.colorFormat;
            if (ImGui::BeginMenu(_("Color Format"))) {
                if (ImGui::MenuItem(_("None"), nullptr,
                                    colorFormat == GPU::Logged::DrawLogSettings::ColorFormat::None)) {
                    colorFormat = GPU::Logged::DrawLogSettings::ColorFormat::None;
                }
                if (ImGui::MenuItem(_("Expanded"), nullptr,
                                    colorFormat == GPU::Logged::DrawLogSettings::ColorFormat::Expanded)) {
                    colorFormat = GPU::Logged::DrawLogSettings::ColorFormat::Expanded;
                }
                if (ImGui::MenuItem("HTML", nullptr, colorFormat == GPU::Logged::DrawLogSettings::ColorFormat::HTML)) {
                    colorFormat = GPU::Logged::DrawLogSettings::ColorFormat::HTML;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void PCSX::Widgets::GPULogger::drawLoggerOptions(PCSX::GPULogger* logger) {
    if (ImGui::Checkbox(_("GPU logging"), &logger->m_enabled)) {
        if (logger->m_enabled) {
            logger->enable();
        } else {
            logger->disable();
        }
    }
    ImGuiHelpers::ShowHelpMarker(
        _("Logs each frame's draw calls. When enabled, all the commands sent to the GPU will be logged and displayed "
          "here. This will contain only a single frame worth of commands. The feature can be pretty demanding in CPU "
          "and memory."));
    if (logger->followsEmulator()) {
        ImGui::Checkbox(_("Breakpoint on vsync"), &logger->m_breakOnVSync);
        ImGui::SameLine();
        if (ImGui::Button(C_("Emulation", "Resume"))) {
            g_system->resume();
        }
    }
    ImGui::Checkbox(_("Replay frame"), &m_replay);
    ImGuiHelpers::ShowHelpMarker(
        _("When enabled, the framebuffer will be constantly redrawned using the selected commands, allowing to see the "
          "resulting output immediately. This doesn't make sense to have this enabled when: (1) the CPU is running and "
          "(2) the GPU logging isn't enabled. Selection of which commands to replay is done using the first checkbox "
          "in the logger display below. The [T] button will select all commands for replay from the top and until this "
          "command."));
    ImGui::Checkbox(_("Show origins"), &m_showOrigins);
    ImGuiHelpers::ShowHelpMarker(
        _("When enabled, the logger display will also show where did the command come from, which can be useful to "
          "debug or reverse engineer, but will also clutter the logger view."));
}

void PCSX::Widgets::GPULogger::drawNodeButtons(NodeFlags& flags) {
    if (ImGui::BeginTable("Buttons", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(_("Collapse all nodes"))) {
            flags.collapseAll = true;
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Checkbox(_("Keep collapsed"), &m_collapseAll);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(_("Expand all nodes"))) {
            flags.expandAll = true;
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Checkbox(_("Keep expanded"), &m_expandAll);
        ImGui::EndTable();
    }
}

void PCSX::Widgets::GPULogger::drawHighlightOptions(NodeFlags& flags) {
    ImGui::Separator();
    if (ImGui::Button(_("Remove all highlight selections"))) {
        flags.removeHighlight = true;
    }
    ImGui::Checkbox(_("Highlight on hover"), &m_hoverHighlight);
    ImGuiHelpers::ShowHelpMarker(
        _("When enabled, hovering a command in the logger view will highlight it in the vram display. Individual "
          "commands can be selected for highlight by using the second checkbox in the logger view. The [B] and [E] "
          "buttons can be used to specify the beginning and the end of a span of commands to highlight."));
    ImGui::Checkbox(_("Filter by pixel"), &m_filterEnabled);
    ImGuiHelpers::ShowHelpMarker(_(
        "When enabled, only the commands that are related to the specified pixel will be shown. The pixel location is "
        "specified in the next input fields. The [Probe VRAM] button can be used to set the pixel location by hovering "
        "and clicking inside the VRAM viewer or the Output window."));
    ImGui::InputInt2(_("Pixel location"), m_filter.raw);
    if (ImGui::Button(_("Probe VRAM"))) {
        m_filterProbing = true;
    }
    ImGuiHelpers::ShowHelpMarker(_(
        "When enabled, hovering then clicking inside the VRAM viewer or the Output window will set the pixel location "
        "for the filtering."));
}

void PCSX::Widgets::GPULogger::drawOutputOptions() {
    ImGui::Separator();
    ImGui::TextUnformatted(_("Outlines on the Output window:"));
    ImGui::SameLine();
    int outlines = static_cast<int>(m_outputOutlines);
    ImGui::RadioButton(_("Off"), &outlines, static_cast<int>(OutputOutlines::Off));
    ImGui::SameLine();
    ImGui::RadioButton(_("Listed"), &outlines, static_cast<int>(OutputOutlines::Listed));
    ImGui::SameLine();
    ImGui::RadioButton(_("Highlighted"), &outlines, static_cast<int>(OutputOutlines::Highlighted));
    m_outputOutlines = static_cast<OutputOutlines>(outlines);
    ImGuiHelpers::ShowHelpMarker(
        _("Draws the edges of the logged commands over the Output window. Listed draws every command shown in the "
          "logger view below, Highlighted only the selected ones and the one under the mouse when highlight on hover "
          "is enabled. Busy frames can have thousands of commands."));
    const char* maps[] = {_("Auto"), _("Display area"), _("Drawing area")};
    int map = static_cast<int>(m_outputMap);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::Combo(_("Output maps to"), &map, maps, IM_ARRAYSIZE(maps))) {
        m_outputMap = static_cast<OutputMap>(map);
    }
    ImGuiHelpers::ShowHelpMarker(
        _("Which part of VRAM the Output window stands for, for the probe and the outlines. Display area is the "
          "buffer being displayed. A double buffered game displays one buffer while it draws the next frame into the "
          "other one, so the logged frame is usually not the one on screen: Drawing area maps the Output window onto "
          "the same spot of the buffer the frame was drawn into. Auto picks Drawing area when it does not overlap the "
          "displayed buffer."));
}

auto PCSX::Widgets::GPULogger::getOutputMapping(PCSX::GPULogger* logger) -> OutputMapping {
    auto& display = logger->gpu()->m_display;
    // A 24 bits pixel takes one and a half 16 bits VRAM pixels.
    const bool depth24 = display.info.depth == GPU::CtrlDisplayMode::CD_24BITS;
    OutputMapping map{float(display.start.x()), float(display.start.y()), display.size.x() * (depth24 ? 1.5f : 1.0f),
                      float(display.size.y())};
    bool useDrawing = m_outputMap == OutputMap::Drawing;
    if (m_outputMap == OutputMap::Auto) {
        // The drawing area end is inclusive.
        const float x0 = logger->m_drawingAreaX0, y0 = logger->m_drawingAreaY0;
        const float x1 = logger->m_drawingAreaX1 + 1.0f, y1 = logger->m_drawingAreaY1 + 1.0f;
        const bool valid = (x1 > x0) && (y1 > y0);
        const bool overlaps = (x0 < map.x + map.w) && (map.x < x1) && (y0 < map.y + map.h) && (map.y < y1);
        useDrawing = valid && !overlaps;
    }
    if (useDrawing) {
        map.x = logger->m_drawingAreaX0;
        map.y = logger->m_drawingAreaY0;
    }
    return map;
}

void PCSX::Widgets::GPULogger::drawOutputOverlay(PCSX::GPULogger* logger, ImVec2 pos, ImVec2 size, bool hovered) {
    if (!m_show || !logger) return;
    const bool probing = m_filterProbing;
    const bool showFilter = m_filterEnabled || probing;
    if (!probing && !showFilter && (m_outputOutlines == OutputOutlines::Off)) return;
    const auto map = getOutputMapping(logger);
    if ((map.w <= 0.0f) || (map.h <= 0.0f) || (size.x <= 0.0f) || (size.y <= 0.0f)) return;
    const ImVec2 scale(size.x / map.w, size.y / map.h);
    const ImVec2 end(pos.x + size.x, pos.y + size.y);

    if (probing && hovered) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if ((mouse.x >= pos.x) && (mouse.x < end.x) && (mouse.y >= pos.y) && (mouse.y < end.y)) {
            float x = map.x + (mouse.x - pos.x) / scale.x;
            float y = map.y + (mouse.y - pos.y) / scale.y;
            // The display wraps around the right edge of VRAM.
            if (x >= 1024.0f) x -= 1024.0f;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_system->m_eventBus->signal(Events::GUI::VRAMClick{x, y, Events::GUI::VRAM_16BITS});
            } else {
                g_system->m_eventBus->signal(Events::GUI::VRAMHover{x, y, Events::GUI::VRAM_16BITS});
            }
        }
    }

    auto toScreen = [&](OpenGL::ivec2 v) {
        return ImVec2(pos.x + (v.x() - map.x) * scale.x, pos.y + (v.y() - map.y) * scale.y);
    };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(pos, end, true);
    if (m_outputOutlines != OutputOutlines::Off) {
        const ImU32 listedColor = IM_COL32(0, 255, 128, 160);
        const ImU32 highlightColor = IM_COL32(255, 255, 0, 255);
        for (auto& logged : logger->m_list) {
            const bool highlighted = logged.highlight || (&logged == m_hoveredNode);
            if (!highlighted) {
                if (m_outputOutlines == OutputOutlines::Highlighted) continue;
                if (m_filterEnabled && !logged.isInside(m_filter.x, m_filter.y)) continue;
            }
            outlineNode(drawList, logged, toScreen, highlighted ? highlightColor : listedColor);
        }
    }
    if (showFilter) {
        // A cross on the probed pixel, accounting for the display wrapping around the right edge of VRAM.
        float px = m_filter.x + 0.5f;
        if (px < map.x) px += 1024.0f;
        const ImVec2 c(pos.x + (px - map.x) * scale.x, pos.y + (m_filter.y + 0.5f - map.y) * scale.y);
        const float r = std::max(4.0f, std::max(scale.x, scale.y) * 2.0f);
        const ImU32 color = IM_COL32(255, 64, 255, 255);
        drawList->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), color);
        drawList->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), color);
    }
    drawList->PopClipRect();
}

void PCSX::Widgets::GPULogger::drawVramBreakpoints(PCSX::GPULogger* logger) {
    ImGui::Separator();
    // Breakpoints pause the emulator, so they only make sense on the emulator's own logger.
    if (logger->followsEmulator() && ImGui::TreeNode(_("VRAM breakpoints"))) {
        ImGuiHelpers::ShowHelpMarker(
            _("Pauses emulation when a command reads from or writes to one of these VRAM rectangles. Handy to catch "
              "the texture upload that stomped an area, since those are hard to spot in a frame that redraws "
              "everything. The command that triggered gets highlighted in the logger view below. Note the footprints "
              "are unclipped, so a primitive that ends up entirely outside the drawing area still counts as a write."));
        if (ImGui::Button(_("Add breakpoint"))) {
            logger->m_vramBreakpoints.emplace_back();
        }
        if (!logger->m_vramBreakpoints.empty()) {
            ImGui::TextUnformatted(_("Enable, then X, Y, width, height:"));
        }
        unsigned toRemove = logger->m_vramBreakpoints.size();
        for (unsigned i = 0; i < logger->m_vramBreakpoints.size(); i++) {
            auto& bp = logger->m_vramBreakpoints[i];
            ImGui::PushID(i);
            ImGui::Checkbox("##enabled", &bp.enabled);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            ImGui::InputInt4("##area", bp.area.raw);
            ImGui::SameLine();
            ImGui::Checkbox(_("Read"), &bp.onRead);
            ImGui::SameLine();
            ImGui::Checkbox(_("Write"), &bp.onWrite);
            ImGui::SameLine();
            if (ImGui::Button(_("Remove"))) toRemove = i;
            ImGui::PopID();
        }
        if (toRemove != logger->m_vramBreakpoints.size()) {
            logger->m_vramBreakpoints.erase(logger->m_vramBreakpoints.begin() + toRemove);
        }
        ImGui::TreePop();
    }
}

void PCSX::Widgets::GPULogger::drawFrameStats(PCSX::GPULogger* logger) {
    std::string label = fmt::format(f_("Frame {}###FrameCounterNode"), logger->m_frameCounter - m_frameCounterOrigin);
    if (ImGui::TreeNode(label.c_str())) {
        if (ImGui::Button(_("Reset frame counter"))) {
            m_frameCounterOrigin = logger->m_frameCounter;
        }
        ImGui::Text(_("%i primitives"), logger->m_list.size());
        GPU::GPUStats stats;
        for (auto& logged : logger->m_list) {
            logged.cumulateStats(&stats);
        }
        ImGui::Text(_("%i triangles"), stats.triangles);
        ImGui::Text(_("%i textured triangles"), stats.texturedTriangles);
        ImGui::Text(_("%i rectangles"), stats.rectangles);
        ImGui::Text(_("%i sprites"), stats.sprites);
        ImGui::Text(_("%i pixel writes"), stats.pixelWrites);
        ImGui::Text(_("%i pixel reads"), stats.pixelReads);
        ImGui::Text(_("%i texel reads"), stats.texelReads);
        ImGui::Checkbox(_("Overdraw statistics"), &logger->m_overdrawStats);
        ImGuiHelpers::ShowHelpMarker(
            _("Counts how many times each VRAM pixel was touched during the previous frame, from the same buffers as "
              "the overdraw heatmap. Reading them back costs a GPU stall per frame."));
        if (logger->m_overdrawStats && logger->hasOverdrawStats()) {
            auto showOverdraw = [](const char* title, const PCSX::GPULogger::OverdrawStats& o) {
                ImGui::TextUnformatted(title);
                ImGui::Indent();
                ImGui::Text(_("%u pixels, %llu touches, mean %.2f, max %u"), o.pixels, (unsigned long long)o.total,
                            o.mean(), o.max);
                ImGui::Text(_("1x: %u  2x: %u  3-4x: %u  5-8x: %u  9x+: %u"), o.buckets[0], o.buckets[1], o.buckets[2],
                            o.buckets[3], o.buckets[4]);
                ImGui::Unindent();
            };
            showOverdraw(_("Pixels written"), logger->getWrittenOverdraw());
            showOverdraw(_("Pixels read"), logger->getReadOverdraw());
        }

        ImGui::TreePop();
    }
}

void PCSX::Widgets::GPULogger::drawOrigin(GPU::Logged& logged) {
    ImGui::Separator();
    std::string label;
    const auto origin = logged.origin;
    const auto value = logged.value;
    const auto length = logged.length;
    switch (origin) {
        case GPU::Logged::Origin::DATAWRITE:
            ImGui::Text(_("Data port write: %08x"), value);
            break;
        case GPU::Logged::Origin::CTRLWRITE:
            ImGui::Text(_("Control port write: %08x"), value);
            break;
        case GPU::Logged::Origin::DIRECT_DMA:
            ImGui::TextUnformatted(_("Direct DMA from"));
            ImGui::SameLine();
            label = fmt::format("{:08x}", value | 0x80000000);
            if (ImGui::Button(label.c_str())) {
                g_system->m_eventBus->signal(Events::GUI::JumpToMemory{value | 0x80000000, length * 4});
            }
            break;
        case GPU::Logged::Origin::CHAIN_DMA:
            ImGui::TextUnformatted(_("Chain DMA from"));
            ImGui::SameLine();
            label = fmt::format("{:08x}", value | 0x80000000);
            if (ImGui::Button(label.c_str())) {
                g_system->m_eventBus->signal(Events::GUI::JumpToMemory{value | 0x80000000, length * 4 + 4});
            }
            break;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(_("at PC ="));
    ImGui::SameLine();
    label = fmt::format("{:08x}", logged.pc);
    if (ImGui::Button(label.c_str())) {
        g_system->m_eventBus->signal(Events::GUI::JumpToPC{logged.pc});
    }
}

void PCSX::Widgets::GPULogger::drawNode(PCSX::GPULogger* logger, GPU::Logged& logged, int n, NodeFlags& flags) {
    if (flags.disableFromHere) logged.enabled = false;
    if (flags.removeHighlight) logged.highlight = false;
    ImGui::BeginGroup();
    if (!m_replay) ImGui::BeginDisabled();
    ImGui::Checkbox("##enable", &logged.enabled);
    ImGui::SameLine();
    if (ImGui::Button("T")) {
        for (auto& before : logger->m_list) {
            before.enabled = true;
            if (&before == &logged) break;
        }
        flags.disableFromHere = true;
    }
    if (!m_replay) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("##highlight", &logged.highlight);
    ImGui::SameLine();
    if (ImGui::Button("B")) {
        m_setHighlightRange = true;
        m_beginHighlight = n;
    }
    ImGui::SameLine();
    if (ImGui::Button("E")) {
        m_setHighlightRange = true;
        m_endHighlight = n;
    }
    ImGui::SameLine();
    if (logged.editable()) {
        if (ImGui::SmallButton(_("Edit"))) ImGui::OpenPopup("edit");
        if (ImGui::BeginPopup("edit")) {
            // The overlay is only created once a field is edited.
            GPU::Logged::Overlay scratch;
            logged.drawEditor(logged.overlay ? *logged.overlay : scratch);
            if (!logged.overlay && !scratch.empty()) {
                logged.overlay = std::make_unique<GPU::Logged::Overlay>(scratch);
            }
            ImGui::Separator();
            if (ImGui::Button(_("Revert"))) {
                logged.overlay.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    } else {
        // Keep the node names in one column whether the row has an Edit button or not.
        const float width = ImGui::CalcTextSize(_("Edit")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
    }
    if (flags.collapseAll || m_collapseAll) ImGui::SetNextItemOpen(false);
    if (flags.expandAll || m_expandAll) ImGui::SetNextItemOpen(true);
    const bool edited = logged.overlay && !logged.overlay->empty();
    std::string label = fmt::format("{}{}##node{}", edited ? "* " : "", logged.getName(), n);
    if (ImGui::TreeNode(label.c_str())) {
        logged.drawLogNode(n, m_settings);
        ImGui::TreePop();
    }
    ImGui::EndGroup();
    if (m_hoverHighlight && ImGui::IsItemHovered()) {
        flags.tempHighlight = &logged;
    }
    if (flags.setHighlightRange) {
        logged.highlight = (m_beginHighlight <= n) && (n <= m_endHighlight);
    }
    if (logged.highlight) {
        flags.hasHighlight = true;
    }
}
