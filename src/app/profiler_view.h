// The frame profiler's views (core/profiler.h): a window (the editor's View > Profiler) and a compact overlay
// (vkgame, F3)
#pragma once

#include <filesystem>

// traceFolder: where Save trace / Record write Chrome traces (trace-<date>-<time>.json)
void drawProfilerWindow(bool* open, const std::filesystem::path& traceFolder);
void drawProfilerOverlay(float uiScale);
