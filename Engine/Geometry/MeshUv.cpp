#include "Engine/Geometry/MeshUv.h"
#include "Engine/Geometry/MeshTopology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

namespace worldforge {
namespace {

std::size_t cornerIndex(const MeshFace &face,VertexIndex vertex)
{
    return std::size_t(std::ranges::find(face.corners,vertex,&MeshCorner::vertex)-face.corners.begin());
}

bool matchUv(const Mesh &mesh,MeshEdge edge,std::size_t a,std::size_t b)
{
    for (auto vertex : {edge.first,edge.second}) {
        const auto &left = mesh.faces[a], &right = mesh.faces[b];
        const auto p = left.corners[cornerIndex(left,vertex)].uv, q = right.corners[cornerIndex(right,vertex)].uv;
        if (std::hypot(p[0]-q[0],p[1]-q[1]) > 0.00001f)
            return false;
    }
    return true;
}

std::vector<UvIsland> islands(const Mesh &mesh,const MeshTopology &topology,const std::set<MeshEdge> &seams,bool useUvs)
{
    std::vector<bool> visited(mesh.faces.size());
    std::vector<UvIsland> result;
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        if (visited[i])
            continue;
        UvIsland chart{i};
        visited[i] = true;
        for (std::size_t j = 0; j < chart.size(); ++j) {
            const auto face = chart[j];
            for (const auto &edge : topology.faceEdges[face]) {
                const auto &attached = topology.edgeFaces.at(edge);
                if (attached.size() != 2 || seams.contains(edge))
                    continue;
                const auto next = attached.front() == face ? attached.back() : attached.front();
                if (!visited[next] && (!useUvs || matchUv(mesh,edge,face,next))) {
                    visited[next] = true;
                    chart.push_back(next);
                }
            }
        }
        result.push_back(std::move(chart));
    }
    return result;
}

bool hasPins(const UvIsland &chart,const UvPins &pins)
{
    return !pins.empty() && std::ranges::any_of(chart,[&](std::size_t face) {
        const auto pin = pins.lower_bound({face,0});
        return pin != pins.end() && pin->first == face;
    });
}

std::string parameterize(Mesh &mesh,const MeshTopology &topology,const std::set<MeshEdge> &seams,const UvIsland &chart,
                         const UvPins &pins)
{
    std::map<std::size_t,std::size_t> offsets;
    std::size_t count = 0;
    for (const auto face : chart) {
        offsets[face] = count;
        count += mesh.faces[face].corners.size();
    }
    std::vector<std::size_t> parent(count);
    std::iota(parent.begin(),parent.end(),std::size_t(0));
    const auto root = [&](std::size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (const auto face : chart) {
        for (const auto &edge : topology.faceEdges[face]) {
            const auto &attached = topology.edgeFaces.at(edge);
            if (attached.size() != 2 || attached.front() != face || seams.contains(edge))
                continue;
            const auto next = attached.back();
            for (auto vertex : {edge.first,edge.second}) {
                const auto a = offsets.at(face)+cornerIndex(mesh.faces[face],vertex);
                const auto b = offsets.at(next)+cornerIndex(mesh.faces[next],vertex);
                parent[root(b)] = root(a);
            }
        }
    }
    std::map<std::size_t,std::size_t> indices;
    std::vector<Point3> positions;
    std::vector<std::size_t> node(count);
    for (const auto face : chart) {
        const auto &corners = mesh.faces[face].corners;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const auto key = offsets.at(face)+i;
            const auto [entry,added] = indices.try_emplace(root(key),positions.size());
            if (added)
                positions.push_back(mesh.vertices[corners[i].vertex]);
            node[key] = entry->second;
        }
    }
    struct Boundary { unsigned count = 0; std::size_t from = 0,to = 0; };
    std::map<std::pair<std::size_t,std::size_t>,Boundary> boundaries;
    std::vector<std::set<std::size_t>> neighbors(positions.size());
    for (const auto face : chart) {
        const auto offset = offsets.at(face);
        const auto &polygon = mesh.faces[face];
        for (std::size_t i = 0; i < polygon.corners.size(); ++i) {
            const auto a = node[offset+i], b = node[offset+(i+1)%polygon.corners.size()];
            auto &edge = boundaries[std::minmax(a,b)];
            ++edge.count;
            edge.from = a;
            edge.to = b;
        }
        visitFaceTriangles(mesh,polygon,[&](const auto &triangle) {
            for (int i = 0; i < 3; ++i) {
                const auto a = node[offset+triangle[i]], b = node[offset+triangle[(i+1)%3]];
                if (a != b) {
                    neighbors[a].insert(b);
                    neighbors[b].insert(a);
                }
            }
        });
    }
    std::map<std::size_t,std::size_t> next;
    for (const auto &[key,boundary] : boundaries) {
        if (boundary.count > 2 || (boundary.count == 1 && !next.emplace(boundary.from,boundary.to).second))
            return "Unwrap requires manifold charts.";
    }
    if (next.empty())
        return "Mark seams to open the closed UV island.";
    struct Rim {
        std::vector<std::size_t> vertices;
        std::vector<double> lengths;
        double perimeter = 0;
    };
    std::vector<Rim> rims;
    while (!next.empty()) {
        auto &rim = rims.emplace_back();
        const auto first = next.begin()->first;
        auto vertex = first;
        do {
            const auto entry = next.find(vertex);
            if (entry == next.end())
                return "The UV boundary is inconsistent. Check mesh winding.";
            rim.vertices.push_back(vertex);
            rim.lengths.push_back(rim.perimeter);
            const auto a = positions[vertex], b = positions[entry->second];
            rim.perimeter += std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);
            vertex = entry->second;
            next.erase(entry);
        } while (vertex != first);
        if (rim.vertices.size() < 3 || !(rim.perimeter > 0))
            return "The UV island has a degenerate boundary.";
    }
    if (positions.size()+chart.size()+rims.size() != boundaries.size()+2)
        return "Mark seams across handles to make the UV island planar.";
    const auto outer = std::ranges::max_element(rims,{},&Rim::perimeter);
    for (auto rim = rims.begin(); rim != rims.end(); ++rim) {
        if (rim == outer)
            continue;
        // Close each hole in the solver graph while keeping the original faces unchanged.
        const auto center = neighbors.size();
        neighbors.emplace_back(rim->vertices.begin(),rim->vertices.end());
        for (const auto vertex : rim->vertices)
            neighbors[vertex].insert(center);
    }
    std::vector<bool> boundary(neighbors.size());
    std::vector<std::array<double,2>> uv(neighbors.size());
    constexpr double tau = 2*std::numbers::pi;
    const double radius = outer->perimeter/tau;
    std::vector<std::optional<Point2>> existing(positions.size()), pinned(positions.size());
    if (hasPins(chart,pins)) {
        for (const auto face : chart) {
            const auto &corners = mesh.faces[face].corners;
            for (std::size_t i = 0; i < corners.size(); ++i) {
                const auto vertex = node[offsets.at(face)+i];
                if (!existing[vertex])
                    existing[vertex] = corners[i].uv;
                if (!pins.contains({face,i}))
                    continue;
                if (pinned[vertex] && *pinned[vertex] != corners[i].uv)
                    return "Two pinned corners have different UVs on a joined vertex. Mark a seam or unpin one corner.";
                pinned[vertex] = corners[i].uv;
            }
        }
    }
    for (std::size_t i = 0; i < outer->vertices.size(); ++i) {
        const double angle = tau*outer->lengths[i]/outer->perimeter;
        const auto vertex = outer->vertices[i];
        boundary[vertex] = true;
        const auto &previous = existing[vertex];
        uv[vertex] = previous ? std::array<double,2>{(*previous)[0],(*previous)[1]}
            : std::array<double,2>{radius*std::cos(angle),radius*std::sin(angle)};
    }
    for (std::size_t i = 0; i < pinned.size(); ++i)
        if (pinned[i]) {
            boundary[i] = true;
            uv[i] = {(*pinned[i])[0],(*pinned[i])[1]};
        }
    std::vector<std::size_t> interior;
    for (std::size_t i = 0; i < neighbors.size(); ++i)
        if (!boundary[i])
            interior.push_back(i);
    for (int axis = 0; axis < 2; ++axis) {
        std::vector<double> residual(neighbors.size()), direction(neighbors.size()), step(neighbors.size());
        for (auto vertex : interior)
            for (auto neighbor : neighbors[vertex])
                if (boundary[neighbor])
                    residual[vertex] += uv[neighbor][axis];
        direction = residual;
        const auto norm = [&] { return std::inner_product(residual.begin(),residual.end(),residual.begin(),0.0); };
        double squared = norm();
        const double tolerance = std::max(1e-18,squared*1e-12);
        for (int iteration = 0; iteration < 2048 && squared > tolerance; ++iteration) {
            double denominator = 0;
            for (auto vertex : interior) {
                double value = double(neighbors[vertex].size())*direction[vertex];
                for (auto neighbor : neighbors[vertex])
                    value -= direction[neighbor];
                step[vertex] = value;
                denominator += direction[vertex]*value;
            }
            if (!(denominator > 0))
                return "The UV unwrap solver could not resolve the island.";
            const double alpha = squared/denominator;
            for (auto vertex : interior) {
                uv[vertex][axis] += alpha*direction[vertex];
                residual[vertex] -= alpha*step[vertex];
            }
            const auto updated = norm();
            for (auto vertex : interior)
                direction[vertex] = residual[vertex]+(updated/squared)*direction[vertex];
            squared = updated;
        }
        if (squared > tolerance)
            return "The UV unwrap solver did not converge. Add seams to split the island.";
    }
    for (auto face : chart)
        for (std::size_t i = 0; i < mesh.faces[face].corners.size(); ++i) {
            const auto p = uv[node[offsets.at(face)+i]];
            mesh.faces[face].corners[i].uv = {float(p[0]),float(p[1])};
        }
    return {};
}

