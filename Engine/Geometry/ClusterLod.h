#pragma once

// Cluster level of detail (R4.4; design in the playbook, section 10.3): a hierarchy of triangle clusters, each level a
// simplified copy of the one below, built so a renderer can pick, per cluster, the coarsest level whose error is under a
// pixel without cracks between neighbors.
//
// Level 0 cuts the mesh into clusters of at most clusterTriangles triangles in Morton order. Each level then groups
// about groupClusters neighboring clusters, merges and simplifies each group to about half its triangles with
// quadric-error edge collapse while locking the group's outer boundary (so it still meets its neighbors exactly), and
// cuts the result into the next level's clusters. A cluster's error is the largest simplification error behind it, in
// meters; its parent group's error is never smaller. A renderer draws a cluster when its own projected error is at most
// a pixel and its parent's is more.

#include "Engine/Geometry/Mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <span>
#include <unordered_map>
#include <vector>

namespace worldforge::lod {

using Triangle = std::array<uint32_t,3>;

struct Sphere {
    Point3 center{};
    float radius = 0;
};

struct Cluster {
    std::vector<Triangle> triangles; // into ClusterHierarchy::positions
    Sphere bounds;
    float error = 0;                 // meters; 0 at level 0
    Sphere parentBounds;             // the group this cluster was simplified into (its own bounds at the top)
    float parentError = std::numeric_limits<float>::infinity(); // infinite at the top: always fine enough
    uint32_t level = 0;
};

struct ClusterHierarchy {
    std::vector<Point3> positions;
    std::vector<Cluster> clusters; // level by level
    uint32_t levels = 0;
};

constexpr std::size_t clusterTriangles = 124, groupClusters = 4;

namespace detail {

inline Point3 subtract(Point3 a,Point3 b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
inline Point3 cross(Point3 a,Point3 b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline float dot(Point3 a,Point3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline float length(Point3 a) { return std::sqrt(dot(a,a)); }

// A symmetric 4 x 4 quadric (Garland and Heckbert), its ten distinct entries.
struct Quadric {
    std::array<double,10> q{};
    void addPlane(Point3 normal,float d,double weight)
    {
        const double a = normal[0], b = normal[1], c = normal[2], e = d;
        const std::array<double,10> plane{a*a,a*b,a*c,a*e,b*b,b*c,b*e,c*c,c*e,e*e};
        for (std::size_t i = 0; i < q.size(); ++i)
            q[i] += plane[i]*weight;
    }
    void add(const Quadric &other)
    {
        for (std::size_t i = 0; i < q.size(); ++i)
            q[i] += other.q[i];
    }
    // The summed squared distance of p to the planes (weighted by area).
    double error(Point3 p) const
    {
        const double x = p[0], y = p[1], z = p[2];
        return q[0]*x*x+2*q[1]*x*y+2*q[2]*x*z+2*q[3]*x+q[4]*y*y+2*q[5]*y*z+2*q[6]*y+q[7]*z*z+2*q[8]*z+q[9];
    }
};

inline uint32_t mortonPart(float value)
{
    auto v = uint32_t(std::clamp(value,0.0f,1.0f)*1023.0f);
    v = (v*0x00010001u) & 0xFF0000FFu;
    v = (v*0x00000101u) & 0x0F00F00Fu;
    v = (v*0x00000011u) & 0xC30C30C3u;
    v = (v*0x00000005u) & 0x49249249u;
    return v;
}

// Indices of points sorted along a Morton curve through their bounding box.
inline std::vector<std::size_t> mortonOrder(std::span<const Point3> points)
{
    Point3 low{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    Point3 high{std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest()};
    for (const auto &p : points)
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis],p[axis]);
            high[axis] = std::max(high[axis],p[axis]);
        }
    std::vector<std::pair<uint32_t,std::size_t>> keys;
    keys.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        uint32_t code = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const float span = high[axis]-low[axis];
            code |= mortonPart(span > 0 ? (points[i][axis]-low[axis])/span : 0.0f) << axis;
        }
        keys.push_back({code,i});
    }
    std::ranges::sort(keys);
    std::vector<std::size_t> order;
    order.reserve(keys.size());
    for (const auto &key : keys)
        order.push_back(key.second);
    return order;
}

inline Point3 centroid(std::span<const Point3> positions,const Triangle &t)
{
    const auto &a = positions[t[0]], &b = positions[t[1]], &c = positions[t[2]];
    return {(a[0]+b[0]+c[0])/3,(a[1]+b[1]+c[1])/3,(a[2]+b[2]+c[2])/3};
}

inline Sphere bounds(std::span<const Point3> positions,std::span<const Triangle> triangles)
{
    Point3 low{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    Point3 high{std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest()};
    for (const auto &t : triangles)
        for (const auto index : t)
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis],positions[index][axis]);
                high[axis] = std::max(high[axis],positions[index][axis]);
            }
    Sphere sphere;
    sphere.center = {(low[0]+high[0])/2,(low[1]+high[1])/2,(low[2]+high[2])/2};
    for (const auto &t : triangles)
        for (const auto index : t)
            sphere.radius = std::max(sphere.radius,length(subtract(positions[index],sphere.center)));
    return sphere;
}

