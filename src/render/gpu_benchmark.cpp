// The GPU's times summed up (gpu_benchmark.h)
#include "render/gpu_benchmark.h"

#include <algorithm>
#include <cstdio>

namespace {
float median(std::vector<float> v) {
    if (v.empty()) return 0.f;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}
std::string escaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}
}  // namespace

void GpuBenchmark::add(const std::vector<std::pair<std::string, float>>& passes, float cpuMs) {
    const size_t frame = totals_.size();
    float total = 0.f;
    for (const auto& [name, ms] : passes) {
        auto it = std::find(names_.begin(), names_.end(), name);
        size_t k = (size_t)(it - names_.begin());
        if (it == names_.end()) {
            names_.push_back(name);
            times_.emplace_back(frame, 0.f);   // (not there before: 0 in the frames gone)
        }
        times_[k].resize(frame, 0.f);
        times_[k].push_back(ms);
        total += ms;
    }
    for (auto& t : times_) t.resize(frame + 1, 0.f);   // (a pass missing this frame: 0)
    totals_.push_back(total);
    cpu_.push_back(cpuMs);
}

float GpuBenchmark::medianTotal() const { return median(totals_); }
float GpuBenchmark::medianCpu() const { return median(cpu_); }

std::vector<std::pair<std::string, float>> GpuBenchmark::medians() const {
    std::vector<std::pair<std::string, float>> out;
    for (size_t k = 0; k < names_.size(); ++k) out.push_back({names_[k], median(times_[k])});
    return out;
}

std::string GpuBenchmark::json(const std::string& gpu, const std::string& scene, const std::string& quality) const {
    char buf[256];
    std::string s = "{\"gpu\": \"" + escaped(gpu) + "\", \"scene\": \"" + escaped(scene) + "\", \"quality\": \"" +
                    escaped(quality) + "\", ";
    std::snprintf(buf, sizeof(buf), "\"frames\": %zu, \"gpu_ms\": %.3f, \"frame_ms\": %.3f, \"passes\": {", frames(),
                  medianTotal(), medianCpu());
    s += buf;
    bool first = true;
    for (const auto& [name, ms] : medians()) {
        std::snprintf(buf, sizeof(buf), "%s\"%s\": %.3f", first ? "" : ", ", escaped(name).c_str(), ms);
        s += buf;
        first = false;
    }
    return s + "}}\n";
}

int chooseQuality(float highFrameMs, float budgetMs) {
    if (highFrameMs <= 0.f) return 2;   // (nothing measured: as asked)
    if (highFrameMs <= budgetMs) return 2;
    if (highFrameMs * 0.66f <= budgetMs) return 1;
    return 0;
}