std::string pack(Mesh &mesh,const std::vector<UvIsland> &charts,float padding,const UvPins &pins)
{
    if (!(padding >= 0 && padding < 0.25f) || charts.empty())
        return "Use a padding from zero to below 0.25 UV units.";
    struct Box { Point2 low, size, origin; std::size_t chart; };
    std::vector<Box> boxes;
    struct Rectangle { float left,bottom,right,top; };
    std::vector<Rectangle> available{{padding,padding,1-padding,1-padding}};
    bool anchored = false;
    float limit = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < charts.size(); ++i) {
        Point2 low{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity()};
        Point2 high{-low[0],-low[1]};
        for (auto face : charts[i])
            for (const auto &corner : mesh.faces[face].corners)
                for (int axis = 0; axis < 2; ++axis) {
                    low[axis] = std::min(low[axis],corner.uv[axis]);
                    high[axis] = std::max(high[axis],corner.uv[axis]);
                }
        const Point2 size{high[0]-low[0],high[1]-low[1]};
        if (!(size[0] > 0 && size[1] > 0))
            return "Unwrap the degenerate UV islands before packing.";
        if (hasPins(charts[i],pins)) {
            anchored = true;
            if (low[0] < 0 || low[1] < 0 || high[0] > 1 || high[1] > 1)
                return "Move the pinned islands inside the UV tile before packing.";
            const Rectangle obstacle{low[0]-padding,low[1]-padding,high[0]+padding,high[1]+padding};
            std::vector<Rectangle> free;
            for (const auto &area : available) {
                const Rectangle overlap{std::max(area.left,obstacle.left),std::max(area.bottom,obstacle.bottom),
                    std::min(area.right,obstacle.right),std::min(area.top,obstacle.top)};
                if (overlap.left >= overlap.right || overlap.bottom >= overlap.top) {
                    free.push_back(area);
                    continue;
                }
                for (const auto &part : {Rectangle{area.left,area.bottom,overlap.left,area.top},
                         Rectangle{overlap.right,area.bottom,area.right,area.top},
                         Rectangle{overlap.left,area.bottom,overlap.right,overlap.bottom},
                         Rectangle{overlap.left,overlap.top,overlap.right,area.top}})
                    if (part.left < part.right && part.bottom < part.top)
                        free.push_back(part);
            }
            available = std::move(free);
            continue;
        }
        limit = std::min(limit,(1-2*padding)/std::max(size[0],size[1]));
        boxes.push_back({low,size,{},i});
    }
    if (boxes.empty())
        return {};
    std::ranges::stable_sort(boxes,[](const auto &a,const auto &b) { return a.size[1] > b.size[1]; });
    const auto arrange = [&](float scale) {
        if (anchored) {
            auto free = available;
            for (auto &box : boxes) {
                const float w = box.size[0]*scale, h = box.size[1]*scale;
                auto best = free.end();
                float waste = std::numeric_limits<float>::infinity();
                for (auto area = free.begin(); area != free.end(); ++area) {
                    const float width = area->right-area->left, height = area->top-area->bottom;
                    const float remaining = width*height-w*h;
                    if (w <= width && h <= height && remaining < waste) {
                        waste = remaining;
                        best = area;
                    }
                }
                if (best == free.end())
                    return false;
                const auto area = *best;
                free.erase(best);
                box.origin = {area.left,area.bottom};
                if (area.left+w+padding < area.right)
                    free.push_back({area.left+w+padding,area.bottom,area.right,std::min(area.bottom+h+padding,area.top)});
                if (area.bottom+h+padding < area.top)
                    free.push_back({area.left,area.bottom+h+padding,area.right,area.top});
            }
            return true;
        }
        float x = padding, y = padding, height = 0;
        for (auto &box : boxes) {
            const auto w = box.size[0]*scale, h = box.size[1]*scale;
            if (x+w > 1-padding) {
                x = padding;
                y += height+padding;
                height = 0;
            }
            if (x+w > 1-padding || y+h > 1-padding)
                return false;
            box.origin = {x,y};
            x += w+padding;
            height = std::max(height,h);
        }
        return true;
    };
    float lower = 0;
    for (int i = 0; i < 30; ++i) {
        const auto trial = (lower+limit)*0.5f;
        if (arrange(trial))
            lower = trial;
        else
            limit = trial;
    }
    if (!(lower > 0) || !arrange(lower))
        return "The requested UV padding does not fit all islands.";
    for (const auto &box : boxes)
        for (auto face : charts[box.chart])
            for (auto &corner : mesh.faces[face].corners)
                for (int axis = 0; axis < 2; ++axis)
                    corner.uv[axis] = box.origin[axis]+(corner.uv[axis]-box.low[axis])*lower;
    return {};
}

}

