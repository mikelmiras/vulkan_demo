#include "game/navigation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <queue>
#include <thread>

#include "physics/physics.h"

namespace {

// Neighbour directions: the four sides first, then the diagonals (each between kDiagSides' two sides)
constexpr int kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
constexpr int kDiagSides[4][2] = {{0, 2}, {0, 3}, {1, 2}, {1, 3}};
constexpr int kMaxCells = 4'000'000;   // ~150 MB at most; the cell size grows to stay under it

int dirOf(int dx, int dz) {
    for (int d = 0; d < 8; ++d)
        if (kDirs[d][0] == dx && kDirs[d][1] == dz) return d;
    return -1;
}

}  // namespace

void NavGrid::clear() {
    nx_ = nz_ = 0;
    columnStart_.clear();
    cells_.clear();
    cellColumn_.clear();
    search_.clear();
}

bool NavGrid::bake(const Scene& scene, const MeshLibrary& meshes, const NavigationSettings& settings) {
    auto started = std::chrono::steady_clock::now();
    clear();
    settings_ = settings;

    // The level: solid colliders of things that don't move by themselves (no rigid body, no behaviours)
    Scene statics = scene;
    for (size_t i = 0; i < statics.entities.size(); ++i) {
        Entity& e = statics.entities[i];
        bool level = e.collider && !e.collider->trigger && !e.rigidBody && e.behaviours.empty();
        e.behaviours.clear();
        e.rigidBody.reset();
        if (!level) e.collider.reset();
    }
    Physics physics;
    physics.step(statics, meshes, 0.f);
    Vec3 lo, hi;
    if (!physics.bounds(lo, hi)) return false;

    cell_ = std::max(settings.cellSize, 0.05f);
    float w = hi.x - lo.x, d = hi.z - lo.z;
    while ((w / cell_ + 1.f) * (d / cell_ + 1.f) > (float)kMaxCells) cell_ *= 1.25f;
    nx_ = std::max(1, (int)std::ceil(w / cell_));
    nz_ = std::max(1, (int)std::ceil(d / cell_));
    origin_ = lo;

    // Surfaces in each column, top to bottom: flat enough, with headroom
    const float minUp = std::cos(std::clamp(settings.maxSlope, 0.f, 89.f) * DEG2RAD);
    const float top = hi.y + 0.5f, depth = hi.y - lo.y + 1.f;
    const float headroom = std::max(settings.agentHeight, 0.1f);
    // (rows of columns on several threads: queries only read the physics world)
    std::vector<std::vector<float>> rows((size_t)nz_);
    std::vector<std::vector<int32_t>> rowCounts((size_t)nz_);
    std::atomic<int> nextRow{0};
    auto probeRows = [&]() {
        std::vector<float> ys;
        for (int z = nextRow++; z < nz_; z = nextRow++) {
            std::vector<float>& out = rows[(size_t)z];
            std::vector<int32_t>& counts = rowCounts[(size_t)z];
            counts.assign((size_t)nx_, 0);
            for (int x = 0; x < nx_; ++x) {
                Vec3 p{origin_.x + (x + 0.5f) * cell_, top, origin_.z + (z + 0.5f) * cell_};
                ys.clear();
                for (const RaycastHit& h : physics.raycastAll(p, {0, -1, 0}, depth)) {
                    if (h.normal.y < minUp) continue;
                    RaycastHit above;
                    if (physics.raycast(h.point + Vec3{0, 0.02f, 0}, {0, 1, 0}, headroom - 0.02f, above)) continue;
                    if (ys.empty() || std::fabs(ys.back() - h.point.y) > 0.05f) ys.push_back(h.point.y);
                }
                std::sort(ys.begin(), ys.end());
                out.insert(out.end(), ys.begin(), ys.end());
                counts[(size_t)x] = (int32_t)ys.size();
            }
        }
    };
    unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (nz_ < 16) threads = 1;
    std::vector<std::thread> workers;
    for (unsigned t = 1; t < threads; ++t) workers.emplace_back(probeRows);
    probeRows();
    for (std::thread& t : workers) t.join();

    columnStart_.assign((size_t)nx_ * nz_ + 1, 0);
    for (int z = 0; z < nz_; ++z) {
        size_t k = 0;
        for (int x = 0; x < nx_; ++x) {
            int c = z * nx_ + x;
            columnStart_[(size_t)c] = (int32_t)cells_.size();
            for (int32_t n = 0; n < rowCounts[(size_t)z][(size_t)x]; ++n) {
                Cell cl;
                cl.y = rows[(size_t)z][k++];
                std::fill(std::begin(cl.next), std::end(cl.next), -1);
                cells_.push_back(cl);
                cellColumn_.push_back(c);
            }
        }
        std::vector<float>().swap(rows[(size_t)z]);
    }
    columnStart_.back() = (int32_t)cells_.size();

    // Links: to the neighbour cell at the closest height, if the step is low enough. Diagonals only where both
    // sides are open too, so paths don't cut corners.
    auto link = [&](int i, int d) {
        int c = cellColumn_[(size_t)i], x = c % nx_ + kDirs[d][0], z = c / nx_ + kDirs[d][1];
        if (x < 0 || z < 0 || x >= nx_ || z >= nz_) return -1;
        int nc = z * nx_ + x, best = -1;
        float bestDy = settings.maxClimb;
        for (int j = columnStart_[(size_t)nc]; j < columnStart_[(size_t)nc + 1]; ++j) {
            float dy = std::fabs(cells_[(size_t)j].y - cells_[(size_t)i].y);
            if (dy <= bestDy) { bestDy = dy; best = j; }
        }
        return best;
    };
    for (int i = 0; i < (int)cells_.size(); ++i)
        for (int dir = 0; dir < 4; ++dir) cells_[(size_t)i].next[dir] = link(i, dir);
    auto linkDiagonals = [&]() {
        for (int i = 0; i < (int)cells_.size(); ++i)
            for (int k = 0; k < 4; ++k) {
                Cell& cl = cells_[(size_t)i];
                int a = cl.next[kDiagSides[k][0]], b = cl.next[kDiagSides[k][1]];
                cl.next[4 + k] = a >= 0 && b >= 0 ? link(i, 4 + k) : -1;
            }
    };
    linkDiagonals();

    // Keep the agent's radius from edges: the distance from each cell to the nearest missing side (half a cell
    // from a cell that has one), along the links
    std::vector<float> dist(cells_.size(), 1e30f);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    for (int i = 0; i < (int)cells_.size(); ++i)
        for (int dir = 0; dir < 4; ++dir)
            if (cells_[(size_t)i].next[dir] < 0) {
                dist[(size_t)i] = cell_ * 0.5f;
                open.push({dist[(size_t)i], i});
                break;
            }
    while (!open.empty()) {
        auto [dd, i] = open.top();
        open.pop();
        if (dd > dist[(size_t)i]) continue;
        if (dd >= settings.agentRadius) continue;   // far enough: nothing beyond needs a smaller value
        for (int dir = 0; dir < 8; ++dir) {
            int j = cells_[(size_t)i].next[dir];
            if (j < 0) continue;
            float nd = dd + (dir < 4 ? cell_ : cell_ * 1.41421356f);
            if (nd < dist[(size_t)j]) {
                dist[(size_t)j] = nd;
                open.push({nd, j});
            }
        }
    }
    std::vector<int32_t> remap(cells_.size(), -1);
    std::vector<Cell> kept;
    std::vector<int32_t> keptColumn;
    std::vector<int32_t> starts((size_t)nx_ * nz_ + 1, 0);
    for (int c = 0; c < nx_ * nz_; ++c) {
        starts[(size_t)c] = (int32_t)kept.size();
        for (int i = columnStart_[(size_t)c]; i < columnStart_[(size_t)c + 1]; ++i)
            if (dist[(size_t)i] >= settings.agentRadius) {
                remap[(size_t)i] = (int32_t)kept.size();
                kept.push_back(cells_[(size_t)i]);
                keptColumn.push_back(c);
            }
    }
    starts.back() = (int32_t)kept.size();
    for (Cell& cl : kept)
        for (int32_t& n : cl.next) n = n >= 0 ? remap[(size_t)n] : -1;
    cells_ = std::move(kept);
    cellColumn_ = std::move(keptColumn);
    columnStart_ = std::move(starts);
    linkDiagonals();   // a side may have gone

    bakeMs_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    return !cells_.empty();
}

