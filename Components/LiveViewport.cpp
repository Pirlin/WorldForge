#include "Components/LiveViewport.h"
#include "State/ProjectStore.h"
#include "State/WorldRuntime.h"
#include "Engine/Geometry/MeshClip.h"
#include <QDebug>
#include <QFileInfo>
#include "State/WorldScene.h"
#include "Engine/Render/SkyLight.h"
#include <optional>

#include <QLabel>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <bit>
#include <QJsonArray>
#include <QVBoxLayout>

#ifdef WORLDFORGE_VULKAN
#include <QFile>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QQuaternion>
#include <QVulkanFunctions>
#include <QVulkanWindow>
#include <QWheelEvent>
#include <QTimer>
#include "Components/WorldLightingVulkan.h"
#include "Components/ViewportFrameVulkan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif
#endif

#ifdef WORLDFORGE_VULKAN
namespace {
namespace wf = worldforge;

QVector3D point(wf::Point3 p) { return {p[0], p[1], p[2]}; }
// Authored colors are sRGB-encoded; the scene pass shades in linear light. Geometry rebuilds convert
// every corner (each frame during Play), so a 12-bit table replaces pow; its error is under 0.0003.
QVector3D linearColor(QVector3D encoded)
{
    static const auto table = [] {
        std::array<float,4096> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = wf::color::srgbToLinear(float(i)/4095.0f);
        return values;
    }();
    const auto decode = [](float value) { return table[std::size_t(std::clamp(value,0.0f,1.0f)*4095.0f+0.5f)]; };
    return {decode(encoded.x()),decode(encoded.y()),decode(encoded.z())};
}

struct Vertex {
    float position[3];
    float color[3];
    float wind;
    float sun;
    float uv[2]{};
    uint32_t texture[3]{};
    float materialNormal[3]{};
    float surfaceFactors[2]{};
};

struct ViewUniform {
    float matrix[16];
    float eye[4];
    float fog[4];
    float horizon[4];
    float zenith[4];
};
static_assert(sizeof(ViewUniform) == 128);
constexpr auto shaderStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct PickTriangle {
    QVector3D a, b, c;
    QString id;
    int face;
};

struct Light {
    QVector3D vector;
    float intensity;
    bool directional;
    // Spot lights (State/WorldScene.h): the direction they point, the full cone in radians (0 for a point light), blend.
    QVector3D spotDirection{0,0,-1};
    float spotAngle = 0, spotBlend = 0.15f;
};

using ImageRange = std::array<uint32_t,3>;
constexpr std::size_t materialWords = 18;

ImageRange appendMaterialImage(std::vector<uint32_t> &pixels,const QImage &source)
{
    const auto image = source.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        return {};
    const ImageRange range{uint32_t(pixels.size()),uint32_t(image.width()),uint32_t(image.height())};
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const auto *p = row+4*x;
            pixels.push_back(uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24));
        }
    }
    return range;
}

ImageRange loadMaterialImage(std::vector<uint32_t> &pixels,const std::vector<uint8_t> &bytes,int side)
{
    if (bytes.empty() || side <= 0)
        return {};
    QBuffer input;
    input.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()),bytes.size()));
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input);
    const auto format = reader.format();
    const auto size = reader.size();
    if ((format != "png" && format != "jpeg") || !size.isValid() || size.width() > 8192
        || size.height() > 8192 || qint64(size.width())*size.height() > 32*1024*1024)
        return {};
    if (size.width() > side || size.height() > side)
        reader.setScaledSize(size.scaled(side,side,Qt::KeepAspectRatio).expandedTo({1,1}));
    return appendMaterialImage(pixels,reader.read());
}

// The primary sun first, then the other visible lights, as the renderers light the scene.
float skyAmbient(const WorldAtmosphere &environment)
{
    const auto rgb = [](const QColor &color) { return wf::color::Rgb{float(color.redF()),float(color.greenF()),float(color.blueF())}; };
    return wf::color::skyAmbientScale(rgb(environment.horizon),rgb(environment.zenith));
}

Light primarySun(const QList<SceneObject> &objects,const WorldAtmosphere &environment)
{
    Light result{sunDirection(environment),1,true};
    for (const auto &object : objects)
        if (object.visible && object.type == "Directional Light") {
            if (!(object.id == "sun" && object.rotation.isNull()))
                result.vector = QQuaternion::fromEulerAngles(object.rotation).rotatedVector({0,0,1});
            result.intensity = lightUnits(object);
            break;
        }
    return result;
}

// The primary sun first, then the other visible lights, as the renderers light the scene.
std::vector<Light> sceneLights(const QList<SceneObject> &objects,const WorldAtmosphere &environment)
{
    std::vector<Light> lights{primarySun(objects,environment)};
    bool usedPrimary = false;
    for (const auto &object : objects) {
        const bool directional = object.type == "Directional Light";
        if (!object.visible || (!directional && object.type != "Point Light" && object.type != "Spot Light"))
            continue;
        if (directional && !usedPrimary) {
            usedPrimary = true;
            continue;
        }
        const auto vector = directional ? QQuaternion::fromEulerAngles(object.rotation).rotatedVector({0,0,1}) : object.position;
        Light light{vector,lightUnits(object),directional};
        if (object.type == "Spot Light") {
            light.spotDirection = spotDirection(object);
            light.spotAngle = float(qDegreesToRadians(spotConeAngle(object)));
            light.spotBlend = float(spotConeBlend(object));
        }
        lights.push_back(light);
    }
    return lights;
}

// Sky light for the shading model, cached because it changes only with the sky model, its colors and the sun.
// revision changes whenever the sky light does.
const wf::color::SkyLight &cachedSkyLight(const WorldAtmosphere &environment,const Light &sun,uint64_t &revision)
{
    static std::optional<wf::color::SkyLight> cached;
    static std::array<float,11> key{};
    static uint64_t counter = 0;
    const auto rgb = [](const QColor &color) { return wf::color::Rgb{float(color.redF()),float(color.greenF()),float(color.blueF())}; };
    const std::array<float,11> next{float(environment.horizon.redF()),float(environment.horizon.greenF()),
        float(environment.horizon.blueF()),float(environment.zenith.redF()),float(environment.zenith.greenF()),
        float(environment.zenith.blueF()),sun.vector.x(),sun.vector.y(),sun.vector.z(),sun.intensity,
        environment.physicalSky ? 1.0f : 0.0f};
    if (!cached || next != key) {
        const std::array towardSun{sun.vector.x(),sun.vector.y(),sun.vector.z()};
        cached = environment.physicalSky ? wf::color::physicalSkyLight(towardSun,sun.intensity)
            : wf::color::skyLight(rgb(environment.horizon),rgb(environment.zenith),towardSun,sun.intensity);
        key = next;
        ++counter;
    }
    revision = counter;
    return *cached;
}

struct Geometry {
    // Scales the ambient term by the sky's brightness (color::skyAmbientScale); 1 for the default sky.
    float ambient = 1;
    struct FaceRange { std::size_t triangles,triangleEnd,lines,lineEnd; };
    std::vector<Vertex> triangles, lines, overlays, selectionOutline;
    std::vector<PickTriangle> picks;
    std::vector<FaceRange> faceRanges;
    std::vector<float> faceLighting;
    std::vector<uint32_t> texturePixels;
    QVector3D minimum, maximum;
    bool hasBounds = false;
    bool animated = false;

    void extendBounds(QVector3D p)
    {
        if (!hasBounds) {
            minimum = maximum = p;
            hasBounds = true;
        }
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis],p[axis]);
            maximum[axis] = std::max(maximum[axis],p[axis]);
        }
    }

    void append(std::vector<Vertex> &list, QVector3D p, QVector3D color,float wind = 0,float sun = 0)
    {
        list.push_back({{p.x(), p.y(), p.z()}, {color.x(), color.y(), color.z()},wind,sun});
    }

    void line(QVector3D a, QVector3D b, QVector3D color)
    {
        append(lines, a, color);
        append(lines, b, color);
    }

    void overlay(QVector3D a, QVector3D b, QVector3D color)
    {
        append(overlays, a, color);
        append(overlays, b, color);
    }

    void outlineSegment(QVector3D a, QVector3D b)
    {
        // The outline shader expands each segment into a screen-space strip.
        // Its vertex inputs reuse the scene buffer slots for UVs and normals.
        constexpr float corners[][2]{{0,-1},{1,-1},{1,1},{0,-1},{1,1},{0,1}};
        for (const auto &corner : corners) {
            Vertex vertex{{a.x(),a.y(),a.z()}};
            std::ranges::copy(corner,vertex.uv);
            vertex.materialNormal[0] = b.x();
            vertex.materialNormal[1] = b.y();
            vertex.materialNormal[2] = b.z();
            selectionOutline.push_back(vertex);
        }
    }

    void addMesh(const SceneObject &object, const wf::Mesh &mesh, bool modelOnly, bool selected,
                 const std::vector<Light> &lights, bool pickable = true,const wf::Heightfield *field = nullptr,
                 float windStrength = 0,bool trackFaces = false,bool wire = true,bool clay = false,bool textured = false,
                 const QMatrix4x4 *worldTransform = nullptr,int paintSlot = -1,const QImage &paint = {},
                 std::span<const ImageRange> worldImages = {}, const QVector3D *outlineEye = nullptr,
                 const wf::MeshClip *clip = nullptr,double clipSeconds = 0)
    {
        std::vector<uint32_t> tiles;
        if (textured) {
            tiles.resize(mesh.materials.size());
            const auto hasPaint = [&](std::size_t slot) { return int(slot) == paintSlot && !paint.isNull(); };
            std::size_t count = 0, materialCount = 0;
            for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
                const auto &slot = mesh.materials[i];
                const auto maps = slot.previewTextures();
                const auto mapCount = std::ranges::count_if(maps,[](const auto *image) { return !image->empty(); })
                    + (hasPaint(i) && slot.baseColorTexture.empty() ? 1 : 0);
                count += mapCount;
                materialCount += mapCount > 0 || slot.emissive != wf::Point3{};
            }
            const auto capacity = std::size_t(WorldLightingVulkan::textureCapacity);
            const auto headers = 1+materialWords*materialCount;
            const auto side = headers < capacity ? int(std::sqrt((capacity-headers)/std::max<std::size_t>(count,1))) : 0;
            if (side > 0 && headers > 1)
                texturePixels.resize(headers);
            std::size_t header = 1;
            for (std::size_t i = 0; side > 0 && i < mesh.materials.size(); ++i) {
                const auto &slot = mesh.materials[i];
                const auto maps = slot.previewTextures();
                if (!hasPaint(i) && slot.emissive == wf::Point3{}
                    && std::ranges::none_of(maps,[](const auto *image) { return !image->empty(); }))
                    continue;
                tiles[i] = uint32_t(header);
                for (const auto *image : maps) {
                    const auto range = hasPaint(i) && image == &slot.baseColorTexture
                        ? appendMaterialImage(texturePixels,paint.width() > side || paint.height() > side
                            ? paint.scaled(paint.size().scaled(side,side,Qt::KeepAspectRatio).expandedTo({1,1}),
                                Qt::IgnoreAspectRatio,Qt::SmoothTransformation)
                            : paint)
                        : loadMaterialImage(texturePixels,*image,side);
                    std::ranges::copy(range,texturePixels.begin()+header);
                    header += range.size();
                }
                for (const auto value : slot.emissive)
                    texturePixels[header++] = std::bit_cast<uint32_t>(value);
            }
        }
        QMatrix4x4 transform;
        if (worldTransform) {
            transform = *worldTransform;
        } else if (!modelOnly) {
            transform.translate(object.position);
            transform.rotate(QQuaternion::fromEulerAngles(object.rotation));
            transform.scale(object.scale);
        }
        std::vector<QVector3D> positions;
        std::vector<wf::Point3> poseNormals;
        positions.reserve(mesh.vertices.size());
        const auto frame = clip ? clip->phase(clipSeconds) : wf::MeshClip::Sample{};
        if (clip)
            poseNormals.reserve(mesh.vertices.size());
        for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
            auto vertex = mesh.vertices[index];
            if (clip) {
                const auto pose = clip->vertex(frame,index);
                vertex = pose.position;
                poseNormals.push_back(pose.normal);
            }
            const auto p = transform.map(point(vertex));
            positions.push_back(p);
            extendBounds(p);
        }
        const auto normals = transform.normalMatrix();
        const auto transformNormal = [&](const wf::Point3 &normal) {
            QVector3D result;
            for (int row = 0; row < 3; ++row)
                result[row] = normals(row,0)*normal[0] + normals(row,1)*normal[1] + normals(row,2)*normal[2];
            return result;
        };
        struct EdgeFacing {
            std::size_t faces = 0;
            bool front = false;
            bool back = false;
        };
        // Positions join render vertices split by UVs or normals at a shared edge.
        std::map<std::array<wf::Point3,2>, EdgeFacing> outlineEdges;
        const QVector3D light = QVector3D(-0.45f, -0.7f, 1.0f).normalized();
        if (trackFaces)
            faceRanges.reserve(mesh.faces.size());
        for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
            const auto &face = mesh.faces[i];
            const auto &material = mesh.materials[face.material];
            const auto firstTriangle = triangles.size(), firstLine = lines.size();
            const QVector3D base = clay ? QVector3D(0.64f,0.66f,0.71f)
                : QVector3D(material.color[0],material.color[1],material.color[2]);
            const auto defaultNormal = wf::faceNormal(mesh, face);
            const auto addCorner = [&](const wf::MeshCorner &corner) {
                const auto n = clip ? poseNormals[corner.vertex] : corner.normal == wf::Point3{} ? defaultNormal : corner.normal;
                const auto worldNormal = transformNormal(n).normalized();
                const auto p = positions[corner.vertex];
                float strength = modelOnly ? 0.28f : ambient*(0.34f + 0.16f*std::max(0.0f,worldNormal.z()));
                float key = 0;
                if (lights.empty())
                    strength += 0.72f*std::max(0.0f, QVector3D::dotProduct(worldNormal, light));
                for (const auto &source : lights) {
                    const auto offset = source.directional ? source.vector : source.vector - p;
                    const auto attenuation = source.directional ? 1.0f : 1.0f/(1.0f + offset.lengthSquared());
                    const float contribution = source.intensity * attenuation
                        * std::max(0.0f, QVector3D::dotProduct(worldNormal, offset.normalized()));
                    strength += contribution;
                    if (&source == &lights.front() && source.directional)
                        key = contribution;
                }
                const auto surface = linearColor(field ? point(field->surfaceColor(mesh.vertices[corner.vertex],n)) : base);
                const float wind = windStrength > 0
                    ? wf::Heightfield::foliageSway(mesh,corner.vertex)*windStrength : 0;
                animated |= wind > 0;
                const float clamped = std::min(strength,1.0f);
                const float weight = (clamped-std::min(strength-key,1.0f))/std::max(clamped,0.001f);
                append(triangles,p,surface*clamped,wind,weight);
                if (!modelOnly) {
                    // World views light every surface per pixel (Shading.glsl): pass albedo, normal and material.
                    auto &vertex = triangles.back();
                    vertex.color[0] = surface.x();
                    vertex.color[1] = surface.y();
                    vertex.color[2] = surface.z();
                    vertex.materialNormal[0] = worldNormal.x();
                    vertex.materialNormal[1] = worldNormal.y();
                    vertex.materialNormal[2] = worldNormal.z();
                    vertex.surfaceFactors[0] = std::clamp(material.metallic,0.0f,1.0f);
                    vertex.surfaceFactors[1] = std::clamp(material.roughness,0.045f,1.0f);
                }
                if (textured) {
                    auto &vertex = triangles.back();
                    vertex.color[0] = surface.x();
                    vertex.color[1] = surface.y();
                    vertex.color[2] = surface.z();
                    vertex.uv[0] = corner.uv[0];
                    vertex.uv[1] = 1-corner.uv[1];
                    vertex.texture[0] = tiles[face.material];
                    vertex.materialNormal[0] = worldNormal.x();
                    vertex.materialNormal[1] = worldNormal.y();
                    vertex.materialNormal[2] = worldNormal.z();
                    vertex.surfaceFactors[0] = std::clamp(material.metallic,0.0f,1.0f);
                    vertex.surfaceFactors[1] = std::clamp(material.roughness,0.045f,1.0f);
                } else if (!worldImages.empty()) {
                    auto &vertex = triangles.back();
                    vertex.uv[0] = corner.uv[0];
                    vertex.uv[1] = 1-corner.uv[1];
                    const auto &range = worldImages[face.material];
                    std::ranges::copy(range,vertex.texture);
                    if (range[0] && !range[1]) {
                        vertex.color[0] = surface.x();
                        vertex.color[1] = surface.y();
                        vertex.color[2] = surface.z();
                        vertex.materialNormal[0] = worldNormal.x();
                        vertex.materialNormal[1] = worldNormal.y();
                        vertex.materialNormal[2] = worldNormal.z();
                        vertex.surfaceFactors[0] = material.metallic;
                        vertex.surfaceFactors[1] = std::max(material.roughness,0.045f);
                    }
                }
                if (trackFaces)
                    faceLighting.push_back(clamped);
            };
            wf::visitFaceTriangles(mesh,face,[&](const auto &triangle) {
                const auto &a = face.corners[triangle[0]];
                const auto &b = face.corners[triangle[1]];
                const auto &c = face.corners[triangle[2]];
                addCorner(a);
                addCorner(b);
                addCorner(c);
                if (pickable)
                    picks.push_back({positions[a.vertex], positions[b.vertex], positions[c.vertex], object.id, int(i)});
            });
            if (outlineEye) {
                const auto origin = positions[face.corners.front().vertex];
                const bool front = QVector3D::dotProduct(transformNormal(defaultNormal),*outlineEye-origin) > 0;
                for (std::size_t j = 0; j < face.corners.size(); ++j) {
                    const auto &a = mesh.vertices[face.corners[j].vertex];
                    const auto &b = mesh.vertices[face.corners[(j+1)%face.corners.size()].vertex];
                    if (a == b)
                        continue;
                    auto &edge = outlineEdges[{std::min(a,b),std::max(a,b)}];
                    ++edge.faces;
                    edge.front |= front;
                    edge.back |= !front;
                }
            } else if (wire && (selected || modelOnly)) {
                const QVector3D color = modelOnly ? QVector3D(0.36f,0.42f,0.52f) : QVector3D(0.68f,0.43f,0.98f);
                for (std::size_t j = 0; j < face.corners.size(); ++j)
                    line(positions[face.corners[j].vertex],
                         positions[face.corners[(j+1)%face.corners.size()].vertex], color);
            }
            if (trackFaces)
                faceRanges.push_back({firstTriangle,triangles.size(),firstLine,lines.size()});
        }
        for (const auto &[ends,facing] : outlineEdges)
            if (facing.faces == 1 || (facing.front && facing.back))
                outlineSegment(transform.map(point(ends[0])),transform.map(point(ends[1])));
    }
};

