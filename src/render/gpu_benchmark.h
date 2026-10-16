// Timing the GPU (vkgame --benchmark; the first run's quality, Game settings): the passes' times frame after frame
// (profiler::gpuTimes), summed up as each pass's median, the frame's, and written out; and the quality a measured
// frame time calls for. Plain code (no GPU), so it can be tested.
#pragma once

#include <string>
#include <utility>
#include <vector>

class GpuBenchmark {
public:
    // One frame's passes (milliseconds each) and the frame's time on the clock (start to start)
    void add(const std::vector<std::pair<std::string, float>>& passes, float cpuMs);
    size_t frames() const { return totals_.size(); }
    float medianTotal() const;   // the GPU's whole frame
    float medianCpu() const;
    // Each pass's median, in the order they first came (a pass a frame skipped counts as 0 there)
    std::vector<std::pair<std::string, float>> medians() const;
    // {"gpu": ..., "scene": ..., "frames": n, "gpu_ms": total, "frame_ms": wall clock, "passes": {name: ms, ...}}
    std::string json(const std::string& gpu, const std::string& scene, const std::string& quality) const;

private:
    std::vector<std::string> names_;
    std::vector<std::vector<float>> times_;   // per pass, per frame
    std::vector<float> totals_, cpu_;
};

// 0 low, 1 medium, 2 high: the highest whose frame fits the budget, from a frame measured at high (medium costs
// about two thirds of high's, low about half: round 8's measures)
int chooseQuality(float highFrameMs, float budgetMs);