std::vector<UvIsland> uvIslands(const Mesh &mesh,const std::set<MeshEdge> &seams,bool useExistingUvs)
{
    return islands(mesh,MeshTopology(mesh),seams,useExistingUvs);
}

std::set<MeshEdge> sharpUvSeams(const Mesh &mesh)
{
    std::set<MeshEdge> seams;
    const MeshTopology topology(mesh);
    for (const auto &[edge,faces] : topology.edgeFaces) {
        if (faces.size() != 2)
            continue;
        const auto a = faceNormal(mesh,mesh.faces[faces[0]]);
        const auto b = faceNormal(mesh,mesh.faces[faces[1]]);
        if (std::inner_product(a.begin(),a.end(),b.begin(),0.0f) < 0.70710678f)
            seams.insert(edge);
    }
    return seams;
}

std::string unwrapUvIslands(Mesh &mesh,const std::set<MeshEdge> &seams,float padding,const UvPins &pins)
{
    const MeshTopology topology(mesh);
    const auto charts = islands(mesh,topology,seams,false);
    auto result = mesh;
    for (const auto &chart : charts) {
        const auto error = parameterize(result,topology,seams,chart,pins);
        if (!error.empty())
            return error;
    }
    const auto error = pack(result,charts,padding,pins);
    if (error.empty())
        mesh = std::move(result);
    return error;
}