// Cuts triangles into clusters of at most clusterTriangles, in Morton order of their centroids.
inline std::vector<std::vector<Triangle>> cut(std::span<const Point3> positions,std::span<const Triangle> triangles)
{
    std::vector<Point3> centers;
    centers.reserve(triangles.size());
    for (const auto &t : triangles)
        centers.push_back(centroid(positions,t));
    std::vector<std::vector<Triangle>> clusters;
    for (const auto index : mortonOrder(centers)) {
        if (clusters.empty() || clusters.back().size() == clusterTriangles)
            clusters.emplace_back();
        clusters.back().push_back(triangles[index]);
    }
    return clusters;
}

} // namespace detail

// Simplifies triangles (indices into positions) toward target triangles by quadric-error edge collapse, never moving a
// vertex on an edge that only one triangle uses (the group's boundary) and never flipping a triangle. Returns the
// largest collapse error, in meters (the square root of the quadric error per unit area).
inline float simplify(std::span<const Point3> positions,std::vector<Triangle> &triangles,std::size_t target)
{
    using namespace detail;
    std::unordered_map<uint32_t,Quadric> quadrics;
    std::unordered_map<uint32_t,double> areas;
    std::map<std::pair<uint32_t,uint32_t>,int> edgeUses;
    const auto edge = [](uint32_t a,uint32_t b) { return a < b ? std::pair{a,b} : std::pair{b,a}; };
    for (const auto &t : triangles) {
        const auto n = cross(subtract(positions[t[1]],positions[t[0]]),subtract(positions[t[2]],positions[t[0]]));
        const float twiceArea = length(n);
        if (twiceArea <= 0)
            continue;
        const Point3 unit{n[0]/twiceArea,n[1]/twiceArea,n[2]/twiceArea};
        for (const auto v : t) {
            quadrics[v].addPlane(unit,-dot(unit,positions[v]),twiceArea*0.5);
            areas[v] += twiceArea*0.5;
        }
        for (int i = 0; i < 3; ++i)
            ++edgeUses[edge(t[std::size_t(i)],t[std::size_t((i+1)%3)])];
    }
    std::unordered_map<uint32_t,bool> locked;
    for (const auto &[key,uses] : edgeUses)
        if (uses == 1)
            locked[key.first] = locked[key.second] = true;
    // remap[v]: what v has collapsed into (itself when it has not).
    std::unordered_map<uint32_t,uint32_t> remap;
    const auto find = [&](uint32_t v) {
        while (true) {
            const auto it = remap.find(v);
            if (it == remap.end() || it->second == v)
                return v;
            v = it->second;
        }
    };
    struct Candidate { double cost; uint32_t from, to; };
    const auto worse = [](const Candidate &a,const Candidate &b) { return a.cost > b.cost; };
    std::priority_queue<Candidate,std::vector<Candidate>,decltype(worse)> queue(worse);
    const auto consider = [&](uint32_t a,uint32_t b) {
        const bool lockA = locked.contains(a), lockB = locked.contains(b);
        if (lockA && lockB)
            return;
        Quadric q = quadrics[a];
        q.add(quadrics[b]);
        // Collapse the movable end into the other (into the locked one when there is one).
        if (lockA)
            queue.push({q.error(positions[a]),b,a});
        else if (lockB)
            queue.push({q.error(positions[b]),a,b});
        else {
            const double toA = q.error(positions[a]), toB = q.error(positions[b]);
            queue.push(toA <= toB ? Candidate{toA,b,a} : Candidate{toB,a,b});
        }
    };
    for (const auto &[key,uses] : edgeUses)
        consider(key.first,key.second);
    std::size_t alive = triangles.size();
    double largest = 0;
    std::unordered_map<uint32_t,std::vector<std::size_t>> incident;
    for (std::size_t i = 0; i < triangles.size(); ++i)
        for (const auto v : triangles[i])
            incident[v].push_back(i);
    std::vector<bool> removed(triangles.size(),false);
    while (alive > target && !queue.empty()) {
        const auto candidate = queue.top();
        queue.pop();
        const auto from = find(candidate.from), to = find(candidate.to);
        if (from != candidate.from || to != candidate.to || from == to)
            continue; // stale: an end has already moved
        // Reject collapses that would flip or degenerate a surviving triangle around from.
        bool valid = true;
        std::size_t dying = 0;
        for (const auto index : incident[from]) {
            if (removed[index])
                continue;
            auto t = triangles[index];
            for (auto &v : t)
                v = find(v);
            if (std::ranges::count(t,to) > 0) {
                ++dying;
                continue;
            }
            const auto before = cross(subtract(positions[t[1]],positions[t[0]]),subtract(positions[t[2]],positions[t[0]]));
            for (auto &v : t)
                if (v == from)
                    v = to;
            const auto after = cross(subtract(positions[t[1]],positions[t[0]]),subtract(positions[t[2]],positions[t[0]]));
            if (dot(before,after) <= 0.2f*length(before)*length(after)) {
                valid = false;
                break;
            }
        }
        if (!valid || dying == 0)
            continue;
        remap[from] = to;
        quadrics[to].add(quadrics[from]);
        areas[to] += areas[from];
        largest = std::max(largest,candidate.cost/std::max(areas[to],1e-12));
        for (const auto index : incident[from]) {
            if (removed[index])
                continue;
            auto t = triangles[index];
            for (auto &v : t)
                v = find(v);
            if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) {
                removed[index] = true;
                --alive;
            } else {
                incident[to].push_back(index);
            }
        }
        // New edges around to, at their new costs.
        for (const auto index : incident[to]) {
            if (removed[index])
                continue;
            for (const auto v : triangles[index]) {
                const auto other = find(v);
                if (other != to)
                    consider(std::min(other,to),std::max(other,to));
            }
        }
    }
    std::vector<Triangle> result;
    result.reserve(alive);
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        if (removed[i])
            continue;
        auto t = triangles[i];
        for (auto &v : t)
            v = find(v);
        if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
            result.push_back(t);
    }
    triangles = std::move(result);
    return float(std::sqrt(largest));
}

