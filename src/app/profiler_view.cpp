#include "app/profiler_view.h"

#include <algorithm>
#include <cstdio>

#include "core/profiler.h"
#include "imgui.h"

namespace {

ImU32 scopeColor(const char* name) {   // a steady colour per name
    unsigned h = 2166136261u;
    for (const char* c = name; *c; ++c) h = (h ^ (unsigned char)*c) * 16777619u;
    return IM_COL32(80 + (h & 0x7F), 80 + ((h >> 8) & 0x7F), 110 + ((h >> 16) & 0x7F), 255);
}

}  // namespace

void drawProfilerWindow(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(560.f, 460.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Profiler", open)) { ImGui::End(); return; }
    const std::vector<profiler::Frame>& frames = profiler::history();
    if (frames.empty()) {
        ImGui::TextDisabled("No frames yet");
        ImGui::End();
        return;
    }
    // Frame times
    std::vector<float> ms;
    float worst = 0.f, sum = 0.f;
    for (const profiler::Frame& f : frames) {
        ms.push_back((float)f.ms);
        worst = std::max(worst, (float)f.ms);
        sum += (float)f.ms;
    }
    char label[96];
    std::snprintf(label, sizeof(label), "avg %.2f ms  worst %.2f ms", sum / (float)ms.size(), worst);
    ImGui::PlotLines("##frames", ms.data(), (int)ms.size(), 0, label, 0.f, std::max(worst, 16.7f) * 1.1f,
                     ImVec2(-1.f, 60.f));

    // The last frame: scopes as bars, nested ones below their parents
    const profiler::Frame& last = frames.back();
    ImGui::SeparatorText("Last frame (CPU, main thread)");
    int depth = 0;
    for (const profiler::Scope& s : last.scopes) depth = std::max(depth, s.depth + 1);
    float rowH = ImGui::GetTextLineHeight() + 4.f, width = ImGui::GetContentRegionAvail().x;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    double span = std::max(last.ms, 0.001);
    for (const profiler::Scope& s : last.scopes) {
        float x0 = origin.x + (float)(s.start / span) * width, x1 = origin.x + (float)((s.start + s.ms) / span) * width;
        float y0 = origin.y + s.depth * rowH;
        ImVec2 a(x0, y0), b(std::max(x1, x0 + 1.f), y0 + rowH - 2.f);
        dl->AddRectFilled(a, b, scopeColor(s.name), 2.f);
        dl->PushClipRect(a, b, true);
        char text[96];
        std::snprintf(text, sizeof(text), "%s %.2f", s.name, s.ms);
        dl->AddText(ImVec2(x0 + 3.f, y0 + 1.f), IM_COL32(15, 15, 20, 255), text);
        dl->PopClipRect();
        if (ImGui::IsMouseHoveringRect(a, b)) ImGui::SetTooltip("%s: %.3f ms", s.name, s.ms);
    }
    ImGui::Dummy(ImVec2(width, std::max(depth, 1) * rowH));

    // Averages over the history
    ImGui::SeparatorText("Per frame, over the last 2 s");
    if (ImGui::BeginTable("##scopes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Scope");
        ImGui::TableSetupColumn("avg ms");
        ImGui::TableSetupColumn("worst ms");
        ImGui::TableHeadersRow();
        for (const profiler::Summary& s : profiler::summarize()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Indent((float)s.depth * 12.f + 0.001f);
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::Unindent((float)s.depth * 12.f + 0.001f);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s.avgMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s.maxMs);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("GPU passes");
    const auto& gpu = profiler::gpuTimes();
    if (gpu.empty()) ImGui::TextDisabled("(no timestamps from this GPU)");
    float total = 0.f;
    for (const auto& [name, t] : gpu) {
        ImGui::Text("%-22s %7.3f ms", name.c_str(), t);
        total += t;
    }
    if (!gpu.empty()) ImGui::Text("%-22s %7.3f ms", "total", total);
    ImGui::End();
}

void drawProfilerOverlay(float scale) {
    const std::vector<profiler::Frame>& frames = profiler::history();
    if (frames.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(10.f * scale, 40.f * scale), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##profiler-overlay", nullptr, flags);
    double sum = 0.0;
    for (const profiler::Frame& f : frames) sum += f.ms;
    ImGui::Text("frame %.2f ms (F3 hides)", sum / (double)frames.size());
    for (const profiler::Summary& s : profiler::summarize())
        if (s.depth <= 1) ImGui::Text("%*s%-18s %6.2f  max %6.2f", s.depth * 2, "", s.name.c_str(), s.avgMs, s.maxMs);
    for (const auto& [name, t] : profiler::gpuTimes()) ImGui::Text("GPU %-18s %6.2f", name.c_str(), t);
    ImGui::End();
}