std::string packUvIslands(Mesh &mesh,const std::set<MeshEdge> &seams,float padding,const UvPins &pins)
{
    auto result = mesh;
    const auto error = pack(result,uvIslands(mesh,seams,true),padding,pins);
    if (error.empty())
        mesh = std::move(result);
    return error;
}

std::string projectUvFaces(Mesh &mesh,std::span<const std::size_t> faces,UvProjection projection)
{
    if (faces.empty())
        return "Select faces to project.";
    auto result = mesh;
    for (const auto index : faces) {
        if (index >= mesh.faces.size())
            return "The face selection is no longer valid.";
        if (projection == UvProjection::Planar) {
            const auto error = unwrapFace(result,index);
            if (!error.empty())
                return error;
            continue;
        }
        auto &face = result.faces[index];
        const auto normal = faceNormal(mesh,face);
        int drop = 0;
        for (int axis = 1; axis < 3; ++axis)
            if (std::abs(normal[axis]) > std::abs(normal[drop]))
                drop = axis;
        Point2 low{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity()}, high{-low[0],-low[1]};
        for (auto &corner : face.corners) {
            const auto p = mesh.vertices[corner.vertex];
            corner.uv = projection == UvProjection::Box ? Point2{p[(drop+1)%3],p[(drop+2)%3]}
                : Point2{float(std::atan2(p[1],p[0])/(2*std::numbers::pi)+0.5),p[2]};
            for (int axis = 0; axis < 2; ++axis) {
                low[axis] = std::min(low[axis],corner.uv[axis]);
                high[axis] = std::max(high[axis],corner.uv[axis]);
            }
        }
        for (auto &corner : face.corners) {
            if (projection == UvProjection::Cylinder && high[0]-low[0] > 0.5f && corner.uv[0] < 0.5f)
                corner.uv[0] += 1;
        }
    }
    mesh = std::move(result);
    return {};
}