float hitDistance(const auto &triangle,QVector3D origin,QVector3D direction)
{
    constexpr float miss = std::numeric_limits<float>::infinity();
    const auto u = triangle.b - triangle.a;
    const auto v = triangle.c - triangle.a;
    const auto p = QVector3D::crossProduct(direction, v);
    const auto determinant = QVector3D::dotProduct(u, p);
    if (std::abs(determinant) < 1e-8f)
        return miss;
    const auto delta = origin - triangle.a;
    const float s = QVector3D::dotProduct(delta, p) / determinant;
    const auto q = QVector3D::crossProduct(delta, u);
    const float t = QVector3D::dotProduct(direction, q) / determinant;
    const float depth = QVector3D::dotProduct(v, q) / determinant;
    constexpr float edgeTolerance = 1e-6f;
    return s >= -edgeTolerance && t >= -edgeTolerance && s + t <= 1+edgeTolerance && depth > 0 ? depth : miss;
}

struct PickRay {
    QVector3D origin, direction;
};

// The index tracks the editable base mesh. Highlighting and camera motion do not rebuild it.
class ModelPickIndex {
public:
    void setMesh(const QSharedPointer<const wf::Mesh> &mesh)
    {
        if (mesh == mesh_)
            return;
        mesh_ = mesh;
        indexReady_ = false;
        triangles_.clear();
        nodes_.clear();
        edges_.reset();
        looseVertices_.clear();
        unindexedFaces_.clear();
    }

    struct Hit {
        float distance = std::numeric_limits<float>::infinity();
        int face = -1;
    };

    Hit hit(const PickRay &ray,const std::vector<bool> *excludedFaces = nullptr)
    {
        buildIfNeeded();
        Hit result;
        if (!nodes_.empty())
            intersect(0,ray,result,excludedFaces);
        return result;
    }

    std::vector<int> facesNear(const QMatrix4x4 &matrix,QSize size,const QRectF &area)
    {
        buildIfNeeded();
        auto result = unindexedFaces_;
        if (nodes_.empty())
            return result;
        const auto collect = [&](auto &&self,std::size_t index) -> void {
            const auto &node = nodes_[index];
            float left = std::numeric_limits<float>::infinity(), top = left, right = -left, bottom = -left;
            bool intersectsEyePlane = false;
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto clip = matrix*QVector4D(corner&1 ? node.low.x() : node.high.x(),
                    corner&2 ? node.low.y() : node.high.y(),corner&4 ? node.low.z() : node.high.z(),1);
                if (clip.w() <= 0) {
                    intersectsEyePlane = true;
                    break;
                }
                const auto x = (clip.x()/clip.w()+1)*float(size.width())/2;
                const auto y = (clip.y()/clip.w()+1)*float(size.height())/2;
                left = std::min(left,x);
                right = std::max(right,x);
                top = std::min(top,y);
                bottom = std::max(bottom,y);
            }
            if (!intersectsEyePlane && (right < area.left() || left > area.right() || bottom < area.top() || top > area.bottom()))
                return;
            if (node.left) {
                self(self,node.left);
                self(self,node.right);
            } else {
                for (auto i = node.begin; i < node.end; ++i)
                    result.push_back(triangles_[i].face);
            }
        };
        collect(collect,0);
        std::ranges::sort(result);
        result.erase(std::unique(result.begin(),result.end()),result.end());
        return result;
    }

    const std::vector<int> &looseVertices() const { return looseVertices_; }

private:
    void buildIfNeeded()
    {
        if (mesh_ && !indexReady_) {
            indexReady_ = true;
            triangles_.reserve(mesh_->faces.size());
            std::vector<bool> referenced(mesh_->vertices.size());
            auto low = point(mesh_->vertices.front()), high = low;
            for (const auto &vertex : mesh_->vertices)
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis],vertex[axis]);
                    high[axis] = std::max(high[axis],vertex[axis]);
                }
            for (std::size_t i = 0; i < mesh_->faces.size(); ++i) {
                const auto &face = mesh_->faces[i];
                for (const auto &corner : face.corners)
                    referenced[corner.vertex] = true;
                const auto before = triangles_.size();
                wf::visitFaceTriangles(*mesh_,face,[&](const auto &triangle) {
                    const auto p = [&](std::size_t j) { return point(mesh_->vertices[face.corners[triangle[j]].vertex]); };
                    const auto a = p(0), b = p(1), c = p(2);
                    std::uint32_t key = 0;
                    for (int axis = 0; axis < 3; ++axis) {
                        const auto extent = double(high[axis])-low[axis];
                        const auto center = (double(a[axis])+b[axis]+c[axis])/3;
                        const auto coordinate = extent > 0 ? std::clamp((center-low[axis])/extent,0.0,1.0) : 0;
                        key |= spreadBits(std::uint32_t(1023*coordinate)) << axis;
                    }
                    triangles_.push_back({a,b,c,int(i),key});
                });
                if (triangles_.size() == before)
                    unindexedFaces_.push_back(int(i));
            }
            for (std::size_t i = 0; i < referenced.size(); ++i)
                if (!referenced[i])
                    looseVertices_.push_back(int(i));
            if (!triangles_.empty()) {
                // Spatially adjacent triangles share a Morton-code prefix.
                std::ranges::sort(triangles_,{},&Triangle::key);
                nodes_.reserve(triangles_.size()/3);
                build(0,triangles_.size());
            }
        }
    }

public:
    const std::vector<ModelComponent> &edges()
    {
        if (!edges_) {
            auto &edges = edges_.emplace();
            if (mesh_) {
                for (const auto &face : mesh_->faces)
                    for (std::size_t j = 0; j < face.corners.size(); ++j) {
                        const auto [a,b] = std::minmax(face.corners[j].vertex,face.corners[(j+1)%face.corners.size()].vertex);
                        edges.push_back({int(a),int(b)});
                    }
                std::ranges::sort(edges);
                edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
            }
        }
        return *edges_;
    }

private:
    static std::uint32_t spreadBits(std::uint32_t value)
    {
        value = (value | (value<<16)) & 0x030000ff;
        value = (value | (value<<8)) & 0x0300f00f;
        value = (value | (value<<4)) & 0x030c30c3;
        return (value | (value<<2)) & 0x09249249;
    }

    struct Node {
        QVector3D low, high;
        std::size_t begin = 0, end = 0, left = 0, right = 0;
    };

    std::size_t build(std::size_t begin,std::size_t end)
    {
        const auto index = nodes_.size();
        Node node{triangles_[begin].a,triangles_[begin].a,begin,end};
        nodes_.push_back(node);
        if (end-begin > 8) {
            const auto mid = begin+(end-begin)/2;
            node.left = build(begin,mid);
            node.right = build(mid,end);
            for (int axis = 0; axis < 3; ++axis) {
                node.low[axis] = std::min(nodes_[node.left].low[axis],nodes_[node.right].low[axis]);
                node.high[axis] = std::max(nodes_[node.left].high[axis],nodes_[node.right].high[axis]);
            }
        } else {
            for (auto i = begin; i < end; ++i) {
                const auto &tri = triangles_[i];
                for (int axis = 0; axis < 3; ++axis) {
                    node.low[axis] = std::min({node.low[axis],tri.a[axis],tri.b[axis],tri.c[axis]});
                    node.high[axis] = std::max({node.high[axis],tri.a[axis],tri.b[axis],tri.c[axis]});
                }
            }
        }
        nodes_[index] = node;
        return index;
    }

    void intersect(std::size_t index,const PickRay &ray,Hit &hit,const std::vector<bool> *excludedFaces) const
    {
        const auto &node = nodes_[index];
        float entry = 0, exit = hit.distance;
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = node.low[axis]-ray.origin[axis]-0.000002f;
            const float hi = node.high[axis]-ray.origin[axis]+0.000002f;
            if (std::abs(ray.direction[axis]) < 1e-8f) {
                if (lo > 0 || hi < 0)
                    return;
                continue;
            }
            const float a = lo/ray.direction[axis], b = hi/ray.direction[axis];
            entry = std::max(entry,std::min(a,b));
            exit = std::min(exit,std::max(a,b));
            if (entry > exit)
                return;
        }
        if (node.left) {
            intersect(node.left,ray,hit,excludedFaces);
            intersect(node.right,ray,hit,excludedFaces);
            return;
        }
        for (std::size_t i = node.begin; i < node.end; ++i) {
            const auto &triangle = triangles_[i];
            if (excludedFaces && (*excludedFaces)[triangle.face])
                continue;
            const float distance = hitDistance(triangle,ray.origin,ray.direction);
            if (distance < hit.distance || (distance == hit.distance && triangle.face < hit.face))
                hit = {distance,triangle.face};
        }
    }

    QSharedPointer<const wf::Mesh> mesh_;
    struct Triangle { QVector3D a,b,c; int face; std::uint32_t key; };
    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    std::optional<std::vector<ModelComponent>> edges_;
    std::vector<int> looseVertices_, unindexedFaces_;
    bool indexReady_ = false;
};
}

class VulkanSceneRenderer;