int NavGrid::columnOf(float x, float z) const {
    int cx = (int)std::floor((x - origin_.x) / cell_), cz = (int)std::floor((z - origin_.z) / cell_);
    if (cx < 0 || cz < 0 || cx >= nx_ || cz >= nz_) return -1;
    return cz * nx_ + cx;
}

int NavGrid::cellColumn(int cell) const { return cellColumn_[(size_t)cell]; }

Vec3 NavGrid::centre(int cell) const {
    int c = cellColumn_[(size_t)cell];
    return {origin_.x + (c % nx_ + 0.5f) * cell_, cells_[(size_t)cell].y, origin_.z + (c / nx_ + 0.5f) * cell_};
}

// The cell under p: the highest one at most a step above it and at most an agent's height (plus a step) below,
// in p's column, else the nearest such cell within searchRadius
int NavGrid::cellAt(Vec3 p, float searchRadius) const {
    if (cells_.empty()) return -1;
    float above = settings_.maxClimb, below = settings_.agentHeight + settings_.maxClimb;
    int cx = (int)std::floor((p.x - origin_.x) / cell_), cz = (int)std::floor((p.z - origin_.z) / cell_);
    int reach = (int)std::ceil(searchRadius / cell_);
    int best = -1;
    float bestScore = 1e30f;
    for (int z = cz - reach; z <= cz + reach; ++z)
        for (int x = cx - reach; x <= cx + reach; ++x) {
            if (x < 0 || z < 0 || x >= nx_ || z >= nz_) continue;
            int c = z * nx_ + x;
            for (int i = columnStart_[(size_t)c]; i < columnStart_[(size_t)c + 1]; ++i) {
                float y = cells_[(size_t)i].y;
                if (y > p.y + above || y < p.y - below) continue;
                Vec3 m = centre(i);
                float hx = m.x - p.x, hz = m.z - p.z, h2 = hx * hx + hz * hz;
                if (c != cz * nx_ + cx && h2 > searchRadius * searchRadius) continue;
                float score = (x == cx && z == cz ? 0.f : h2) - y * 1e-4f;   // (ties: the higher floor)
                if (score < bestScore) { bestScore = score; best = i; }
            }
        }
    return best;
}