std::string stitchUvEdges(Mesh &mesh,std::span<const MeshEdge> edges,const std::set<MeshEdge> &seams)
{
    if (edges.empty())
        return "Select shared edges to stitch.";
    const MeshTopology topology(mesh);
    using Corner = std::pair<std::size_t,std::size_t>;
    std::map<Corner,std::size_t> indices;
    std::vector<std::size_t> parent;
    const auto root = [&](std::size_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    const auto node = [&](std::size_t face,VertexIndex vertex) {
        const auto key = Corner{face,cornerIndex(mesh.faces[face],vertex)};
        const auto [entry,inserted] = indices.try_emplace(key,parent.size());
        if (inserted)
            parent.push_back(entry->second);
        return entry->second;
    };
    for (const auto &edge : edges) {
        const auto attached = topology.edgeFaces.find(edge);
        if (attached == topology.edgeFaces.end() || attached->second.size() != 2)
            return "UV stitch requires edges shared by two faces.";
        for (const auto vertex : {edge.first,edge.second})
            parent[root(node(attached->second.back(),vertex))] = root(node(attached->second.front(),vertex));
    }
    // Preserve continuity with corners already connected to the stitched UVs.
    for (const auto &[edge,faces] : topology.edgeFaces)
        if (faces.size() == 2 && !seams.contains(edge) && matchUv(mesh,edge,faces[0],faces[1]))
            for (auto vertex : {edge.first,edge.second})
                parent[root(node(faces[1],vertex))] = root(node(faces[0],vertex));
    struct Average { double u = 0,v = 0; std::size_t count = 0; };
    std::vector<Average> averages(parent.size());
    for (const auto &[corner,index] : indices) {
        const auto uv = mesh.faces[corner.first].corners[corner.second].uv;
        auto &average = averages[root(index)];
        average.u += uv[0];
        average.v += uv[1];
        ++average.count;
    }
    for (const auto &[corner,index] : indices) {
        const auto &average = averages[root(index)];
        mesh.faces[corner.first].corners[corner.second].uv =
            {float(average.u/double(average.count)),float(average.v/double(average.count))};
    }
    return {};
}

std::string normalizeUvDensity(Mesh &mesh,const std::set<MeshEdge> &seams,float padding,const UvPins &pins)
{
    auto result = mesh;
    const auto charts = uvIslands(mesh,seams,true);
    for (const auto &chart : charts) {
        if (hasPins(chart,pins))
            continue;
        double uvArea = 0, meshArea = 0;
        for (auto index : chart) {
            const auto &face = mesh.faces[index];
            visitFaceTriangles(mesh,face,[&](const auto &triangle) {
                const auto &a = face.corners[triangle[0]], &b = face.corners[triangle[1]], &c = face.corners[triangle[2]];
                uvArea += std::abs((b.uv[0]-a.uv[0])*(c.uv[1]-a.uv[1])-(b.uv[1]-a.uv[1])*(c.uv[0]-a.uv[0]));
                const auto p = mesh.vertices[a.vertex], q = mesh.vertices[b.vertex], r = mesh.vertices[c.vertex];
                const Point3 x{q[0]-p[0],q[1]-p[1],q[2]-p[2]}, y{r[0]-p[0],r[1]-p[1],r[2]-p[2]};
                meshArea += std::hypot(x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]);
            });
        }
        if (!(uvArea > 0 && meshArea > 0))
            return "Unwrap the degenerate islands before normalizing density.";
        const auto scale = float(std::sqrt(meshArea/uvArea));
        for (auto index : chart)
            for (auto &corner : result.faces[index].corners) {
                corner.uv[0] *= scale;
                corner.uv[1] *= scale;
            }
    }
    const auto error = pack(result,charts,padding,pins);
    if (error.empty())
        mesh = std::move(result);
    return error;
}