class VulkanSceneWindow : public QVulkanWindow {
public:
    VulkanSceneWindow(ProjectStore &store, bool modelOnly, LiveViewport &owner)
        : store_(store), modelOnly_(modelOnly), owner_(owner)
    {
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] {
            elapsed_ += std::clamp(double(clock_.restart())/1000.0,0.0,0.1);
            while (elapsed_ >= wf::WorldSimulation::stepSeconds) {
                tick();
                elapsed_ -= wf::WorldSimulation::stepSeconds;
            }
            invalidate();
        });
        windClock_.start();
        auto *wind = new QTimer(this);
        wind->setInterval(40);
        connect(wind,&QTimer::timeout,this,[this] {
            if (isExposed() && geometry_.animated)
                requestUpdate();
        });
        wind->start();
    }

    using RunMode = LiveViewport::RunMode;
    RunMode mode() const { return mode_; }
    void setBrush(float radius, float strength, bool smooth)
    {
        brushRadius_ = radius;
        brushStrength_ = strength;
        brushSmooth_ = smooth;
        brushLayer_ = -1;
    }
    void setLayerBrush(float radius,int layer) { brushRadius_ = radius; brushLayer_ = layer; }
    bool start(RunMode mode)
    {
        if (modelOnly_ || !rendered_ || runtime_ || mode == RunMode::Editing)
            return false;
        WorldRuntime runtime;
        if (!runtime.start(store_, mode == RunMode::Play)) {
            report(QObject::tr("Open a project with at most 256 box colliders to start the runtime."));
            return false;
        }
        for (const auto &problem : runtime.gameplayErrors())
            qWarning().noquote() << problem;
        runtime_ = std::move(runtime);
        worldImagesDirty_ = true;
        mode_ = mode;
        savedTarget_ = target_;
        savedDistance_ = distance_;
        savedYaw_ = yaw_;
        savedPitch_ = pitch_;
        if (const auto player = runtime_->player()) {
            target_ = *player;
            distance_ = 5;
            requestActivate();
        }
        elapsed_ = 0;
        clock_.start();
        timer_.start();
        invalidate();
        return true;
    }
    void stop()
    {
        if (!runtime_)
            return;
        timer_.stop();
        mode_ = RunMode::Editing;
        runtime_->stopNative();
        runtime_.reset();
        worldImagesDirty_ = true;
        target_ = savedTarget_;
        distance_ = savedDistance_;
        yaw_ = savedYaw_;
        pitch_ = savedPitch_;
        invalidate();
    }
    void nativeGameplayChanged()
    {
        if (!runtime_ || mode_ != RunMode::Play)
            return;
        const auto path = store_.nativeGameplayPath();
        if (!QFileInfo::exists(path)) {
            runtime_->stopNative();
            return;
        }
        QString error;
        if (runtime_->stageNative(path,error))
            qInfo().noquote() << QObject::tr("C++ gameplay staged for the next fixed update.");
        else
            qWarning().noquote() << error;
    }
    void pause(bool paused)
    {
        if (!runtime_)
            return;
        if (paused) {
            timer_.stop();
        } else {
            clock_.restart();
            timer_.start();
        }
    }
    void stepOnce()
    {
        if (runtime_ && !timer_.isActive()) {
            tick();
            invalidate();
        }
    }
    std::optional<QVector3D> runtimePosition(const QString &id) const
    {
        return runtime_ ? runtime_->position(id) : std::nullopt;
    }
    QString runtimeObjective() const { return runtime_ ? runtime_->objective() : QString(); }
    bool runtimeShowsHud() const { return runtime_ && runtime_->gameplay().showMissionHud; }
    std::optional<QVector3D> playerPosition() const
    {
        return runtime_ ? runtime_->player() : std::nullopt;
    }

    QVulkanWindowRenderer *createRenderer() override;
    void invalidate()
    {
        dirty_ = true;
        requestUpdate();
    }
    void cameraChanged()
    {
        const bool projectedGuides = modelOnly_ && (domain_ == ModelDomain::Vertex || gizmo_ || marquee_ || knifeStart_
            || !retopologyPoints_.isEmpty() || !measurement_.isEmpty());
        const auto *selected = modelOnly_ || runtime_ ? nullptr : store_.selectedObject();
        if ((!modelOnly_ && (!terrains_.isEmpty() || hasLod_ || (selected && !selected->meshAsset.isEmpty())))
            || projectedGuides)
            dirty_ = true;
        requestUpdate();
    }
    void sceneEdited()
    {
        worldImagesDirty_ = true;
        for (auto it = terrains_.begin(); it != terrains_.end();) {
            const auto *object = store_.object(it.key());
            if (!object || !object->visible)
                it = terrains_.erase(it);
            else
                ++it;
        }
        invalidate();
    }
    void setModelSelection(ModelDomain domain, const std::set<ModelComponent> &selection, bool xray)
    {
        if (domain_ != domain || selection_ != selection || xray_ != xray) {
            const bool rebuild = domain_ != ModelDomain::Face || domain != ModelDomain::Face || xray_ || xray;
            domain_ = domain;
            selection_ = selection;
            xray_ = xray;
            if (rebuild)
                invalidate();
            else
                requestUpdate();
        }
    }
    void setModelPreview(QSharedPointer<const wf::Mesh> mesh, std::optional<ModelGizmo> gizmo)
    {
        if (gizmo_ && !gizmo) {
            modelPress_.reset();
            handleDrag_ = false;
        }
        preview_ = std::move(mesh);
        if (sculpt_)
            modelDrawMesh_.reset();
        gizmo_ = std::move(gizmo);
        invalidate();
    }
    void setAssetPreview(QSharedPointer<const wf::Mesh> mesh,bool reframe)
    {
        if (assetPreview_ == mesh)
            return;
        assetPreview_ = std::move(mesh);
        invalidate();
        if (reframe)
            frame();
    }
    void setPlacementPreview(const SceneObject &placement,QSharedPointer<const wf::Mesh> mesh)
    {
        placementPreview_ = placement;
        placementPreviewMesh_ = std::move(mesh);
        worldImagesDirty_ = true;
        frame();
    }
    void setCameraPreview(QVector3D eye,QVector3D target)
    {
        cameraPreview_ = std::pair{eye,target};
        framed_ = true;
        invalidate();
    }
    void setObjectPreview(bool enabled,const SceneObject *object)
    {
        if (objectView_ == enabled && !object && !objectPreview_)
            return;
        objectView_ = enabled;
        objectPreview_ = object ? std::optional{*object} : std::nullopt;
        modelDrawMesh_.reset();
        invalidate();
    }
    void setBaseColorPreview(int slot,QImage image)
    {
        paintSlot_ = slot;
        paintPreview_ = std::move(image);
        modelDrawMesh_.reset();
        invalidate();
    }
    void setModelOrthographic(bool enabled)
    {
        if (modelOnly_ && ortho_ != enabled) {
            ortho_ = enabled;
            invalidate();
        }
    }
    void setModelShading(ModelShading mode)
    {
        if (modelOnly_ && modelShading_ != mode) {
            modelShading_ = mode;
            modelDrawMesh_.reset();
            invalidate();
        }
    }
    void setModelAxisView(int axis)
    {
        if (!modelOnly_ || axis < 0 || axis > 2)
            return;
        constexpr float halfPi = 1.57079632679f;
        yaw_ = axis == 0 ? 0.0f : -halfPi;
        pitch_ = axis == 2 ? halfPi : 0.0f;
        ortho_ = true;
        cameraChanged();
    }
    void setModelReference(const QString &id)
    {
        if (referenceId_ != id) {
            referenceId_ = id;
            invalidate();
        }
    }
    void setModelLodPreview(bool enabled)
    {
        modelLodPreview_ = enabled;
        invalidate();
    }
    void setModelKnife(bool enabled,std::optional<QVector3D> start)
    {
        knife_ = enabled;
        knifeStart_ = start;
        marquee_.reset();
        modelPress_.reset();
        invalidate();
    }
    void setModelRetopology(bool enabled,const QList<QVector3D> &points)
    {
        retopology_ = enabled;
        retopologyPoints_ = points;
        marquee_.reset();
        modelPress_.reset();
        invalidate();
    }
    void setSculptMode(bool enabled)
    {
        sculpt_ = enabled;
        sculptDown_ = false;
        setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
        invalidate();
    }
    void setModelSurfaceTool(bool enabled)
    {
        surfaceTool_ = enabled;
        setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
        modelPress_.reset();
        invalidate();
    }
    void setModelPairPicking(bool enabled)
    {
        pairPicking_ = enabled;
        modelPress_.reset();
        pairGuide_.reset();
        marquee_.reset();
        setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
        invalidate();
    }
    void emitPairPoint(QPointF pixel,bool pressed)
    {
        const auto components = pickComponents(pixel,{},domain_);
        const auto hit = pickIndex_.hit(pickRay(camera(size()).inverted(),pixel));
        emit owner_.modelPairPoint(components.isEmpty() ? ModelComponent{-1} : components.front(),pressed,hit.face);
    }
    void setModelMarkup(const QList<QVector3D> &measurement,const QList<QVector3D> &stroke)
    {
        measurement_ = measurement;
        annotationStroke_ = stroke;
        invalidate();
    }
    std::optional<qsizetype> annotationNear(QVector3D position)
    {
        const auto *object = store_.selectedObject();
        const auto pixel = screenPoint(position);
        if (!object || !pixel)
            return {};
        const auto strokes = object->details.value("modelAnnotations").toArray();
        std::optional<qsizetype> nearest;
        qreal distance = 12;
        for (qsizetype i = 0; i < strokes.size(); ++i) {
            std::optional<QPointF> previous;
            for (const auto &vertex : strokes[i].toArray()) {
                const auto coordinates = vertex.toArray();
                const auto next = coordinates.size() == 3
                    ? screenPoint({float(coordinates[0].toDouble()),float(coordinates[1].toDouble()),
                        float(coordinates[2].toDouble())})
                    : std::nullopt;
                if (previous && next) {
                    const auto separation = distanceToSegment(*pixel,*previous,*next).first;
                    if (separation < distance) {
                        nearest = i;
                        distance = separation;
                    }
                }
                previous = next;
            }
        }
        return nearest;
    }
    void surfaceToolPoint(QPointF pixel,bool first)
    {
        const auto *asset = store_.selectedMesh();
        if (!asset)
            return;
        pickIndex_.setMesh(asset->mesh);
        const auto ray = pickRay(camera(size()).inverted(),pixel);
        const auto hit = pickIndex_.hit(ray);
        if (hit.face >= 0)
            emit owner_.modelSurfaceToolPoint(ray.origin+ray.direction*hit.distance,first);
    }
    void sculptSampleAt(QPointF pixel,Qt::KeyboardModifiers modifiers)
    {
        const auto *asset = store_.selectedMesh();
        if (!asset)
            return;
        pickIndex_.setMesh(asset->mesh);
        const auto ray = pickRay(camera(size()).inverted(),pixel);
        const auto hit = pickIndex_.hit(ray);
        if (hit.face >= 0)
            emit owner_.sculptSample(ray.origin+ray.direction*hit.distance,
                point(wf::faceNormal(*asset->mesh,asset->mesh->faces[hit.face])),modifiers);
    }
    std::optional<QVector3D> conformToReference(const QVector3D &position)
    {
        const auto view = camera(size());
        const auto pixel = screenPoint(view,position,false);
        const auto reference = referenceMesh();
        if (!pixel || !reference)
            return {};
        auto ray = pickRay(view.inverted(),*pixel);
        // A close frame can put the camera inside the reference. Project from
        // the front of its bounds so the fit does not jump to the far surface.
        QVector3D front;
        for (int axis = 0; axis < 3; ++axis)
            front[axis] = ray.direction[axis] >= 0 ? referenceLow_[axis] : referenceHigh_[axis];
        const auto setback = QVector3D::dotProduct(front-ray.origin,ray.direction);
        if (setback <= 0)
            ray.origin += ray.direction*(setback-std::max(0.0001f,(referenceHigh_-referenceLow_).length()*0.00001f));
        const auto hit = referenceIndex_.hit(ray);
        return hit.face >= 0 ? std::optional{ray.origin+ray.direction*hit.distance} : std::nullopt;
    }
    void frame()
    {
        if (playerPosition())
            return;
        Geometry selectedBounds;
        if (placementPreviewMesh_) {
            QMatrix4x4 transform;
            transform.translate(placementPreview_.position);
            transform.rotate(QQuaternion::fromEulerAngles(placementPreview_.rotation));
            transform.scale(placementPreview_.scale);
            for (const auto &vertex : placementPreviewMesh_->vertices)
                selectedBounds.extendBounds(transform.map(point(vertex)));
        } else if (modelOnly_ && !selection_.empty())
            if (const auto *asset = store_.selectedMesh())
                for (const auto index : modelVertices(*asset->mesh,domain_,selection_))
                    selectedBounds.extendBounds(point(asset->mesh->vertices[index]));
        const auto &data = selectedBounds.hasBounds ? selectedBounds : geometry();
        target_ = data.hasBounds ? (data.minimum + data.maximum)*0.5f : QVector3D();
        distance_ = data.hasBounds ? std::max(0.3f, (data.maximum - data.minimum).length()*1.55f) : 5.0f;
        framed_ = data.hasBounds;
        cameraChanged();
    }
    bool rendered() const { return rendered_; }
    bool temporalSettled() const { return temporalSettled_; }
    void previewTerrain()
    {
        const auto *object = store_.selectedObject();
        if (!object || object->type != "Terrain")
            return;
        const auto field = readHeightfield(*object);
        target_ = object->position + QVector3D(0,0,field.height(0,0).value_or(0)+1.4f);
        distance_ = std::clamp(field.size/8,6.0f,24.0f);
        pitch_ = 0.14f;
        framed_ = true;
        invalidate();
    }
    void report(const QString &status) { emit owner_.rendererStatusChanged(status); }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        previous_ = event->position();
        if (!modelOnly_ || event->button() != Qt::LeftButton)
            return;
        if (surfaceTool_) {
            modelPress_ = previous_;
            surfaceToolPoint(previous_,true);
            return;
        }
        if (sculpt_) {
            if (rendered_ && store_.selectedMesh()) {
                sculptDown_ = true;
                sculptPixel_ = event->position();
                emit owner_.sculptStrokeBegan();
                sculptSampleAt(sculptPixel_,event->modifiers());
            }
            return;
        }
        modelPress_ = previous_;
        if (pairPicking_) {
            emitPairPoint(previous_,true);
            return;
        }
        if (gizmo_ && (gizmo_->passive || (gizmo_->kind == ModelHandle::Rotate && gizmo_->axis < 0))) {
            const auto center = screenPoint(gizmo_->pivot);
            int axis = -1;
            qreal best = 8;
            if (center && QLineF(*center,previous_).length() > 9) {
                for (int i = 0; i < 3; ++i) {
                    const auto line = gizmoPath(i);
                    for (std::size_t j = 1; j < line.size(); ++j) {
                        const auto a = screenPoint(line[j-1]), b = screenPoint(line[j]);
                        if (!a || !b || QLineF(*a,*b).length() < 0.1)
                            continue;
                        const auto distance = distanceToSegment(previous_,*a,*b).first;
                        if (distance < best) {
                            best = distance;
                            axis = i;
                        }
                    }
                }
            }
            if (axis >= 0 || (gizmo_->passive && center && QLineF(*center,previous_).length() <= 9)) {
                handleDrag_ = true;
                if (gizmo_->kind == ModelHandle::Rotate && axis >= 0)
                    rotationDrag_ = RotationDrag{camera(size()).inverted(),gizmo_->pivot,gizmo_->axes[axis].normalized()};
                emit owner_.modelHandlePressed(axis);
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const auto delta = event->position() - previous_;
        previous_ = event->position();
        if (sculptDown_ && event->buttons().testFlag(Qt::LeftButton)) {
            const auto offset = previous_-sculptPixel_;
            const auto distance = std::hypot(offset.x(),offset.y());
            const auto steps = int(distance/6);
            for (int i = 0; i < steps; ++i) {
                sculptPixel_ += offset*(6/distance);
                sculptSampleAt(sculptPixel_,event->modifiers());
            }
            return;
        }
        if (event->buttons().testFlag(Qt::RightButton)) {
            yaw_ -= float(delta.x())*0.008f;
            pitch_ = std::clamp(pitch_ + float(delta.y())*0.008f, -1.48f, 1.48f);
            cameraChanged();
        } else if (event->buttons().testFlag(Qt::MiddleButton)) {
            const auto side = QVector3D(-std::sin(yaw_), std::cos(yaw_), 0);
            const auto up = QVector3D::crossProduct(viewDirection(), side).normalized();
            target_ += (side*float(-delta.x()) + up*float(delta.y())) * distance_ / std::max(height(), 1);
            cameraChanged();
        } else if (modelOnly_ && modelPress_ && event->buttons().testFlag(Qt::LeftButton)) {
            if (pairPicking_) {
                pairGuide_ = QLineF(*modelPress_,previous_);
                invalidate();
            } else if (surfaceTool_) {
                if (delta.manhattanLength() > 0)
                    surfaceToolPoint(previous_,false);
            } else if (gizmo_ && !gizmo_->passive) {
                auto snapTarget = gizmo_->vertexSnap ? modelSnapTarget(event->position()) : std::nullopt;
                if (gizmo_->surfaceSnap)
                    snapTarget = modelSurfaceTarget(event->position());
                auto gesture = delta;
                if (gizmo_->kind == ModelHandle::Rotate && gizmo_->axis >= 0) {
                    const auto inverse = rotationDrag_ ? rotationDrag_->inverse : camera(size()).inverted();
                    const auto normal = rotationDrag_ ? rotationDrag_->normal : gizmo_->axes[gizmo_->axis].normalized();
                    const auto pivot = rotationDrag_ ? rotationDrag_->pivot : gizmo_->pivot;
                    const auto onPlane = [&](QPointF pixel) -> std::optional<QVector3D> {
                        const auto ray = pickRay(inverse,pixel);
                        const auto denominator = QVector3D::dotProduct(normal,ray.direction);
                        if (std::abs(denominator) < 0.08f)
                            return {};
                        const auto t = QVector3D::dotProduct(normal,pivot-ray.origin)/denominator;
                        const auto arm = ray.origin+ray.direction*t-pivot;
                        return arm.lengthSquared() > 1e-8f ? std::optional{arm.normalized()} : std::nullopt;
                    };
                    const auto a = onPlane(previous_-delta), b = onPlane(previous_);
                    if (a && b) {
                        const auto sine = QVector3D::dotProduct(normal,QVector3D::crossProduct(*a,*b));
                        const auto cosine = QVector3D::dotProduct(*a,*b);
                        gesture = {std::atan2(sine,cosine)*180.0/3.141592653589793/0.6,0};
                    }
                }
                emit owner_.modelDragged(gesture,modelDisplacement(event->position()-delta,event->position()),snapTarget);
            } else if (!pairPicking_ && !knife_ && !retopology_ && domain_ != ModelDomain::Object && (previous_-*modelPress_).manhattanLength() >= 5) {
                marquee_ = QRectF(*modelPress_,previous_).normalized();
                invalidate();
            }
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !rendered_ || runtime_ || placementPreviewMesh_)
            return;
        if (sculpt_) {
            if (std::exchange(sculptDown_,false))
                emit owner_.sculptStrokeFinished(true);
            return;
        }
        if (modelOnly_) {
            const auto start = std::exchange(modelPress_,{});
            rotationDrag_.reset();
            if (pairPicking_) {
                pairGuide_.reset();
                invalidate();
                if (start)
                    emitPairPoint(event->position(),false);
                return;
            }
            if (surfaceTool_) {
                if (start) {
                    if (*start != event->position())
                        surfaceToolPoint(event->position(),false);
                    emit owner_.modelSurfaceToolReleased();
                }
                return;
            }
            if (std::exchange(handleDrag_,false)) {
                emit owner_.modelHandleReleased();
                return;
            }
            if (!start || (gizmo_ && !gizmo_->passive))
                return;
            if (retopology_) {
                const auto ray = pickRay(camera(size()).inverted(),event->position());
                const auto reference = referenceMesh();
                const auto *asset = store_.selectedMesh();
                const auto mesh = reference ? reference : asset ? asset->mesh : QSharedPointer<const wf::Mesh>{};
                if (mesh) {
                    auto &index = reference ? referenceIndex_ : pickIndex_;
                    index.setMesh(mesh);
                    const auto hit = index.hit(ray);
                    if (hit.face >= 0)
                        emit owner_.modelSurfacePicked(ray.origin+ray.direction*hit.distance,point(wf::faceNormal(*mesh,mesh->faces[hit.face])));
                }
                return;
            }
            if (domain_ == ModelDomain::Object)
                return;
            if (knife_) {
                float fraction = 0;
                const auto edge = pickComponents(event->position(),{},ModelDomain::Edge,nullptr,&fraction);
                if (!edge.empty()) {
                    const auto inverse = camera(size()).inverted();
                    const auto hit = pickIndex_.hit(pickRay(inverse,event->position()));
                    emit owner_.modelCutPointPicked(edge.first(),fraction,hit.face);
                }
                return;
            }
            const auto area = QRectF(*start,event->position()).normalized();
            const bool box = (event->position()-*start).manhattanLength() >= 5;
            if (marquee_) {
                marquee_.reset();
                invalidate();
            }
            emit owner_.componentsPicked(pickComponents(event->position(),box ? std::optional{area} : std::nullopt,domain_),
                event->modifiers());
            return;
        }
        bool invertible = false;
        const auto inverse = camera(size()).inverted(&invertible);
        if (!invertible)
            return;
        const float x = float(event->position().x()*2.0/width() - 1.0);
        const float y = float(event->position().y()*2.0/height() - 1.0);
        const auto origin = inverse.map(QVector3D(x, y, 0));
        const auto direction = (inverse.map(QVector3D(x, y, 1)) - origin).normalized();
        const PickTriangle *best = nullptr;
        float distance = std::numeric_limits<float>::infinity();
        for (const auto &triangle : geometry().picks) {
            const float hit = hitDistance(triangle, origin, direction);
            if (hit < distance) {
                distance = hit;
                best = &triangle;
            }
        }
        if (best && brushRadius_ > 0) {
            const auto id = best->id;
            if (brushLayer_ >= 0)
                store_.paintTerrain(id,origin+direction*distance,brushRadius_,brushLayer_);
            else
                store_.sculptTerrain(id,origin+direction*distance,brushRadius_,brushStrength_,brushSmooth_);
        } else
            store_.select(best ? best->id : QString());
    }
    void wheelEvent(QWheelEvent *event) override
    {
        distance_ = std::clamp(distance_*std::exp(-float(event->angleDelta().y())/650.0f), 0.02f, 1000000.0f);
        cameraChanged();
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape && runtime_) {
            owner_.stop();
        } else if (event->key() == Qt::Key_Escape && std::exchange(sculptDown_,false)) {
            emit owner_.sculptStrokeFinished(false);
        } else if (event->key() == Qt::Key_F) {
            frame();
        } else if (modelOnly_ && !event->isAutoRepeat() && !(event->modifiers() & ~Qt::KeypadModifier)) {
            emit owner_.modelKeyPressed(event->key());
        } else if (runtime_ && !event->isAutoRepeat()) {
            runtime_->key(event->key(), true);
        }
    }
    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (runtime_ && !event->isAutoRepeat())
            runtime_->key(event->key(),false);
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        if (runtime_)
            runtime_->clearInput();
        if (std::exchange(sculptDown_,false))
            emit owner_.sculptStrokeFinished(false);
        modelPress_.reset();
        handleDrag_ = false;
        rotationDrag_.reset();
        if (std::exchange(pairGuide_,{}))
            invalidate();
        if (marquee_) {
            marquee_.reset();
            invalidate();
        }
        QVulkanWindow::focusOutEvent(event);
    }