// Welds a triangle list (three positions per triangle) by exact position and builds its cluster hierarchy.
inline ClusterHierarchy buildHierarchy(std::span<const Point3> corners)
{
    using namespace detail;
    ClusterHierarchy hierarchy;
    std::map<std::array<float,3>,uint32_t> welded;
    std::vector<Triangle> triangles;
    for (std::size_t i = 0; i+2 < corners.size(); i += 3) {
        Triangle t{};
        for (std::size_t c = 0; c < 3; ++c) {
            const auto [it,added] = welded.try_emplace(corners[i+c],uint32_t(hierarchy.positions.size()));
            if (added)
                hierarchy.positions.push_back(corners[i+c]);
            t[c] = it->second;
        }
        if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
            triangles.push_back(t);
    }
    const std::span<const Point3> positions = hierarchy.positions;
    std::vector<std::size_t> level; // indices of the current level's clusters
    for (auto &triangleSet : cut(positions,triangles)) {
        Cluster cluster;
        cluster.bounds = bounds(positions,triangleSet);
        cluster.triangles = std::move(triangleSet);
        level.push_back(hierarchy.clusters.size());
        hierarchy.clusters.push_back(std::move(cluster));
    }
    hierarchy.levels = 1;
    while (level.size() > 1) {
        std::vector<Point3> centers;
        for (const auto index : level)
            centers.push_back(hierarchy.clusters[index].bounds.center);
        const auto order = mortonOrder(centers);
        std::vector<std::size_t> next;
        std::size_t before = 0, after = 0;
        for (std::size_t first = 0; first < order.size(); first += groupClusters) {
            std::vector<Triangle> group;
            float childError = 0;
            std::vector<std::size_t> members;
            for (std::size_t i = first; i < std::min(order.size(),first+groupClusters); ++i) {
                const auto index = level[order[i]];
                members.push_back(index);
                const auto &cluster = hierarchy.clusters[index];
                group.insert(group.end(),cluster.triangles.begin(),cluster.triangles.end());
                childError = std::max(childError,cluster.error);
            }
            before += group.size();
            const float error = childError+simplify(positions,group,group.size()/2);
            after += group.size();
            const auto groupBounds = bounds(positions,group);
            for (const auto index : members) {
                hierarchy.clusters[index].parentError = error;
                hierarchy.clusters[index].parentBounds = groupBounds;
            }
            for (auto &triangleSet : cut(positions,group)) {
                Cluster cluster;
                cluster.bounds = bounds(positions,triangleSet);
                cluster.triangles = std::move(triangleSet);
                cluster.error = error;
                cluster.level = hierarchy.levels;
                next.push_back(hierarchy.clusters.size());
                hierarchy.clusters.push_back(std::move(cluster));
            }
        }
        ++hierarchy.levels;
        // Stop when simplifying no longer pays: the next level keeps over 85% of the triangles.
        if (after*100 > before*85) {
            for (const auto index : next) {
                hierarchy.clusters[index].parentError = std::numeric_limits<float>::infinity();
                hierarchy.clusters[index].parentBounds = hierarchy.clusters[index].bounds;
            }
            break;
        }
        level = std::move(next);
    }
    for (const auto index : level)
        if (std::isinf(hierarchy.clusters[index].parentError))
            hierarchy.clusters[index].parentBounds = hierarchy.clusters[index].bounds;
    return hierarchy;
}

} // namespace worldforge::lod