bool NavGrid::nearest(Vec3 p, Vec3& onGround, float maxDistance) const {
    int i = cellAt(p, maxDistance);
    if (i < 0) return false;
    onGround = cellColumn(i) == columnOf(p.x, p.z) ? Vec3{p.x, cells_[(size_t)i].y, p.z} : centre(i);
    return true;
}

bool NavGrid::walkable(Vec3 a, Vec3 b) const {
    int cur = cellAt(a, 0.f);
    if (cur < 0) return false;
    float dx = b.x - a.x, dz = b.z - a.z, len = std::sqrt(dx * dx + dz * dz);
    int steps = std::max(1, (int)std::ceil(len / (cell_ * 0.25f)));
    int col = cellColumn(cur);
    for (int s = 1; s <= steps; ++s) {
        float t = (float)s / (float)steps;
        int c = columnOf(a.x + dx * t, a.z + dz * t);
        if (c < 0) return false;
        if (c == col) continue;
        int dir = dirOf(c % nx_ - col % nx_, c / nx_ - col / nx_);
        if (dir < 0) return false;
        int next = cells_[(size_t)cur].next[dir];
        if (next < 0) return false;
        cur = next;
        col = c;
    }
    return std::fabs(cells_[(size_t)cur].y - b.y) <= settings_.maxClimb + 0.05f;
}

bool NavGrid::findPath(Vec3 from, Vec3 to, std::vector<Vec3>& path) const {
    path.clear();
    int start = cellAt(from, std::max(settings_.agentRadius * 2.f, cell_ * 2.f));
    int goal = cellAt(to, 3.f);
    if (start < 0 || goal < 0) return false;

    // A* over the cells
    // (scratch kept between searches: clearing per-cell arrays would cost more than most searches)
    if (search_.size() != cells_.size()) {
        search_.assign(cells_.size(), {});
        generation_ = 0;
    }
    if (++generation_ == 0) {   // wrapped: forget every stamp
        for (Search& s : search_) s.generation = 0;
        generation_ = 1;
    }
    auto node = [&](int i) -> Search& {
        Search& s = search_[(size_t)i];
        if (s.generation != generation_) s = {1e30f, -1, generation_, false};
        return s;
    };
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    Vec3 goalPos = centre(goal);
    node(start).g = 0.f;
    open.push({length(centre(start) - goalPos), start});
    bool found = false;
    while (!open.empty()) {
        int i = open.top().second;
        open.pop();
        Search& si = node(i);
        if (si.closed) continue;
        si.closed = true;
        if (i == goal) { found = true; break; }
        Vec3 pi = centre(i);
        for (int j : cells_[(size_t)i].next) {
            if (j < 0) continue;
            Search& sj = node(j);
            if (sj.closed) continue;
            Vec3 pj = centre(j);
            float ng = si.g + length(pj - pi);
            if (ng < sj.g) {
                sj.g = ng;
                sj.parent = i;
                open.push({ng + length(goalPos - pj) * 1.5f, j});   // (greedy: fewer cells searched; straightening evens it out)
            }
        }
    }
    if (!found) return false;

    std::vector<Vec3> points;
    for (int i = goal; i >= 0; i = search_[(size_t)i].parent) points.push_back(centre(i));
    std::reverse(points.begin(), points.end());
    // The ends where the points themselves are, when they're on the grid there
    if (cellColumn(start) == columnOf(from.x, from.z)) points.front() = {from.x, points.front().y, from.z};
    if (cellColumn(goal) == columnOf(to.x, to.z)) points.back() = {to.x, points.back().y, to.z};

    // Straighten: from each corner, go as far along the cells as a straight walk reaches
    path.push_back(points[0]);
    size_t i = 0;
    while (i + 1 < points.size()) {
        size_t k = i + 1;
        while (k + 1 < points.size() && walkable(points[i], points[k + 1])) ++k;
        path.push_back(points[k]);
        i = k;
    }
    return true;
}

std::vector<Vec3> NavGrid::cellCenters() const {
    std::vector<Vec3> out;
    out.reserve(cells_.size());
    for (int i = 0; i < (int)cells_.size(); ++i) out.push_back(centre(i));
    return out;
}

std::vector<Vec3> NavGrid::links() const {
    std::vector<Vec3> out;
    for (int i = 0; i < (int)cells_.size(); ++i)
        for (int d : {0, 2}) {   // +x and +z: each side once
            int j = cells_[(size_t)i].next[d];
            if (j < 0) continue;
            out.push_back(centre(i));
            out.push_back(centre(j));
        }
    return out;
}