private:
    friend class VulkanSceneRenderer;
    void tick()
    {
        runtime_->tick(yaw_);
        if (const auto player = runtime_->player())
            target_ = *player;
    }
    WorldAtmosphere atmosphere() const
    {
        if (modelOnly_) {
            WorldAtmosphere studio;
            studio.sky = false;
            studio.fogDensity = 0;
            studio.shadows = false;
            studio.viewTransform = wf::color::ViewTransform::Standard;
            return studio;
        }
        if (runtime_) {
            const auto &objects = runtime_->objects();
            const auto found = std::ranges::find(objects, QString("World Atmosphere"), &SceneObject::type);
            if (found != objects.cend() && found->visible)
                return readAtmosphere(found->details);
            if (found != objects.cend()) {
                WorldAtmosphere hidden;
                hidden.sky = false;
                hidden.fogDensity = 0;
                hidden.localFogRadius = 0;
                return hidden;
            }
            return {};
        }
        if (const auto *environment = store_.environment(); environment && environment->visible)
            return readAtmosphere(environment->details);
        if (store_.environment()) {
            WorldAtmosphere hidden;
            hidden.sky = false;
            hidden.fogDensity = 0;
            return hidden;
        }
        return {};
    }
    QVector3D viewDirection() const
    {
        return {std::cos(yaw_)*std::cos(pitch_), std::sin(yaw_)*std::cos(pitch_), std::sin(pitch_)};
    }
    std::pair<QVector3D,QVector3D> viewPose() const
    {
        if (runtime_)
            if (const auto *shot = runtime_->gameplay().activeShot())
                return {point(wf::shotPosition(*shot,1-shot->remaining/shot->seconds)),point(shot->target)};
        return cameraPreview_.value_or(std::pair{target_+viewDirection()*distance_,target_});
    }
    QMatrix4x4 camera(QSize size)
    {
        QMatrix4x4 projection, view;
        const float aspect = float(size.width()) / std::max(size.height(), 1);
        const float extent = distance_*0.41421356f;
        const float nearPlane = std::max(0.001f, distance_/1000), farPlane = std::max(100.0f, distance_*100.0f);
        if (ortho_)
            projection.ortho(-extent*aspect,extent*aspect,-extent,extent,nearPlane,farPlane);
        else
            projection.perspective(45.0f, aspect, nearPlane, farPlane);
        const auto [eye,target] = viewPose();
        const QVector3D up = std::abs((eye-target).normalized().z()) > 0.99995f
            ? QVector3D(-std::cos(yaw_),-std::sin(yaw_),0) : QVector3D(0,0,1);
        view.lookAt(eye,target,up);
        return clipCorrectionMatrix()*projection*view;
    }

    std::optional<QPointF> screenPoint(QVector3D position)
    {
        return screenPoint(camera(size()),position);
    }

    std::optional<QPointF> screenPoint(const QMatrix4x4 &matrix,QVector3D position,bool clipDepth = true) const
    {
        const auto projected = matrix*QVector4D(position,1);
        if (projected.w() <= 0 || (clipDepth && (projected.z() < 0 || projected.z() > projected.w())))
            return {};
        return QPointF((projected.x()/projected.w()+1)*width()/2,(projected.y()/projected.w()+1)*height()/2);
    }

    QVector3D unproject(const QMatrix4x4 &inverse,QPointF pixel,float depth) const
    {
        return inverse.map(QVector3D(float(pixel.x()*2/std::max(1,width())-1),
            float(pixel.y()*2/std::max(1,height())-1),depth));
    }

    PickRay pickRay(const QMatrix4x4 &inverse,QPointF pixel) const
    {
        const auto origin = unproject(inverse,pixel,0);
        return {origin,(unproject(inverse,pixel,1)-origin).normalized()};
    }

    static std::pair<qreal,qreal> distanceToSegment(QPointF pixel,QPointF a,QPointF b)
    {
        const auto delta = b-a;
        const auto length = QPointF::dotProduct(delta,delta);
        const qreal t = length > 0 ? std::clamp(QPointF::dotProduct(pixel-a,delta)/length,0.0,1.0) : 0.0;
        return {QLineF(pixel,a+delta*t).length(),t};
    }

    QList<ModelComponent> pickComponents(QPointF pixel,const std::optional<QRectF> &area,ModelDomain domain,
                                         const std::set<int> *excluded = nullptr,float *edgeFraction = nullptr)
    {
        const auto *asset = store_.selectedMesh();
        if (!asset)
            return {};
        const auto &mesh = *asset->mesh;
        pickIndex_.setMesh(asset->mesh);
        const auto matrix = camera(size());
        const auto inverse = matrix.inverted();
        if (domain == ModelDomain::Face && !area) {
            const auto hit = pickIndex_.hit(pickRay(inverse,pixel));
            return hit.face >= 0 ? QList<ModelComponent>{{hit.face}} : QList<ModelComponent>{};
        }
        QList<ModelComponent> result;
        qreal nearest = 8;
        float nearestDepth = std::numeric_limits<float>::infinity();
        const auto consider = [&](ModelComponent component,QVector3D position,float fraction = 0) {
            const auto screen = screenPoint(matrix,position);
            if (!screen || (area && !area->contains(*screen)))
                return;
            const qreal distance = area ? 0 : QLineF(pixel,*screen).length();
            if (distance > nearest)
                return;
            const auto ray = pickRay(inverse,*screen);
            const float depth = QVector3D::dotProduct(position-ray.origin,ray.direction);
            if (!xray_ && pickIndex_.hit(ray).distance + std::max(0.00002f,depth*0.0001f) < depth)
                return;
            if (area) {
                result.append(component);
            } else if (result.isEmpty() || distance < nearest-0.01
                       || (std::abs(distance-nearest) <= 0.01 && depth < nearestDepth)) {
                result = {component};
                nearest = distance;
                nearestDepth = depth;
                if (edgeFraction)
                    *edgeFraction = fraction;
            }
        };
        if (domain == ModelDomain::Vertex) {
            const auto considerVertex = [&](int vertex) {
                if (!excluded || !excluded->contains(vertex))
                    consider({vertex},point(mesh.vertices[vertex]));
            };
            if (!area) {
                std::set<int> vertices;
                for (const auto face : pickIndex_.facesNear(matrix,size(),QRectF(pixel-QPointF(8,8),QSizeF(16,16))))
                    for (const auto &corner : mesh.faces[face].corners)
                        vertices.insert(int(corner.vertex));
                vertices.insert(pickIndex_.looseVertices().begin(),pickIndex_.looseVertices().end());
                for (const int vertex : vertices)
                    considerVertex(vertex);
            } else {
                for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
                    considerVertex(int(i));
            }
        } else if (domain == ModelDomain::Edge) {
            const auto considerEdge = [&](ModelComponent edge) {
                const auto a = point(mesh.vertices[edge.index]), b = point(mesh.vertices[edge.end]);
                if (area) {
                    consider(edge,(a+b)*0.5f);
                    return;
                }
                const auto start = screenPoint(matrix,a), end = screenPoint(matrix,b);
                if (!start || !end)
                    return;
                const auto [distance,fraction] = distanceToSegment(pixel,*start,*end);
                if (distance > nearest)
                    return;
                const float wA = (matrix*QVector4D(a,1)).w(), wB = (matrix*QVector4D(b,1)).w();
                const float t = float(fraction)*wA/(float(1-fraction)*wB+float(fraction)*wA);
                consider(edge,a+(b-a)*t,t);
            };
            if (!area) {
                std::set<ModelComponent> edges;
                for (const auto face : pickIndex_.facesNear(matrix,size(),QRectF(pixel-QPointF(8,8),QSizeF(16,16)))) {
                    const auto &corners = mesh.faces[face].corners;
                    for (std::size_t i = 0; i < corners.size(); ++i) {
                        const auto [a,b] = std::minmax(corners[i].vertex,corners[(i+1)%corners.size()].vertex);
                        edges.insert({int(a),int(b)});
                    }
                }
                for (const auto edge : edges)
                    considerEdge(edge);
            } else {
                for (const auto edge : pickIndex_.edges())
                    considerEdge(edge);
            }
        } else {
            for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
                const auto &face = mesh.faces[i];
                QVector3D center;
                for (const auto &corner : face.corners)
                    center += point(mesh.vertices[corner.vertex]);
                consider({int(i)},center/float(face.corners.size()));
            }
        }
        return result;
    }

    std::optional<QVector3D> modelSnapTarget(QPointF pixel)
    {
        const auto *asset = store_.selectedMesh();
        if (!asset)
            return {};
        const auto excluded = modelVertices(*asset->mesh,domain_,selection_);
        const auto target = pickComponents(pixel,{},ModelDomain::Vertex,&excluded);
        return target.empty() ? std::nullopt : std::optional{point(asset->mesh->vertices[target.first().index])};
    }

    QSharedPointer<const wf::Mesh> referenceMesh()
    {
        const auto *active = store_.selectedObject();
        const auto *reference = store_.object(referenceId_);
        const auto *asset = reference ? store_.meshAsset(reference->meshAsset) : nullptr;
        if (!active || !asset || referenceId_ == active->id) {
            referenceSource_.reset();
            referenceMesh_.reset();
            referenceIndex_.setMesh({});
            return {};
        }
        bool invertible = false;
        const auto inverse = store_.worldMatrix(active->id).inverted(&invertible);
        if (!invertible)
            return {};
        const auto matrix = inverse*store_.worldMatrix(reference->id);
        const auto mesh = asset->evaluatedMesh ? asset->evaluatedMesh : asset->mesh;
        if (referenceSource_ == mesh && referenceMatrix_ == matrix)
            return referenceMesh_;
        auto transformed = QSharedPointer<wf::Mesh>::create(*mesh);
        bool first = true;
        for (auto &p : transformed->vertices) {
            const auto q = matrix.map(point(p));
            p = {q.x(),q.y(),q.z()};
            if (first) {
                referenceLow_ = referenceHigh_ = q;
                first = false;
            } else {
                for (int axis = 0; axis < 3; ++axis) {
                    referenceLow_[axis] = std::min(referenceLow_[axis],q[axis]);
                    referenceHigh_[axis] = std::max(referenceHigh_[axis],q[axis]);
                }
            }
        }
        referenceSource_ = mesh;
        referenceMatrix_ = matrix;
        referenceMesh_ = transformed;
        referenceIndex_.setMesh(referenceMesh_);
        return referenceMesh_;
    }

    std::optional<QVector3D> modelSurfaceTarget(QPointF pixel)
    {
        const auto ray = pickRay(camera(size()).inverted(),pixel);
        if (referenceMesh()) {
            const auto hit = referenceIndex_.hit(ray);
            return hit.face >= 0 ? std::optional{ray.origin+ray.direction*hit.distance} : std::nullopt;
        }
        const auto *asset = store_.selectedMesh();
        if (!asset)
            return {};
        pickIndex_.setMesh(asset->mesh);
        const auto moving = modelVertices(*asset->mesh,domain_,selection_);
        std::vector<bool> excluded(asset->mesh->faces.size());
        for (std::size_t i = 0; i < excluded.size(); ++i)
            excluded[i] = std::ranges::any_of(asset->mesh->faces[i].corners,
                [&](const auto &corner) { return moving.contains(int(corner.vertex)); });
        const auto hit = pickIndex_.hit(ray,&excluded);
        return hit.face >= 0 ? std::optional{ray.origin+ray.direction*hit.distance} : std::nullopt;
    }

    std::array<QVector3D,3> gizmoEnds() const
    {
        const auto &gizmo = *gizmo_;
        const float depth = ortho_ ? distance_ :
            std::max(distance_*0.05f,distance_-QVector3D::dotProduct(gizmo.pivot-target_,viewDirection()));
        const float length = depth*90.0f/std::max(height(),1);
        return {gizmo.pivot+gizmo.axes[0].normalized()*length,
                gizmo.pivot+gizmo.axes[1].normalized()*length,
                gizmo.pivot+gizmo.axes[2].normalized()*length};
    }

    std::vector<QVector3D> gizmoPath(int axis) const
    {
        const auto &gizmo = *gizmo_;
        const auto end = gizmoEnds()[axis];
        if (gizmo.kind != ModelHandle::Rotate)
            return {gizmo.pivot,end};
        const auto a = gizmo.axes[(axis+1)%3].normalized();
        const auto b = QVector3D::crossProduct(gizmo.axes[axis].normalized(),a).normalized();
        const auto radius = (end-gizmo.pivot).length()*0.83f;
        std::vector<QVector3D> circle;
        circle.reserve(49);
        for (int i = 0; i <= 48; ++i) {
            const auto angle = float(i)*0.130899694f;
            circle.push_back(gizmo.pivot+radius*(a*std::cos(angle)+b*std::sin(angle)));
        }
        return circle;
    }

    QVector3D modelDisplacement(QPointF previous,QPointF next)
    {
        const auto &gizmo = *gizmo_;
        const auto matrix = camera(size());
        const auto pivot = matrix*QVector4D(gizmo.pivot,1);
        if (pivot.w() <= 0)
            return {};
        if (gizmo.axis < 0) {
            const auto inverse = matrix.inverted();
            const auto depth = pivot.z()/pivot.w();
            return unproject(inverse,next,depth)-unproject(inverse,previous,depth);
        }
        const auto end = gizmoEnds()[gizmo.axis];
        const auto a = screenPoint(matrix,gizmo.pivot), b = screenPoint(matrix,end);
        if (!a || !b)
            return {};
        const auto screenAxis = *b-*a;
        const auto length = QPointF::dotProduct(screenAxis,screenAxis);
        return length > 16 ? (end-gizmo.pivot)*float(QPointF::dotProduct(next-previous,screenAxis)/length) : QVector3D{};
    }

    void drawModelGuides(const wf::Mesh &mesh)
    {
        const auto matrix = camera(size());
        const auto inverse = matrix.inverted();
        const auto line = [&](QVector3D a,QVector3D b,QVector3D color) {
            if (xray_)
                geometry_.overlay(a,b,color);
            else
                geometry_.line(a,b,color);
        };
        const QVector3D accent(0.9f,0.65f,1);
        if (pairGuide_)
            geometry_.overlay(unproject(inverse,pairGuide_->p1(),0.02f),
                unproject(inverse,pairGuide_->p2(),0.02f),{0.55f,1.0f,0.75f});
        const auto drawStroke = [this](const QList<QVector3D> &points,QVector3D color) {
            for (qsizetype i = 1; i < points.size(); ++i)
                geometry_.overlay(points[i-1],points[i],color);
        };
        const QVector3D ruler(0.28f,0.94f,1);
        drawStroke(measurement_,ruler);
        drawStroke(annotationStroke_,{1.0f,0.68f,0.24f});
        for (const auto &p : measurement_)
            if (const auto projected = screenPoint(matrix,p)) {
                const auto clip = matrix*QVector4D(p,1);
                for (const QPointF offset : {QPointF(5,0),QPointF(0,5)})
                    geometry_.overlay(unproject(inverse,*projected-offset,clip.z()/clip.w()),
                        unproject(inverse,*projected+offset,clip.z()/clip.w()),ruler);
            }
        if (!sculpt_)
            if (const auto *object = store_.selectedObject()) {
                const auto strokes = object->details.value("modelAnnotations").toArray();
                for (const auto &value : strokes) {
                    const auto stroke = value.toArray();
                    const auto pointAt = [&stroke](qsizetype index) {
                        const auto xyz = stroke[index].toArray();
                        return xyz.size() == 3
                            ? QVector3D(float(xyz.at(0).toDouble()),float(xyz.at(1).toDouble()),float(xyz.at(2).toDouble()))
                            : QVector3D{};
                    };
                    for (qsizetype i = 1; i < stroke.size(); ++i)
                        geometry_.overlay(pointAt(i-1),pointAt(i),{1.0f,0.68f,0.24f});
                }
            }
        if (domain_ == ModelDomain::Vertex) {
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                const auto position = point(mesh.vertices[i]);
                const auto center = screenPoint(matrix,position);
                if (!center)
                    continue;
                const auto projected = matrix*QVector4D(position,1);
                const float depth = std::max(0.0f,projected.z()/projected.w()-0.000005f);
                const bool selected = selection_.contains({int(i)});
                const qreal radius = selected ? 4 : 2.5;
                const auto color = selected ? accent : QVector3D(0.61f,0.66f,0.76f);
                for (const QPointF offset : {QPointF(radius,0),QPointF(0,radius)})
                    line(unproject(inverse,*center-offset,depth),unproject(inverse,*center+offset,depth),color);
            }
        } else if (domain_ == ModelDomain::Edge) {
            const auto drawEdge = [&](const auto &edge) {
                const bool selected = selection_.contains(edge);
                if ((selected || xray_) && std::size_t(edge.index) < mesh.vertices.size()
                    && std::size_t(edge.end) < mesh.vertices.size())
                    line(point(mesh.vertices[edge.index]),point(mesh.vertices[edge.end]),
                        selected ? accent : QVector3D(0.27f,0.30f,0.40f));
            };
            if (xray_) {
                for (const auto &edge : pickIndex_.edges())
                    drawEdge(edge);
            } else {
                for (const auto &edge : selection_)
                    drawEdge(edge);
            }
        } else if (domain_ == ModelDomain::Face && xray_) {
            for (const auto component : selection_) {
                if (component.index < 0 || std::size_t(component.index) >= mesh.faces.size())
                    continue;
                const auto &corners = mesh.faces[component.index].corners;
                for (std::size_t i = 0; i < corners.size(); ++i)
                    geometry_.overlay(point(mesh.vertices[corners[i].vertex]),
                        point(mesh.vertices[corners[(i+1)%corners.size()].vertex]),accent);
            }
        }
        if (gizmo_) {
            const QVector3D colors[]{{1,0.30f,0.32f},{0.40f,0.87f,0.35f},{0.35f,0.63f,1}};
            const auto screenLine = [&](QPointF a,QPointF b,QVector3D color) {
                geometry_.overlay(unproject(inverse,a,0.05f),unproject(inverse,b,0.05f),color);
            };
            for (int i = 0; i < 3; ++i) {
                const auto color = i == gizmo_->axis ? accent : colors[i];
                const auto path = gizmoPath(i);
                for (std::size_t j = 1; j < path.size(); ++j) {
                    const auto a = screenPoint(matrix,path[j-1]), b = screenPoint(matrix,path[j]);
                    if (!a || !b)
                        continue;
                    const auto d = *b-*a;
                    const auto length = std::hypot(d.x(),d.y());
                    if (length < 0.1)
                        continue;
                    const QPointF side(-d.y()/length,d.x()/length);
                    for (int width = -1; width <= 1; ++width)
                        screenLine(*a+side*width,*b+side*width,color);
                    if (gizmo_->kind == ModelHandle::Move) {
                        const auto back = *b-d*(12/length);
                        screenLine(*b,back+side*5,color);
                        screenLine(*b,back-side*5,color);
                        screenLine(back-side*5,back+side*5,color);
                    } else if (gizmo_->kind == ModelHandle::Scale) {
                        const QPointF corners[]{*b+QPointF(-5,-5),*b+QPointF(5,-5),*b+QPointF(5,5),*b+QPointF(-5,5)};
                        for (int k = 0; k < 4; ++k)
                            screenLine(corners[k],corners[(k+1)%4],color);
                    }
                }
            }
            if (const auto pivot = screenPoint(matrix,gizmo_->pivot)) {
                screenLine(*pivot-QPointF(5,0),*pivot+QPointF(5,0),accent);
                screenLine(*pivot-QPointF(0,5),*pivot+QPointF(0,5),accent);
            }
        }
        if (marquee_) {
            const QPointF corners[]{marquee_->topLeft(),marquee_->topRight(),marquee_->bottomRight(),marquee_->bottomLeft()};
            for (int i = 0; i < 4; ++i)
                geometry_.overlay(unproject(inverse,corners[i],0.1f),unproject(inverse,corners[(i+1)%4],0.1f),accent);
        }
        if (knifeStart_) {
            const auto center = screenPoint(matrix,*knifeStart_);
            if (center) {
                const auto projected = matrix*QVector4D(*knifeStart_,1);
                const float depth = projected.z()/projected.w();
                for (const QPointF offset : {QPointF(6,0),QPointF(0,6)})
                    geometry_.overlay(unproject(inverse,*center-offset,depth),unproject(inverse,*center+offset,depth),
                        {0.39f,1,0.77f});
            }
        }
        for (int i = 1; i < retopologyPoints_.size(); ++i)
            geometry_.overlay(retopologyPoints_[i-1],retopologyPoints_[i],{0.39f,1,0.77f});
        for (const auto &p : retopologyPoints_) {
            if (const auto center = screenPoint(matrix,p)) {
                const auto clip = matrix*QVector4D(p,1);
                const float depth = clip.z()/clip.w();
                for (const QPointF offset : {QPointF(5,0),QPointF(0,5)})
                    geometry_.overlay(unproject(inverse,*center-offset,depth),unproject(inverse,*center+offset,depth),
                        {0.39f,1,0.77f});
            }
        }
        if (const auto reference = referenceMesh())
            for (const auto &edge : referenceIndex_.edges())
                geometry_.line(point(reference->vertices[edge.index]),point(reference->vertices[edge.end]),{0.30f,0.58f,0.63f});
    }
    void rebuildWorldImages(const QList<SceneObject> &objects,const QMap<QString,MeshAsset> &assets)
    {
        worldImagesDirty_ = false;
        ++textureRevision_;
        worldImages_.clear();
        worldTexturePixels_.clear();
        std::size_t imageCount = 0, materialCount = 0;
        const auto collect = [&](const wf::Mesh *mesh) {
            if (!mesh || worldImages_.contains(mesh))
                return;
            worldImages_.insert(mesh,{});
            for (const auto &material : mesh->materials) {
                const auto maps = material.previewTextures();
                imageCount += std::ranges::count_if(maps,[](const auto *image) { return !image->empty(); });
                materialCount += material.hasAdvancedSurface();
            }
        };
        collect(placementPreviewMesh_.data());
        for (const auto &object : objects) {
            const auto asset = assets.constFind(object.meshAsset);
            if (!object.visible || asset == assets.cend())
                continue;
            collect(&asset->evaluated());
            collect(asset->lodMesh.data());
        }
        if (imageCount == 0 && materialCount == 0)
            return;
        const auto capacity = std::size_t(WorldLightingVulkan::worldTextureCapacity);
        const auto headers = 1+materialWords*materialCount;
        if (headers >= capacity || capacity-headers < imageCount)
            return;
        const auto side = int(std::sqrt((capacity-headers)/std::max<std::size_t>(imageCount,1)));
        worldTexturePixels_.resize(headers);
        std::size_t header = 1;
        for (auto it = worldImages_.begin(); it != worldImages_.end(); ++it) {
            auto &ranges = it.value();
            for (const auto &material : it.key()->materials) {
                if (!material.hasAdvancedSurface()) {
                    ranges.push_back(loadMaterialImage(worldTexturePixels_,material.baseColorTexture,side));
                    continue;
                }
                ranges.push_back({uint32_t(header),0,1});
                for (const auto *image : material.previewTextures()) {
                    const auto range = loadMaterialImage(worldTexturePixels_,*image,side);
                    std::ranges::copy(range,worldTexturePixels_.begin()+header);
                    header += range.size();
                }
                for (float value : material.emissive)
                    worldTexturePixels_[header++] = std::bit_cast<uint32_t>(value);
            }
        }
    }

    Geometry &geometry()
    {
        if (!dirty_)
            return geometry_;
        dirty_ = false;
        ++geometryRevision_;
        if (assetPreview_) {
            geometry_ = {};
            ++textureRevision_;
            geometry_.addMesh(SceneObject{},*assetPreview_,true,false,{},false,nullptr,0,false,false,false,true);
            return geometry_;
        }
        if (modelOnly_) {
            const auto *asset = store_.selectedMesh();
            const auto *object = store_.selectedObject();
            QSharedPointer<const wf::Mesh> mesh;
            if (asset) {
                mesh = asset->mesh;
                if (preview_)
                    mesh = preview_;
                else if (domain_ == ModelDomain::Object && !sculpt_) {
                    if (modelLodPreview_ && asset->lodMesh)
                        mesh = asset->lodMesh;
                    else if (asset->evaluatedMesh)
                        mesh = asset->evaluatedMesh;
                }
            }
            const bool trackFaces = domain_ == ModelDomain::Face;
            if (objectView_ || mesh != modelDrawMesh_ || trackFaces != modelTracksFaces_ || sculpt_ != modelDrawSculpt_) {
                geometry_ = {};
                ++textureRevision_;
                if (mesh && object) {
                    const bool wire = !sculpt_ && (modelShading_ == ModelShading::Wireframe || modelShading_ == ModelShading::ShadedWire);
                    const auto matrices = objectView_ ? store_.worldMatrices(objectPreview_ ? &*objectPreview_ : nullptr)
                        : QHash<QString,QMatrix4x4>{};
                    const auto matrix = matrices.value(object->id);
                    geometry_.addMesh(*object,*mesh,true,true,{},false,nullptr,0,trackFaces,wire,
                        modelShading_ == ModelShading::Solid,!sculpt_ && modelShading_ == ModelShading::Material,
                        objectView_ ? &matrix : nullptr,paintSlot_,paintPreview_);
                    if (objectView_ && store_.project()) {
                        for (const auto &part : store_.project()->objects) {
                            if (part.id == object->id || !part.visible)
                                continue;
                            const auto *parent = store_.object(part.parentId);
                            while (parent && parent->id != object->id)
                                parent = store_.object(parent->parentId);
                            const auto *childAsset = parent ? store_.meshAsset(part.meshAsset) : nullptr;
                            if (childAsset) {
                                const auto childMatrix = matrices.value(part.id);
                                geometry_.addMesh(part,childAsset->evaluated(),false,wire,{},false,nullptr,0,false,wire,
                                    modelShading_ == ModelShading::Solid,false,&childMatrix);
                            }
                        }
                    }
                }
                modelDrawMesh_ = mesh;
                modelTracksFaces_ = trackFaces;
                modelDrawSculpt_ = sculpt_;
                modelBaseLines_ = geometry_.lines.size();
            }
            geometry_.lines.resize(modelBaseLines_);
            geometry_.overlays.clear();
            if (mesh) {
                if (domain_ == ModelDomain::Edge)
                    pickIndex_.setMesh(asset->mesh);
                drawModelGuides(*mesh);
            }
            if (!sculpt_)
                drawGrid();
            return geometry_;
        }
        geometry_ = {};
        bool hasTerrain = false;
        geometry_.ambient = skyAmbient(atmosphere());
        hasLod_ = false;
        const auto eye = viewPose().first;
        if (const auto *project = store_.project()) {
            const auto &objects = runtime_ ? runtime_->objects() : project->objects;
            const auto matrices = runtime_ ? runtime_->renderMatrices() : store_.worldMatrices();
            const auto &assets = runtime_ ? runtime_->assets() : project->assets;
            if (worldImagesDirty_)
                rebuildWorldImages(objects,assets);
            const auto lights = sceneLights(objects,atmosphere());
            for (auto object : objects) {
                const bool selected = !runtime_ && !placementPreviewMesh_ && object.id == store_.selection();
                if (!object.visible)
                    continue;
                if (const auto moved = runtimePosition(object.id))
                    object.position = *moved;
                if (runtime_)
                    if (const auto rotated = runtime_->rotation(object.id))
                        object.rotation = *rotated;
                if (object.type == "Terrain") {
                    hasTerrain = true;
                    object.rotation = {};
                    object.scale = {1,1,1};
                    auto &terrain = terrains_[object.id];
                    if (terrain.details != object.details) {
                        terrain.update(readHeightfield(object),object.details);
                    }
                    const auto [low,high] = std::ranges::minmax(terrain.field.samples);
                    const float half = terrain.field.size*0.5f;
                    geometry_.extendBounds(object.position+QVector3D(-half,-half,low));
                    geometry_.extendBounds(object.position+QVector3D(half,half,high));
                    const auto focus = playerPosition().value_or(target_)-object.position;
                    const float radius = playerPosition() ? 120.0f : std::max(24.0f,distance_*0.85f);
                    constexpr int side = wf::Heightfield::chunks;
                    for (int y = 0; y < side; ++y)
                        for (int x = 0; x < side; ++x) {
                            auto &chunk = terrain.chunks[y*side+x];
                            if (!terrain.field.chunkInRange(x,y,focus.x(),focus.y(),radius)) {
                                chunk.reset();
                                continue;
                            }
                            if (!chunk)
                                chunk.emplace(TerrainChunk{terrain.field.chunk(x,y),terrain.field.foliage(x,y,
                                    object.details.value("foliageDensity").toInt(),object.details.value("foliageSeed").toInt(42))});
                            geometry_.addMesh(object,chunk->mesh,false,false,lights,true,&terrain.field);
                            geometry_.addMesh(object,chunk->foliage,false,false,lights,false,nullptr,
                                float(std::clamp(object.details.value("foliageWind").toDouble(0.09),0.0,0.5)));
                        }
                } else if (const auto asset = assets.constFind(object.meshAsset); asset != assets.cend()) {
                    const auto matrix = matrices.value(object.id);
                    object.position = matrix.map(QVector3D{});
                    hasLod_ |= bool(asset->lodMesh);
                    const auto &mesh = placementPreviewMesh_ && object.meshAsset == placementPreview_.meshAsset
                        ? *placementPreviewMesh_
                        : asset->lodMesh && (object.position-eye).lengthSquared() >= asset->lodDistance*asset->lodDistance
                            ? *asset->lodMesh : asset->evaluated();
                    const auto images = worldImages_.constFind(&mesh);
                    geometry_.addMesh(object,mesh,false,selected,lights,true,nullptr,0,false,false,false,false,&matrix,
                        -1,{},images == worldImages_.cend() ? std::span<const ImageRange>{} : std::span<const ImageRange>{*images},
                        selected ? &eye : nullptr,runtime_ ? asset->clip.data() : nullptr,runtime_ ? runtime_->clipTime() : 0);
                }
            }
            if (placementPreviewMesh_) {
                const auto &ranges = worldImages_[placementPreviewMesh_.data()];
                geometry_.addMesh(placementPreview_,*placementPreviewMesh_,false,true,lights,false,nullptr,0,
                    false,false,false,false,nullptr,-1,{},ranges,&eye);
            }
            if (runtime_)
                for (const auto *beacons : {&runtime_->gameplay().beacons,&runtime_->gameplay().behaviorBeacons})
                    for (const auto &beacon : *beacons)
                        if (beacon.remaining > 0)
                            geometry_.addMesh(SceneObject{},wf::makeBeaconMesh(beacon),false,false,lights,false);
            if (runtime_)
                for (const auto &patrol : runtime_->gameplay().patrols)
                    geometry_.addMesh(SceneObject{},wf::makePatrolMesh(patrol),false,false,lights,false);
            if (const auto player = playerPosition()) {
                SceneObject avatar;
                avatar.position = *player;
                avatar.scale = {0.6f,0.6f,1.7f};
                static const auto mesh = [] {
                    auto cube = wf::makeCube();
                    cube.materials[0].color = {0.91f,0.54f,0.19f,1};
                    return cube;
                }();
                geometry_.addMesh(avatar,mesh,false,false,lights);
            }
        }
        if (hasTerrain || playerPosition())
            return geometry_;
        drawGrid();
        return geometry_;
    }
    void drawGrid()
    {
        const float z = geometry_.hasBounds ? geometry_.minimum.z() - 0.003f : 0;
        for (int i = -10; i <= 10; ++i) {
            const auto v = float(i);
            geometry_.line({v,-10,z}, {v,10,z}, i ? QVector3D(0.20f,0.23f,0.29f) : QVector3D(0.30f,0.55f,0.32f));
            geometry_.line({-10,v,z}, {10,v,z}, i ? QVector3D(0.20f,0.23f,0.29f) : QVector3D(0.64f,0.28f,0.32f));
        }
    }
    ProjectStore &store_;
    bool modelOnly_;
    bool hasLod_ = false, modelLodPreview_ = false;
    LiveViewport &owner_;
    bool dirty_ = true;
    quint64 geometryRevision_ = 0;
    bool framed_ = false;
    bool rendered_ = false;
    // False while a still view is still averaging its TAA samples; captureFrame waits for it.
    bool temporalSettled_ = true;
    ModelDomain domain_ = ModelDomain::Object;
    std::set<ModelComponent> selection_;
    bool xray_ = false, ortho_ = false;
    QSharedPointer<const wf::Mesh> preview_;
    QSharedPointer<const wf::Mesh> assetPreview_;
    std::optional<std::pair<QVector3D,QVector3D>> cameraPreview_;
    std::optional<ModelGizmo> gizmo_;
    ModelShading modelShading_ = ModelShading::ShadedWire;
    uint64_t textureRevision_ = 0;
    bool worldImagesDirty_ = true;
    SceneObject placementPreview_;
    QSharedPointer<const wf::Mesh> placementPreviewMesh_;
    QHash<const wf::Mesh *,std::vector<ImageRange>> worldImages_;
    std::vector<uint32_t> worldTexturePixels_;
    bool handleDrag_ = false;
    struct RotationDrag { QMatrix4x4 inverse; QVector3D pivot, normal; };
    std::optional<RotationDrag> rotationDrag_;
    bool surfaceTool_ = false;
    bool pairPicking_ = false;
    std::optional<QLineF> pairGuide_;
    QList<QVector3D> measurement_, annotationStroke_;
    std::optional<QPointF> modelPress_;
    std::optional<QRectF> marquee_;
    ModelPickIndex pickIndex_;
    QString referenceId_;
    bool knife_ = false;
    std::optional<QVector3D> knifeStart_;
    bool retopology_ = false;
    QList<QVector3D> retopologyPoints_;
    QMatrix4x4 referenceMatrix_;
    QSharedPointer<const wf::Mesh> referenceSource_, referenceMesh_;
    QVector3D referenceLow_, referenceHigh_;
    ModelPickIndex referenceIndex_;
    Geometry geometry_;
    QSharedPointer<const wf::Mesh> modelDrawMesh_;
    QImage paintPreview_;
    int paintSlot_ = -1;
    std::size_t modelBaseLines_ = 0;
    bool modelTracksFaces_ = false;
    bool objectView_ = false;
    std::optional<SceneObject> objectPreview_;
    bool sculpt_ = false, sculptDown_ = false, modelDrawSculpt_ = false;
    QPointF sculptPixel_;
    struct TerrainChunk { wf::Mesh mesh, foliage; };
    struct TerrainCache {
        QJsonObject details;
        wf::Heightfield field;
        std::array<std::optional<TerrainChunk>, wf::Heightfield::chunks*wf::Heightfield::chunks> chunks;

        void update(const wf::Heightfield &next,const QJsonObject &settings)
        {
            constexpr int side = wf::Heightfield::chunks;
            constexpr int cells = (wf::Heightfield::side-1)/side;
            const bool settingsChanged = field.size != next.size
                || details.value("foliageDensity") != settings.value("foliageDensity")
                || details.value("foliageSeed") != settings.value("foliageSeed");
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x) {
                    auto &chunk = chunks[y*side+x];
                    if (!chunk)
                        continue;
                    bool changed = settingsChanged;
                    // Include the adjacent sample used to compute each edge normal.
                    for (int row = std::max(0,y*cells-1); !changed && row <= std::min(wf::Heightfield::side-1,(y+1)*cells+1); ++row) {
                        const int start = row*wf::Heightfield::side + std::max(0,x*cells-1);
                        const int stop = row*wf::Heightfield::side + std::min(wf::Heightfield::side,(x+1)*cells+2);
                        changed = !std::equal(next.samples.begin()+start,next.samples.begin()+stop,field.samples.begin()+start);
                    }
                    if (changed)
                        chunk.reset();
                }
            details = settings;
            field = next;
        }
    };
    QHash<QString, TerrainCache> terrains_;
    QVector3D target_;
    float distance_ = 5;
    float yaw_ = -0.85f;
    float pitch_ = 0.57f;
    QPointF previous_;
    RunMode mode_ = RunMode::Editing;
    std::optional<WorldRuntime> runtime_;
    QTimer timer_;
    QElapsedTimer windClock_;
    QElapsedTimer clock_;
    double elapsed_ = 0;
    QVector3D savedTarget_;
    float savedDistance_ = 0, savedYaw_ = 0, savedPitch_ = 0;
    float brushRadius_ = 0, brushStrength_ = 0;
    bool brushSmooth_ = false;
    int brushLayer_ = -1;
};

