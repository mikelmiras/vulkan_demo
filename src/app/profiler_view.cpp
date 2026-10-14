#include "app/profiler_view.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "core/profiler.h"
#include "imgui.h"
#include "script/lua_script.h"

namespace {

ImU32 scopeColor(const char* name) {   // a steady colour per name
    unsigned h = 2166136261u;
    for (const char* c = name; *c; ++c) h = (h ^ (unsigned char)*c) * 16777619u;
    return IM_COL32(80 + (h & 0x7F), 80 + ((h >> 8) & 0x7F), 110 + ((h >> 16) & 0x7F), 255);
}

std::string saveTrace(const std::filesystem::path& folder, const std::vector<profiler::Frame>& frames) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::time_t now = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof(name), "trace-%Y%m%d-%H%M%S.json", std::localtime(&now));
    std::string error;
    std::filesystem::path file = folder / name;
    if (!profiler::saveChromeTrace(file, frames, error)) return error;
    return "Saved " + std::to_string(frames.size()) + " frames to " + file.u8string() + " (open it in chrome://tracing)";
}

}  // namespace

void drawProfilerWindow(bool* open, const std::filesystem::path& traceFolder) {
    ImGui::SetNextWindowSize(ImVec2(560.f, 460.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Profiler", open)) { ImGui::End(); return; }
    static std::string traceMessage;
    if (ImGui::Button("Save trace")) traceMessage = saveTrace(traceFolder, profiler::history());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The last 2 s as a Chrome trace (chrome://tracing, ui.perfetto.dev)");
    ImGui::SameLine();
    if (!profiler::recording()) {
        if (ImGui::Button("Record")) {
            profiler::startRecording();
            traceMessage.clear();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps every frame until Stop, then saves them as a trace");
    } else {
        char stop[64];
        std::snprintf(stop, sizeof(stop), "Stop and save (%zu frames)", profiler::recorded().size());
        if (ImGui::Button(stop)) {
            profiler::stopRecording();
            traceMessage = saveTrace(traceFolder, profiler::recorded());
        }
    }
    if (!traceMessage.empty()) ImGui::TextWrapped("%s", traceMessage.c_str());
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
    // Scripts: their callbacks' scopes, slowest first, and (sampling) where inside them the time goes
    ImGui::SeparatorText("Scripts");
    std::vector<profiler::Summary> scripts;
    for (const profiler::Summary& sm : profiler::summarize()) {
        size_t dot = sm.name.find('.');
        if (dot != std::string::npos && scripting::isScriptType(sm.name.substr(0, dot))) scripts.push_back(sm);
    }
    std::sort(scripts.begin(), scripts.end(), [](const profiler::Summary& a, const profiler::Summary& b) { return a.avgMs > b.avgMs; });
    if (scripts.empty()) ImGui::TextDisabled("(no Lua callbacks ran)");
    else if (ImGui::BeginTable("##scripts", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Callback");
        ImGui::TableSetupColumn("calls");
        ImGui::TableSetupColumn("avg ms");
        ImGui::TableSetupColumn("worst ms");
        ImGui::TableHeadersRow();
        for (const profiler::Summary& sm : scripts) {
            int calls = 0;   // (in the last frame: one per instance it ran on)
            for (const profiler::Scope& sc : last.scopes) calls += sm.name == sc.name;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(sm.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", calls);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", sm.avgMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", sm.maxMs);
        }
        ImGui::EndTable();
    }
    bool sampling = scripting::sampling();
    if (ImGui::Checkbox("Sample Lua functions", &sampling)) scripting::setSampling(sampling);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every millisecond a script runs, the function and line running get a sample: where the time goes "
                          "(a slow call counts as the line that made it)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) scripting::clearSamples();
    static bool byLine = false;
    ImGui::SameLine();
    ImGui::Checkbox("By line", &byLine);
    std::vector<scripting::Sample> samples = scripting::samples();
    std::vector<scripting::LineSample> lines = scripting::lineSamples();
    int all = 0;
    for (const scripting::Sample& sm : samples) all += sm.count;
    double sampleMs = scripting::sampleSeconds() * 1000.0;
    size_t rows = byLine ? lines.size() : samples.size();
    if (rows > 0 && ImGui::BeginTable("##samples", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(byLine ? "Line" : "Function");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("share");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows && i < 30; ++i) {
            int count = byLine ? lines[i].count : samples[i].count;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (byLine) ImGui::Text("%s:%d", lines[i].file.c_str(), lines[i].line);
            else ImGui::TextUnformatted(samples[i].function.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", count * sampleMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f %%", 100.0 * count / std::max(all, 1));
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