UvReport inspectUvs(const Mesh &mesh,const std::set<MeshEdge> &seams)
{
    UvReport report;
    report.islands = uvIslands(mesh,seams,true).size();
    struct Triangle { std::array<Point2,3> points; Point2 low,high; };
    std::vector<Triangle> triangles;
    const auto area = [](Point2 a,Point2 b,Point2 c) {
        return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
    };
    for (const auto &face : mesh.faces) {
        for (const auto &corner : face.corners)
            report.outside += corner.uv[0] < -0.00001f || corner.uv[1] < -0.00001f
                || corner.uv[0] > 1.00001f || corner.uv[1] > 1.00001f;
        visitFaceTriangles(mesh,face,[&](const auto &indices) {
            Triangle item;
            for (int i = 0; i < 3; ++i)
                item.points[i] = face.corners[indices[i]].uv;
            if (std::abs(area(item.points[0],item.points[1],item.points[2])) < 1e-12f) {
                ++report.degenerate;
                return;
            }
            for (int i = 0; i < 2; ++i) {
                item.low[i] = std::min({item.points[0][i],item.points[1][i],item.points[2][i]});
                item.high[i] = std::max({item.points[0][i],item.points[1][i],item.points[2][i]});
            }
            triangles.push_back(item);
        });
    }
    std::ranges::sort(triangles,{},[](const auto &item) { return item.low[0]; });
    const auto separated = [](const Triangle &a,const Triangle &b) {
        for (int i = 0; i < 3; ++i) {
            const auto p = a.points[i], q = a.points[(i+1)%3];
            const auto distance = [&](Point2 v) { return (q[0]-p[0])*(v[1]-p[1])-(q[1]-p[1])*(v[0]-p[0]); };
            const auto interior = distance(a.points[(i+2)%3]);
            if (std::ranges::all_of(b.points,[&](auto v) { return distance(v)*std::copysign(1.0f,interior) <= 1e-8f; }))
                return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < triangles.size(); ++i)
        for (std::size_t j = i+1; j < triangles.size() && triangles[j].low[0] < triangles[i].high[0]; ++j) {
            const auto &a = triangles[i], &b = triangles[j];
            if (a.high[1] > b.low[1] && b.high[1] > a.low[1] && !separated(a,b) && !separated(b,a))
                ++report.overlaps;
        }
    return report;
}
}