class VulkanSceneRenderer : public QVulkanWindowRenderer {
public:
    explicit VulkanSceneRenderer(VulkanSceneWindow &window) : window_(window) {}

    void initResources() override
    {
        device_ = window_.device();
        vk_ = window_.vulkanInstance()->deviceFunctions(device_);
#ifdef Q_OS_WIN
        const auto instance = window_.vulkanInstance();
        const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            instance->getInstanceProcAddr("vkGetPhysicalDeviceProperties2"));
        if (properties2) {
            VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
            properties2(window_.physicalDevice(), &properties);
            if (identity.deviceLUIDValid) {
                LUID luid{};
                static_assert(sizeof(luid) == VK_LUID_SIZE);
                std::memcpy(&luid, identity.deviceLUID, sizeof(luid));
                const auto counter = QString("luid_0x%1_0x%2").arg(quint32(luid.HighPart), 8, 16, QLatin1Char('0'))
                    .arg(quint32(luid.LowPart), 8, 16, QLatin1Char('0'));
                Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))
                    && SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.GetAddressOf()))))
                    adapter->GetDesc1(&desc);
                const auto dedicated = properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                    ? desc.DedicatedVideoMemory : 0;
                window_.owner_.setRenderDevice(QString::fromUtf8(properties.properties.deviceName), counter,
                                                dedicated, desc.SharedSystemMemory);
            }
        }
#endif
        if (window_.owner_.rendererName().isEmpty())
            window_.owner_.setRenderDevice(QString::fromUtf8(window_.physicalDeviceProperties()->deviceName), {}, 0, 0);
        frames_.resize(window_.concurrentFrameCount());
        skyValid_ = skyInitialized_ = false;
        pointShadowsInitialized_ = false;
        if (!lighting_.create(window_,vk_,window_.modelOnly_)) {
            window_.report(QObject::tr("Vulkan could not allocate shadow resources."));
            return;
        }
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkPushConstantRange camera{shaderStages, 0, sizeof(ViewUniform)};
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &camera;
        const auto descriptors = lighting_.layout();
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &descriptors;
        if (vk_->vkCreatePipelineLayout(device_, &layout, nullptr, &layout_) != VK_SUCCESS)
            window_.report(QObject::tr("Vulkan pipeline layout creation failed."));
        if (!frame_.create(window_))
            window_.report(QObject::tr("Vulkan could not create the HDR scene pass."));
    }
    void initSwapChainResources() override
    {
        window_.dirty_ = true;
        if (layout_ && frame_.scenePass() && frame_.createTargets()) {
            const auto spirv = [](const QString &path) {
                QFile file(path);
                return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            };
            frame_.createDisplayPipeline(spirv(":/shaders/Display.vert.spv"),spirv(":/shaders/Display.frag.spv"),
                                         spirv(":/shaders/Temporal.frag.spv"),spirv(":/shaders/Ambient.frag.spv"),
                                         spirv(":/shaders/BloomDown.frag.spv"),spirv(":/shaders/BloomUp.frag.spv"),
                                         spirv(":/shaders/DepthOfField.frag.spv"),spirv(":/shaders/MotionBlur.frag.spv"));
            triangles_ = pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true);
            lines_ = pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false);
            if (window_.modelOnly_)
                overlays_ = pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST,false,Pass::Overlay);
            else
                outline_ = pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,false,Pass::SelectionOutline);
            sky_ = pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,false,Pass::Sky);
            shadow_ = pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,true,Pass::Shadow);
        }
        if (!triangles_ || !lines_ || !sky_ || !shadow_ || !(window_.modelOnly_ ? overlays_ : outline_) || !frame_.displayReady())
            window_.report(QObject::tr("Vulkan shader or pipeline creation failed."));
    }
    void releaseSwapChainResources() override
    {
        if (triangles_)
            vk_->vkDestroyPipeline(device_, triangles_, nullptr);
        if (lines_)
            vk_->vkDestroyPipeline(device_, lines_, nullptr);
        if (overlays_)
            vk_->vkDestroyPipeline(device_, overlays_, nullptr);
        if (outline_)
            vk_->vkDestroyPipeline(device_, outline_, nullptr);
        if (sky_)
            vk_->vkDestroyPipeline(device_, sky_, nullptr);
        if (shadow_)
            vk_->vkDestroyPipeline(device_,shadow_,nullptr);
        triangles_ = lines_ = overlays_ = outline_ = sky_ = shadow_ = VK_NULL_HANDLE;
        frame_.destroyDisplayPipeline();
        frame_.releaseTargets();
    }
    void releaseResources() override
    {
        for (auto &frame : frames_)
            destroy(frame);
        frames_.clear();
        if (layout_)
            vk_->vkDestroyPipelineLayout(device_, layout_, nullptr);
        lighting_.destroy();
        frame_.destroy();
        layout_ = VK_NULL_HANDLE;
        window_.rendered_ = false;
    }
    // TAA for this frame. While editing, any change to the camera, scene or textures restarts the average, so a
    // still view converges to an exact supersampled image. In Play and Simulate, the camera and objects keep
    // moving, so the history is reprojected with the camera and clamped instead.
    ViewportFrameVulkan::Temporal temporalFor(const WorldAtmosphere &environment,const Geometry &geometry,const QMatrix4x4 &camera,
                                              QSize size)
    {
        ViewportFrameVulkan::Temporal temporal;
        temporal.enabled = environment.temporalAntiAliasing;
        const bool playing = window_.runtime_.has_value();
        const bool changed = !previousValid_ || camera != previousCamera_ || size != previousSize_
            || window_.geometryRevision_ != previousGeometry_ || window_.textureRevision_ != previousTextures_;
        if (temporal.enabled && !playing && changed)
            frame_.resetHistory();
        temporal.still = !playing && !geometry.animated;
        const auto reprojection = (previousValid_ ? previousCamera_ : camera)*camera.inverted();
        std::copy_n(reprojection.constData(),16,temporal.reprojection.begin());
        previousCamera_ = camera;
        previousSize_ = size;
        previousGeometry_ = window_.geometryRevision_;
        previousTextures_ = window_.textureRevision_;
        previousValid_ = true;
        return temporal;
    }

    // The exposure in stops for this frame. With auto exposure, reads the histogram this frame slot metered last
    // time and moves toward its target, asking for more frames until the exposure settles.
    float exposureFor(const WorldAtmosphere &environment,uint32_t frameIndex)
    {
        namespace color = worldforge::color;
        if (!environment.autoExposure) {
            adaptedStops_ = std::numeric_limits<float>::quiet_NaN();
            adaptClock_.invalidate();
            return environment.exposure;
        }
        const float seconds = adaptClock_.isValid() ? float(adaptClock_.restart())*0.001f : 0.0f;
        if (!adaptClock_.isValid())
            adaptClock_.start();
        float average = 0;
        if (frame_.takeMeter(frameIndex,average)) {
            const float target = color::autoExposureStops(average,environment.exposureCompensation);
            adaptedStops_ = std::isnan(adaptedStops_) ? target : color::adaptExposure(adaptedStops_,target,std::min(seconds,0.25f));
            if (std::abs(target-adaptedStops_) > 0.01f)
                window_.requestUpdate();
        } else if (std::isnan(adaptedStops_)) {
            window_.requestUpdate();
        }
        return std::isnan(adaptedStops_) ? environment.exposure : adaptedStops_;
    }

    void startNextFrame() override
    {
        auto &geometry = window_.geometry();
        if (!window_.framed_ && geometry.hasBounds)
            window_.frame();
        const auto size = window_.swapChainImageSize();
        const auto command = window_.currentCommandBuffer();
        const VkExtent2D extent{uint32_t(size.width()), uint32_t(size.height())};
        const VkRect2D area{{0,0}, extent};
        // Attachment 0 is the swap chain image, 1 the linear HDR scene, 2 the scene depth.
        VkClearValue clear[3]{};
        const auto background = wf::color::srgbToLinear({0.045f,0.059f,0.080f});
        clear[1].color = {{background[0],background[1],background[2],1.0f}};
        clear[2].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderArea = area;
        if (!frame_.displayReady()) {
            // Without the HDR pass, clear the window so the swap chain image is still presentable.
            pass.renderPass = window_.defaultRenderPass();
            pass.framebuffer = window_.currentFramebuffer();
            clear[0].color = {{0.045f,0.059f,0.080f,1.0f}};
            clear[1].depthStencil = {1.0f, 0};
            pass.clearValueCount = 2;
            pass.pClearValues = clear;
            vk_->vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);
            vk_->vkCmdEndRenderPass(command);
            window_.frameReady();
            return;
        }
        auto &frame = frames_.at(window_.currentFrame());
        const bool hasGeometry = !geometry.triangles.empty() || !geometry.lines.empty()
            || !geometry.overlays.empty() || !geometry.selectionOutline.empty();
        const auto environment = window_.atmosphere();
        std::vector<Light> lights{Light{sunDirection(environment),1,true}};
        if (const auto *project = window_.store_.project())
            lights = sceneLights(window_.runtime_ ? window_.runtime_->objects() : project->objects,environment);
        const Light sunlight = lights.front();
        const auto sun = sunlight.vector;
        uint64_t skyRevision = 0;
        const auto &skyLight = cachedSkyLight(environment,sunlight,skyRevision);
        // The physical sky's sun reaches the ground dimmer and warmer than overhead (sunLight); the gradient sky's
        // sun is as authored.
        const auto &sunLight = skyLight.sunLight;
        const float sunStrength = 0.2126f*sunLight[0]+0.7152f*sunLight[1]+0.0722f*sunLight[2];
        const float windTime = float(window_.windClock_.elapsed()%1000000)*0.001f;
        const bool shadows = environment.shadows && sunStrength > 0 && !geometry.triangles.empty();
        LightingUniform lighting{};
        lighting.fogCenterRadius[0] = environment.localFogCenter.x();
        lighting.fogCenterRadius[1] = environment.localFogCenter.y();
        lighting.fogCenterRadius[2] = environment.localFogCenter.z();
        lighting.fogCenterRadius[3] = environment.localFogRadius;
        lighting.settings[0] = environment.localFogDensity;
        lighting.settings[1] = shadows ? 1.0f : 0.0f;
        lighting.settings[2] = float(frame_.sampleIndex()); // turns the shadow filter pattern each TAA frame
        lighting.settings[3] = window_.modelOnly_ ? 1.0f : skyAmbient(environment);
        // The shading model (Shading.glsl): the sun, the sky light and up to 16 other lights.
        const auto towardSun = sunlight.vector.normalized();
        const float sunValues[4]{towardSun.x(),towardSun.y(),towardSun.z(),sunStrength};
        std::copy_n(sunValues,4,lighting.sun);
        for (int c = 0; c < 3; ++c)
            lighting.sunTint[c] = sunStrength > 0 ? sunLight[std::size_t(c)]/sunStrength : 1.0f;
        lighting.skySettings[2] = environment.physicalSky ? 1.0f : 0.0f;
        // Screen-space reflections trace the previous frame, seen through the previous steady camera.
        // Orthographic views have no perspective depth to trace with.
        const bool reflections = environment.reflections && !window_.modelOnly_ && !window_.ortho_ && previousValid_
            && frame_.previousFrameReady();
        std::copy_n((reflections ? previousCamera_ : QMatrix4x4()).constData(),16,lighting.previousViewProjection);
        lighting.reflectionSettings[0] = reflections ? 1.0f : 0.0f;
        lighting.reflectionSettings[1] = wf::vk::HdrFrame::reflectionRange;
        for (std::size_t i = 0; i < 9; ++i)
            std::copy_n(skyLight.irradiance[i].data(),4,lighting.skyIrradiance[i]);
        std::copy_n(skyLight.horizon.data(),3,lighting.skyHorizon);
        std::copy_n(skyLight.zenith.data(),3,lighting.skyZenith);
        std::copy_n(skyLight.ground.data(),3,lighting.skyGround);
        // Clustered lights (LightClusters.h): every light but the sun, binned by the clusters of the steady camera.
        std::vector<wf::lights::Light> clusterLights;
        for (std::size_t i = 1; i < lights.size(); ++i)
            if (lights[i].intensity > 0)
                clusterLights.push_back({{lights[i].vector.x(),lights[i].vector.y(),lights[i].vector.z()},lights[i].intensity,
                                         {1,1,1},lights[i].directional,
                                         {lights[i].spotDirection.x(),lights[i].spotDirection.y(),lights[i].spotDirection.z()},
                                         lights[i].spotAngle,lights[i].spotBlend});
        const auto steady = window_.camera(size);
        const auto toWorldMatrix = steady.inverted();
        const auto eyePoint = toWorldMatrix.map(QVector4D(0,0,0,1)), farPoint = toWorldMatrix.map(QVector4D(0,0,1,1));
        const auto forward = (farPoint.toVector3DAffine()-eyePoint.toVector3DAffine()).normalized();
        std::array<float,16> clusterMatrix;
        std::copy_n(steady.constData(),16,clusterMatrix.begin());
        const auto viewer = window_.viewPose().first;
        const auto lightClusters = wf::lights::build(clusterLights,clusterMatrix,{viewer.x(),viewer.y(),viewer.z()},
                                                     {forward.x(),forward.y(),forward.z()});
        const float clusterForward[4]{forward.x(),forward.y(),forward.z(),0};
        const float clusterViewport[4]{float(size.width()),float(size.height()),0,0};
        std::copy_n(clusterForward,4,lighting.clusterForward);
        std::copy_n(clusterViewport,4,lighting.clusterViewport);
        const auto cascades = shadowCascades(geometry,sun,window_.camera(size));
        for (int i = 0; i < wf::shadow::cascadeCount; ++i) {
            std::copy_n(cascades.matrices[i].data(),16,lighting.cascadeMatrices[i]);
            std::copy_n(cascades.scales[i].data(),4,lighting.cascadeScales[i]);
        }
        // Large-scale sky occlusion, part of the ambient occlusion setting; world views only.
        const bool skyMaps = environment.ambientOcclusion && shadowBounds_ && !window_.modelOnly_ && !geometry.triangles.empty();
        auto skyVisibility = shadowBounds_ ? wf::sky::fit(*shadowBounds_) : wf::sky::Visibility{};
        // The maps are shared by all frames and re-rendered only when needed: at once after an edit or a refit, and
        // at most every skyRefreshFrames frames while a running game moves objects.
        bool skyRefresh = false;
        if (skyMaps) {
            const auto &rendered = skyRendered_;
            const float moved = std::hypot(skyVisibility.center[0]-rendered.center[0],skyVisibility.center[1]-rendered.center[1],
                                           skyVisibility.center[2]-rendered.center[2]);
            const bool refit = !skyValid_ || std::abs(skyVisibility.radius-rendered.radius) > 0.1f*rendered.radius
                || moved > 8*rendered.metersPerTexel;
            const bool changed = skyRevision_ != window_.geometryRevision_;
            ++skyAge_;
            skyRefresh = refit || (changed && (!window_.runtime_ || skyAge_ >= skyRefreshFrames));
            if (skyRefresh) {
                skyRendered_ = skyVisibility;
                skyValid_ = true;
                skyRevision_ = window_.geometryRevision_;
                skyAge_ = 0;
            } else {
                skyVisibility = skyRendered_;
            }
        }
        for (int i = 0; i < wf::sky::directionCount; ++i) {
            std::copy_n(skyVisibility.matrices[i].data(),16,lighting.skyMatrices[i]);
            std::copy_n(skyVisibility.directions[i].data(),4,lighting.skyDirections[i]);
            // w: the sky's luminance in that direction, which weights it in skyOpenness.
            lighting.skyDirections[i][3] = wf::color::skyLuminance(skyLight,skyVisibility.directions[i].data());
        }
        lighting.skySettings[0] = skyMaps ? 1.0f : 0.0f;
        lighting.skySettings[1] = skyVisibility.metersPerTexel;
        const auto frameIndex = window_.currentFrame();
        const auto exposure = exposureFor(environment,frameIndex);
        const bool ready = triangles_ && lines_ && sky_ && shadow_
            && (!hasGeometry || upload(frame,geometry,window_.geometryRevision_)) && lighting_.upload(frameIndex,lighting)
            && lighting_.uploadTextures(frameIndex,window_.modelOnly_ ? geometry.texturePixels : window_.worldTexturePixels_,
                window_.textureRevision_)
            && lighting_.uploadEnvironment(frameIndex,skyLight.environment,skyRevision)
            && lighting_.uploadClusters(frameIndex,lightClusters);
        if (ready) {
            // Before anything binds this frame's set: the previous frame's TAA result alternates between two images.
            lighting_.bindPreviousFrame(frameIndex,frame_.nearestSampler(),frame_.previousFrame());
            recordDepthTiles(command,frame,lighting_.framebuffer(frameIndex),WorldLightingVulkan::shadowSize,WorldLightingVulkan::shadowSize,
                             wf::shadow::tileSize,
                             shadows,geometry.triangles.size(),cascades.matrices,windTime);
            if (skyRefresh || !skyInitialized_) {
                recordDepthTiles(command,frame,lighting_.skyFramebuffer(),wf::sky::atlasSize,wf::sky::atlasSize,wf::sky::tileSize,
                                 skyRefresh,geometry.triangles.size(),skyVisibility.matrices,windTime);
                skyInitialized_ = true;
            }
            // Point light shadow cubes (LightClusters.h): at once after an edit or when the shadowed lights change, and at
            // most every skyRefreshFrames frames while a running game moves objects.
            const bool pointChanged = lightClusters.shadowed != pointShadowsRendered_
                || (pointShadowRevision_ != window_.geometryRevision_ && (!window_.runtime_ || ++pointShadowAge_ >= skyRefreshFrames));
            if (!pointShadowsInitialized_ || (!lightClusters.shadowed.empty() && pointChanged)) {
                recordDepthTiles(command,frame,lighting_.pointShadowFramebuffer(),wf::lights::shadowAtlasWidth,
                                 wf::lights::shadowAtlasHeight,wf::lights::faceSize,!lightClusters.shadowed.empty(),
                                 geometry.triangles.size(),std::span(lightClusters.shadowFaces).first(lightClusters.shadowed.size()*6),
                                 windTime);
                pointShadowsInitialized_ = true;
                pointShadowsRendered_ = lightClusters.shadowed;
                pointShadowRevision_ = window_.geometryRevision_;
                pointShadowAge_ = 0;
            }
        } else {
            skyValid_ = false; // nothing was drawn; render the maps on the next ready frame
        }
        frame_.begin(command,window_.currentSwapChainImageIndex(),background.data());
        if (ready) {
            const VkViewport viewport{0, 0, float(size.width()), float(size.height()), 0, 1};
            vk_->vkCmdSetViewport(command, 0, 1, &viewport);
            vk_->vkCmdSetScissor(command, 0, 1, &area);
            const auto camera = window_.camera(size);
            const auto temporal = temporalFor(environment,geometry,camera,size);
            // The scene renders with this frame's TAA jitter; editor overlays use the steady camera.
            const auto subpixel = temporal.enabled ? frame_.jitter() : std::array<float,2>{};
            QMatrix4x4 jitter;
            jitter.translate(2*subpixel[0]/float(size.width()),2*subpixel[1]/float(size.height()),0);
            const auto sceneCamera = jitter*camera;
            const auto descriptors = lighting_.descriptor(frameIndex);
            vk_->vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&descriptors,0,nullptr);
            const auto eye = window_.viewPose().first;
            ViewUniform uniforms{};
            const auto writeColor = [](float (&dest)[4], const QColor &source, float alpha) {
                dest[0] = source.redF();
                dest[1] = source.greenF();
                dest[2] = source.blueF();
                dest[3] = alpha;
            };
            uniforms.eye[0] = eye.x();
            uniforms.eye[1] = eye.y();
            uniforms.eye[2] = eye.z();
            writeColor(uniforms.fog,environment.fogColor,environment.fogDensity);
            // Mesh shading uses the sky color slots for the sun's direction and intensity.
            uniforms.horizon[0] = sun.x();
            uniforms.horizon[1] = sun.y();
            uniforms.horizon[2] = sun.z();
            uniforms.horizon[3] = environment.fogFalloff;
            uniforms.zenith[0] = sunStrength;
            // World views shade every surface with the shared physically based model; modeling views keep their studio light.
            uniforms.zenith[1] = window_.modelOnly_ ? 0.0f : 1.0f;
            uniforms.zenith[3] = environment.fogHeight;
            uniforms.eye[3] = windTime;
            std::copy_n(sceneCamera.constData(),16,uniforms.matrix);
            vk_->vkCmdPushConstants(command, layout_, shaderStages, 0, sizeof(uniforms), &uniforms);
            const VkDeviceSize offset = 0;
            if (hasGeometry)
                vk_->vkCmdBindVertexBuffers(command, 0, 1, &frame.buffer, &offset);
            if (!geometry.triangles.empty() && (!window_.modelOnly_ || window_.modelShading_ != ModelShading::Wireframe)) {
                vk_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, triangles_);
                vk_->vkCmdDraw(command, uint32_t(geometry.triangles.size()), 1, 0, 0);
            }
            const auto toWorld = sceneCamera.inverted();
            // Draw the sky at the far plane to skip fog shading behind opaque surfaces.
            if (environment.sky) {
                auto skyUniforms = uniforms;
                writeColor(skyUniforms.horizon,environment.horizon,environment.fogFalloff);
                writeColor(skyUniforms.zenith,environment.zenith,environment.fogHeight);
                std::copy_n(toWorld.constData(),16,skyUniforms.matrix);
                vk_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_);
                vk_->vkCmdPushConstants(command, layout_, shaderStages, 0, sizeof(skyUniforms), &skyUniforms);
                vk_->vkCmdDraw(command,3,1,0,0);
            }
            // Display subpass: tone map the HDR scene, then draw editor lines in display colors.
            ViewportFrameVulkan::Occlusion occlusion;
            occlusion.enabled = environment.ambientOcclusion;
            std::copy_n(toWorld.constData(),16,occlusion.inverseViewProjection.begin());
            frame_.display(command,exposure,environment.viewTransform,environment.autoExposure ? int(frameIndex) : -1,temporal,
                           occlusion,environment.bloom && !window_.modelOnly_ ? wf::color::bloomStrength : 0.0f,
                           cameraEffects(environment,toWorldMatrix,viewer,forward));
            window_.temporalSettled_ = !temporal.enabled || !temporal.still || frame_.accumulated() >= wf::vk::HdrFrame::temporalSamples;
            if (!window_.temporalSettled_)
                window_.requestUpdate();
            std::copy_n(camera.constData(),16,uniforms.matrix);
            vk_->vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&descriptors,0,nullptr);
            vk_->vkCmdPushConstants(command, layout_, shaderStages, 0, sizeof(uniforms), &uniforms);
            if (!geometry.lines.empty()) {
                vk_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, lines_);
                vk_->vkCmdDraw(command, uint32_t(geometry.lines.size()), 1, uint32_t(geometry.triangles.size()), 0);
            }
            if (overlays_ && !geometry.overlays.empty()) {
                vk_->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,overlays_);
                vk_->vkCmdDraw(command,uint32_t(geometry.overlays.size()),1,
                    uint32_t(geometry.triangles.size()+geometry.lines.size()),0);
            }
            if (outline_ && !geometry.selectionOutline.empty()) {
                // The outline pass uses the eye slot for pixel scale and stroke width.
                uniforms.eye[0] = 2.0f/size.width();
                uniforms.eye[1] = 2.0f/size.height();
                uniforms.eye[2] = 2.0f*float(window_.devicePixelRatio());
                vk_->vkCmdPushConstants(command,layout_,shaderStages,0,sizeof(uniforms),&uniforms);
                vk_->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,outline_);
                vk_->vkCmdDraw(command,uint32_t(geometry.selectionOutline.size()),1,
                    uint32_t(geometry.triangles.size()+geometry.lines.size()+geometry.overlays.size()),0);
            }
        } else {
            const VkViewport viewport{0, 0, float(size.width()), float(size.height()), 0, 1};
            vk_->vkCmdSetViewport(command, 0, 1, &viewport);
            vk_->vkCmdSetScissor(command, 0, 1, &area);
            frame_.display(command,exposure,environment.viewTransform);
        }
        vk_->vkCmdEndRenderPass(command);
        if (ready && environment.autoExposure)
            frame_.finishMeter(command);
        if (ready && !window_.rendered_) {
            window_.rendered_ = true;
            window_.report(QObject::tr("Vulkan · %1 · RMB orbit · MMB pan · Wheel zoom · F frame")
                .arg(QString::fromUtf8(window_.physicalDeviceProperties()->deviceName)));
        } else if (!ready && triangles_ && lines_) {
            window_.report(QObject::tr("Vulkan could not upload the scene geometry."));
        }
        window_.frameReady();
        if (ready)
            emit window_.owner_.frameSubmitted();
    }
    void logicalDeviceLost() override { window_.report(QObject::tr("Vulkan device lost.")); }
    void physicalDeviceLost() override { window_.report(QObject::tr("Vulkan graphics device is unavailable.")); }

private:
    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize capacity{};
        quint64 revision = 0;
        std::set<ModelComponent> highlights;
    };
    // Depth of field and motion blur from the project's camera settings; world views only.
    ViewportFrameVulkan::Camera cameraEffects(const WorldAtmosphere &environment,const QMatrix4x4 &toWorld,const QVector3D &eye,
                                              const QVector3D &forward) const
    {
        ViewportFrameVulkan::Camera camera;
        camera.depthOfField = environment.depthOfField && !window_.modelOnly_;
        camera.focusDistance = environment.focusDistance;
        camera.fNumber = environment.fNumber;
        camera.verticalFov = qDegreesToRadians(45.0f);
        camera.motionBlur = environment.motionBlur && !window_.modelOnly_;
        camera.shutter = environment.shutter;
        camera.eye = {eye.x(),eye.y(),eye.z()};
        camera.forward = {forward.x(),forward.y(),forward.z()};
        std::copy_n(toWorld.constData(),16,camera.inverseViewProjection.begin());
        return camera;
    }

    // Sun shadow cascades fitted to the steady camera (no TAA jitter) and to the scene's bounds.
    wf::shadow::Cascades shadowCascades(const Geometry &geometry,QVector3D sun,const QMatrix4x4 &camera)
    {
        if (shadowBoundsRevision_ != window_.geometryRevision_) {
            shadowBoundsRevision_ = window_.geometryRevision_;
            shadowBounds_.reset();
            for (const auto &vertex : geometry.triangles) {
                const wf::shadow::Vector point{vertex.position[0],vertex.position[1],vertex.position[2]};
                if (!shadowBounds_)
                    shadowBounds_ = wf::shadow::Bounds{point,point};
                for (int axis = 0; axis < 3; ++axis) {
                    shadowBounds_->minimum[axis] = std::min(shadowBounds_->minimum[axis],point[axis]);
                    shadowBounds_->maximum[axis] = std::max(shadowBounds_->maximum[axis],point[axis]);
                }
            }
        }
        wf::shadow::Matrix toWorld;
        const auto inverse = camera.inverted();
        std::copy_n(inverse.constData(),16,toWorld.begin());
        return wf::shadow::fit(toWorld,{sun.x(),sun.y(),sun.z()},shadowBounds_);
    }

    // Renders the scene's depth into square tiles of an atlas, one per matrix: the sun shadow cascades or the
    // sky visibility maps. The atlas is always cleared, so it is ready to sample even when nothing is drawn.
    void recordDepthTiles(VkCommandBuffer command,const Buffer &frame,VkFramebuffer framebuffer,uint32_t width,uint32_t height,
                          uint32_t tileSize,bool draw,std::size_t count,std::span<const wf::shadow::Matrix> matrices,float wind)
    {
        const VkRect2D area{{0,0},{width,height}};
        const uint32_t columns = width/tileSize;
        VkClearValue clear{};
        clear.depthStencil = {1,0};
        const VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,lighting_.pass(),
            framebuffer,area,1,&clear};
        vk_->vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        if (draw) {
            vk_->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadow_);
            const VkDeviceSize offset = 0;
            vk_->vkCmdBindVertexBuffers(command,0,1,&frame.buffer,&offset);
            for (std::size_t index = 0; index < matrices.size(); ++index) {
                const std::array<uint32_t,2> origin{uint32_t(index)%columns*tileSize,uint32_t(index)/columns*tileSize};
                const VkViewport viewport{float(origin[0]),float(origin[1]),float(tileSize),float(tileSize),0,1};
                const VkRect2D tile{{int32_t(origin[0]),int32_t(origin[1])},{tileSize,tileSize}};
                vk_->vkCmdSetViewport(command,0,1,&viewport);
                vk_->vkCmdSetScissor(command,0,1,&tile);
                ViewUniform uniforms{};
                std::copy_n(matrices[index].data(),16,uniforms.matrix);
                uniforms.eye[3] = wind;
                vk_->vkCmdPushConstants(command,layout_,shaderStages,0,sizeof(uniforms),&uniforms);
                vk_->vkCmdDraw(command,uint32_t(count),1,0,0);
            }
        }
        vk_->vkCmdEndRenderPass(command);
    }
    void destroy(Buffer &buffer)
    {
        if (buffer.buffer)
            vk_->vkDestroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory)
            vk_->vkFreeMemory(device_, buffer.memory, nullptr);
        buffer = {};
    }
    bool upload(Buffer &buffer,const Geometry &geometry,quint64 revision)
    {
        const auto &selection = window_.selection_;
        const bool rebuild = buffer.revision != revision;
        const bool highlight = !geometry.faceRanges.empty() && (rebuild || selection != buffer.highlights);
        if (!rebuild && !highlight)
            return true;
        const auto triangleBytes = geometry.triangles.size()*sizeof(Vertex);
        const auto lineBytes = geometry.lines.size()*sizeof(Vertex);
        const auto overlayBytes = geometry.overlays.size()*sizeof(Vertex);
        const auto outlineBytes = geometry.selectionOutline.size()*sizeof(Vertex);
        const auto bytes = triangleBytes + lineBytes + overlayBytes + outlineBytes;
        if (buffer.capacity < bytes) {
            destroy(buffer);
            VkBufferCreateInfo desc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            desc.size = std::max<VkDeviceSize>(bytes, 4096);
            desc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            if (vk_->vkCreateBuffer(device_, &desc, nullptr, &buffer.buffer) != VK_SUCCESS)
                return false;
            VkMemoryRequirements requirements;
            vk_->vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
            VkPhysicalDeviceMemoryProperties memory;
            window_.vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(window_.physicalDevice(), &memory);
            uint32_t index = 0;
            constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            while (index < memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u<<index))
                   || (memory.memoryTypes[index].propertyFlags & flags) != flags))
                ++index;
            const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, index};
            if (index == memory.memoryTypeCount || vk_->vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory) != VK_SUCCESS
                || vk_->vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0) != VK_SUCCESS) {
                destroy(buffer);
                return false;
            }
            buffer.capacity = desc.size;
        }
        void *target = nullptr;
        if (vk_->vkMapMemory(device_, buffer.memory, 0, bytes, 0, &target) != VK_SUCCESS)
            return false;
        auto *memory = static_cast<char *>(target);
        if (rebuild) {
            buffer.highlights.clear();
            if (triangleBytes)
                std::memcpy(memory,geometry.triangles.data(),triangleBytes);
            if (lineBytes)
                std::memcpy(memory+triangleBytes,geometry.lines.data(),lineBytes);
            if (overlayBytes)
                std::memcpy(memory+triangleBytes+lineBytes,geometry.overlays.data(),overlayBytes);
            if (outlineBytes)
                std::memcpy(memory+triangleBytes+lineBytes+overlayBytes,geometry.selectionOutline.data(),outlineBytes);
        }
        if (highlight) {
            const auto paint = [&](ModelComponent component,bool selected) {
                if (component.index < 0 || std::size_t(component.index) >= geometry.faceRanges.size())
                    return;
                const auto &range = geometry.faceRanges[component.index];
                for (auto i = range.triangles; i < range.triangleEnd; ++i) {
                    auto vertex = geometry.triangles[i];
                    if (selected) {
                        const auto strength = geometry.faceLighting[i];
                        static const auto highlight = linearColor({0.65f,0.27f,0.95f});
                        vertex.color[0] = highlight.x()*strength;
                        vertex.color[1] = highlight.y()*strength;
                        vertex.color[2] = highlight.z()*strength;
                        vertex.surfaceFactors[1] = 0;
                        vertex.texture[0] = 0;
                    }
                    std::memcpy(memory+i*sizeof(Vertex),&vertex,sizeof(vertex));
                }
                for (auto i = range.lines; i < range.lineEnd; ++i) {
                    auto vertex = geometry.lines[i];
                    if (selected) {
                        vertex.color[0] = 0.9f;
                        vertex.color[1] = 0.7f;
                        vertex.color[2] = 1.0f;
                    }
                    std::memcpy(memory+triangleBytes+i*sizeof(Vertex),&vertex,sizeof(vertex));
                }
            };
            for (const auto face : buffer.highlights)
                if (!selection.contains(face))
                    paint(face,false);
            for (const auto face : selection)
                if (!buffer.highlights.contains(face))
                    paint(face,true);
            buffer.highlights = selection;
        }
        vk_->vkUnmapMemory(device_, buffer.memory);
        buffer.revision = revision;
        return true;
    }
    VkShaderModule shader(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return VK_NULL_HANDLE;
        const auto code = file.readAll();
        if (code.isEmpty() || code.size()%4)
            return VK_NULL_HANDLE;
        std::vector<uint32_t> words(code.size()/4);
        std::memcpy(words.data(), code.constData(), code.size());
        const VkShaderModuleCreateInfo desc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                            std::size_t(code.size()), words.data()};
        VkShaderModule module{};
        return vk_->vkCreateShaderModule(device_, &desc, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
    }
    enum class Pass { Scene, Sky, Shadow, Overlay, SelectionOutline };

    VkPipeline pipeline(VkPrimitiveTopology topology, bool depthWrite, Pass pass = Pass::Scene)
    {
        const bool sky = pass == Pass::Sky, shadow = pass == Pass::Shadow;
        const bool outline = pass == Pass::SelectionOutline;
        const auto vertex = shader(shadow ? ":/shaders/Shadow.vert.spv" : sky ? ":/shaders/Sky.vert.spv"
            : outline ? ":/shaders/SelectionOutline.vert.spv" : ":/shaders/Scene.vert.spv");
        const auto fragment = shadow ? VK_NULL_HANDLE : shader(sky ? ":/shaders/Sky.frag.spv"
            : outline ? ":/shaders/SelectionOutline.frag.spv" : ":/shaders/Scene.frag.spv");
        VkPipeline result{};
        if (vertex && (fragment || shadow)) {
            // Sky and lit triangles render into the HDR scene; lines, overlays and outlines draw after
            // the display transform, and the scene shader keeps their sRGB colors (displayPass).
            const bool inScene = sky || (pass == Pass::Scene && topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            const uint32_t subpass = inScene ? ViewportFrameVulkan::sceneSubpass : ViewportFrameVulkan::displaySubpass;
            const VkBool32 displayPass = inScene ? VK_FALSE : VK_TRUE;
            const VkSpecializationMapEntry constant{0,0,sizeof(VkBool32)};
            const VkSpecializationInfo specialization{1,&constant,sizeof(displayPass),&displayPass};
            const VkPipelineShaderStageCreateInfo stages[]{
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", &specialization}};
            const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            const VkVertexInputAttributeDescription attributes[]{
                {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex, position)}, {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex, color)},
                {2,0,VK_FORMAT_R32_SFLOAT,offsetof(Vertex,wind)},{3,0,VK_FORMAT_R32_SFLOAT,offsetof(Vertex,sun)},
                {4,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv)},
                {5,0,VK_FORMAT_R32G32B32_UINT,offsetof(Vertex,texture)},
                {6,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,materialNormal)},
                {7,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,surfaceFactors)}};
            const VkVertexInputAttributeDescription shadowAttributes[]{attributes[0],attributes[2]};
            const VkVertexInputAttributeDescription outlineAttributes[]{attributes[0],attributes[4],attributes[6]};
            const VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                nullptr, 0, sky ? 0u : 1u, &binding, sky ? 0u : shadow ? 2u : outline ? 3u : 8u,
                shadow ? shadowAttributes : outline ? outlineAttributes : attributes};
            const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                nullptr, 0, topology, VK_FALSE};
            const VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                nullptr, 0, 1, nullptr, 1, nullptr};
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1;
            raster.depthBiasEnable = shadow;
            raster.depthBiasConstantFactor = 1.25f;
            raster.depthBiasSlopeFactor = 1.3f;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = pass == Pass::Overlay ? VK_FALSE : VK_TRUE;
            depth.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
            depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            // The scene pass has targets for the sky-light part and the surface albedo, which only Scene.frag writes.
            VkPipelineColorBlendAttachmentState blends[wf::vk::HdrFrame::sceneColorAttachments]{blend,blend,blend};
            if (sky)
                blends[1].colorWriteMask = blends[2].colorWriteMask = 0;
            VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            colors.attachmentCount = inScene ? wf::vk::HdrFrame::sceneColorAttachments : 1;
            colors.pAttachments = blends;
            const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, states};
            VkGraphicsPipelineCreateInfo desc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            desc.stageCount = shadow ? 1 : 2;
            desc.pStages = stages;
            desc.pVertexInputState = &input;
            desc.pInputAssemblyState = &assembly;
            desc.pViewportState = &view;
            desc.pRasterizationState = &raster;
            desc.pMultisampleState = &samples;
            desc.pDepthStencilState = &depth;
            desc.pColorBlendState = shadow ? nullptr : &colors;
            desc.pDynamicState = &dynamic;
            desc.layout = layout_;
            desc.renderPass = shadow ? lighting_.pass() : inScene ? frame_.scenePass() : frame_.displayPass();
            desc.subpass = shadow ? 0 : subpass;
            if (vk_->vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &desc, nullptr, &result) != VK_SUCCESS)
                result = VK_NULL_HANDLE;
        }
        if (vertex)
            vk_->vkDestroyShaderModule(device_, vertex, nullptr);
        if (fragment)
            vk_->vkDestroyShaderModule(device_, fragment, nullptr);
        return result;
    }
    VulkanSceneWindow &window_;
    VkDevice device_{};
    QVulkanDeviceFunctions *vk_{};
    VkPipelineLayout layout_{};
    VkPipeline triangles_{};
    VkPipeline overlays_{};
    VkPipeline outline_{};
    VkPipeline lines_{};
    VkPipeline sky_{};
    VkPipeline shadow_{};
    WorldLightingVulkan lighting_;
    ViewportFrameVulkan frame_;
    std::vector<Buffer> frames_;
    // Auto exposure: the adapted exposure in stops (NaN until the first metered frame) and the time between frames.
    float adaptedStops_ = std::numeric_limits<float>::quiet_NaN();
    QElapsedTimer adaptClock_;
    // TAA: what the previous frame showed, to restart or reproject the history.
    QMatrix4x4 previousCamera_;
    QSize previousSize_;
    quint64 previousGeometry_ = 0, previousTextures_ = 0;
    // The scene's bounds for fitting shadow cascades, for geometry revision shadowBoundsRevision_.
    std::optional<wf::shadow::Bounds> shadowBounds_;
    quint64 shadowBoundsRevision_ = ~quint64(0);
    // The sky visibility maps as last rendered (see skyRefresh in the frame recording).
    static constexpr uint32_t skyRefreshFrames = 30;
    wf::sky::Visibility skyRendered_;
    bool skyValid_ = false, skyInitialized_ = false;
    quint64 skyRevision_ = 0;
    uint32_t skyAge_ = 0;
    // The point light shadow cubes as last rendered.
    std::vector<std::array<float,4>> pointShadowsRendered_;
    quint64 pointShadowRevision_ = 0;
    uint32_t pointShadowAge_ = 0;
    bool pointShadowsInitialized_ = false;
    bool previousValid_ = false;
};

QVulkanWindowRenderer *VulkanSceneWindow::createRenderer() { return new VulkanSceneRenderer(*this); }
#endif

LiveViewport::LiveViewport(ProjectStore &store, bool modelOnly, QWidget *parent)
    : QWidget(parent), status_(new QLabel(this))
{
    setObjectName("liveViewport");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    status_->setContentsMargins(8, 3, 8, 3);
    status_->setWordWrap(true);
    connect(this, &LiveViewport::rendererStatusChanged, status_, &QLabel::setText);
#ifdef WORLDFORGE_VULKAN
    instance_ = std::make_shared<QVulkanInstance>();
    if (instance_->supportedApiVersion() >= QVersionNumber(1, 1))
        instance_->setApiVersion(QVersionNumber(1, 1));
    if (instance_->supportedLayers().contains("VK_LAYER_KHRONOS_validation"))
        instance_->setLayers({"VK_LAYER_KHRONOS_validation"});
    if (instance_->create()) {
        window_ = new VulkanSceneWindow(store, modelOnly, *this);
        window_->setObjectName("vulkanSceneWindow");
        window_->setVulkanInstance(instance_.get());
        const auto devices = window_->availablePhysicalDevices();
        for (qsizetype i = 0; i < devices.size(); ++i) {
            const auto &adapter = devices[i];
            if (adapter.vendorID == 0x10de && QByteArray(adapter.deviceName).contains("GTX 1650")) {
                window_->setPhysicalDeviceIndex(int(i));
                break;
            }
        }
        container_ = QWidget::createWindowContainer(window_, this);
        container_->setMinimumSize(160, 140);
        container_->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(container_, 1);
        status_->setText(tr("Starting Vulkan viewport…"));
        connect(&store, &ProjectStore::sceneChanged, this, [this] { window_->sceneEdited(); });
        if (!modelOnly)
            connect(&store,&ProjectStore::nativeGameplayChanged,this,[this] { window_->nativeGameplayChanged(); });
        connect(&store, &ProjectStore::selectionChanged, this, [this, &store, modelOnly, selection = store.selection()]() mutable {
            window_->invalidate();
            if (modelOnly && selection != store.selection())
                window_->frame();
            selection = store.selection();
        });
    } else {
        status_->setText(tr("Vulkan could not start (VkResult %1). Check the graphics driver.").arg(int(instance_->errorCode())));
        layout->addStretch(1);
    }
#else
    Q_UNUSED(store);
    Q_UNUSED(modelOnly);
    status_->setText(tr("This build has no Vulkan viewport. Install Vulkan headers and glslang, then rebuild."));
    layout->addStretch(1);
#endif
    layout->addWidget(status_);
}

LiveViewport::~LiveViewport()
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->stop();
#endif
    delete container_;
}

bool LiveViewport::start(RunMode mode)
{
#ifdef WORLDFORGE_VULKAN
    if (window_ && window_->start(mode)) {
        if (mode == RunMode::Play)
            container_->setFocus(Qt::OtherFocusReason);
        emit runtimeChanged();
        return true;
    }
#else
    Q_UNUSED(mode);
#endif
    return false;
}

void LiveViewport::stop()
{
#ifdef WORLDFORGE_VULKAN
    if (window_ && window_->mode() != RunMode::Editing) {
        window_->stop();
        emit runtimeChanged();
    }
#endif
}

void LiveViewport::pause(bool paused)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->pause(paused);
#else
    Q_UNUSED(paused);
#endif
}

void LiveViewport::step()
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->stepOnce();
#endif
}

LiveViewport::RunMode LiveViewport::runMode() const
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        return window_->mode();
#endif
    return RunMode::Editing;
}

void LiveViewport::setTerrainBrush(float radius, float strength, bool smooth)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setBrush(radius,strength,smooth);
#else
    Q_UNUSED(radius);
    Q_UNUSED(strength);
    Q_UNUSED(smooth);
#endif
}

std::optional<QVector3D> LiveViewport::runtimePosition(const QString &id) const
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        return window_->runtimePosition(id);
#else
    Q_UNUSED(id);
#endif
    return {};
}

QString LiveViewport::runtimeObjective() const
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        return window_->runtimeObjective();
#endif
    return {};
}

std::optional<QVector3D> LiveViewport::playerPosition() const
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        return window_->playerPosition();
#endif
    return {};
}

void LiveViewport::frame()
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->frame();
#endif
}

void LiveViewport::previewTerrain()
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->previewTerrain();
#endif
}

void LiveViewport::setTerrainLayerBrush(float radius,int layer)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setLayerBrush(radius,layer);
#else
    Q_UNUSED(radius);
    Q_UNUSED(layer);
#endif
}

void LiveViewport::setRenderDevice(const QString &name, const QString &counterId, quint64 dedicatedMemory, quint64 sharedMemory)
{
    rendererName_ = name;
    adapterCounterId_ = counterId;
    dedicatedMemory_ = dedicatedMemory;
    sharedMemory_ = sharedMemory;
}

void LiveViewport::setModelSelection(ModelDomain domain,const std::set<ModelComponent> &selection,bool xray)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelSelection(domain,selection,xray);
#else
    Q_UNUSED(domain);
    Q_UNUSED(selection);
    Q_UNUSED(xray);
#endif
}

void LiveViewport::setModelPreview(QSharedPointer<const worldforge::Mesh> mesh,std::optional<ModelGizmo> gizmo)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelPreview(std::move(mesh),std::move(gizmo));
#else
    Q_UNUSED(mesh);
    Q_UNUSED(gizmo);
#endif
}

void LiveViewport::setAssetPreview(QSharedPointer<const worldforge::Mesh> mesh,bool reframe)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setAssetPreview(std::move(mesh),reframe);
#else
    Q_UNUSED(mesh);
    Q_UNUSED(reframe);
#endif
}

void LiveViewport::setPlacementPreview(const SceneObject &placement,QSharedPointer<const worldforge::Mesh> mesh)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setPlacementPreview(placement,std::move(mesh));
#else
    Q_UNUSED(placement);
    Q_UNUSED(mesh);
#endif
}

void LiveViewport::setCameraPreview(QVector3D eye,QVector3D target)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setCameraPreview(eye,target);
#else
    Q_UNUSED(eye);
    Q_UNUSED(target);
#endif
}

bool LiveViewport::runtimeShowsHud() const
{
#ifdef WORLDFORGE_VULKAN
    return window_ && window_->runtimeShowsHud();
#else
    return false;
#endif
}

void LiveViewport::setBaseColorPreview(int slot,QImage image)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setBaseColorPreview(slot,std::move(image));
#else
    Q_UNUSED(slot);
    Q_UNUSED(image);
#endif
}

void LiveViewport::setObjectPreview(bool enabled,const SceneObject *object)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setObjectPreview(enabled,object);
#else
    Q_UNUSED(enabled);
    Q_UNUSED(object);
#endif
}

void LiveViewport::setModelOrthographic(bool enabled)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelOrthographic(enabled);
#else
    Q_UNUSED(enabled);
#endif
}

void LiveViewport::setModelShading(ModelShading shading)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelShading(shading);
#else
    Q_UNUSED(shading);
#endif
}

void LiveViewport::setModelAxisView(int axis)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelAxisView(axis);
#else
    Q_UNUSED(axis);
#endif
}

void LiveViewport::setModelReference(const QString &id)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelReference(id);
#else
    Q_UNUSED(id);
#endif
}

void LiveViewport::setModelLodPreview(bool enabled)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelLodPreview(enabled);
#else
    Q_UNUSED(enabled);
#endif
}

void LiveViewport::setModelKnife(bool enabled,std::optional<QVector3D> start)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelKnife(enabled,start);
#else
    Q_UNUSED(enabled);
    Q_UNUSED(start);
#endif
}

void LiveViewport::setModelRetopology(bool enabled,const QList<QVector3D> &points)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelRetopology(enabled,points);
#else
    Q_UNUSED(enabled);
    Q_UNUSED(points);
#endif
}

void LiveViewport::setSculptMode(bool enabled)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setSculptMode(enabled);
#else
    Q_UNUSED(enabled);
#endif
}

void LiveViewport::setModelSurfaceTool(bool enabled)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelSurfaceTool(enabled);
#else
    Q_UNUSED(enabled);
#endif
}

void LiveViewport::setModelMarkup(const QList<QVector3D> &measurement,const QList<QVector3D> &stroke)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelMarkup(measurement,stroke);
#else
    Q_UNUSED(measurement);
    Q_UNUSED(stroke);
#endif
}

void LiveViewport::setModelPairPicking(bool enabled)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setModelPairPicking(enabled);
#else
    Q_UNUSED(enabled);
#endif
}

std::optional<qsizetype> LiveViewport::annotationNear(const QVector3D &position) const
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        return window_->annotationNear(position);
#else
    Q_UNUSED(position);
#endif
    return {};
}

std::optional<QVector3D> LiveViewport::conformToReference(const QVector3D &position)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        return window_->conformToReference(position);
#else
    Q_UNUSED(position);
#endif
    return {};
}

bool LiveViewport::hasRenderedFrame() const
{
#ifdef WORLDFORGE_VULKAN
    return window_ && window_->rendered();
#else
    return false;
#endif
}

QImage LiveViewport::captureFrame()
{
#ifdef WORLDFORGE_VULKAN
    if (hasRenderedFrame() && window_->supportsGrab()) {
        // Each grab renders one frame; a still view needs all its TAA samples for a converged image.
        auto image = window_->grab();
        for (int frame = 0; frame < 40 && !window_->temporalSettled(); ++frame)
            image = window_->grab();
        return image;
    }
#endif
    return {};
}
