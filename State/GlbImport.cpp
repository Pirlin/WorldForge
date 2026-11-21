#include "State/GlbImport.h"
#include "Engine/Geometry/MeshClip.h"
#include "State/StudioClip.h"
#include "Engine/Render/ColorPipeline.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QObject>
#include <QQuaternion>
#include <QSet>
#include <QStringList>
#include <QtEndian>

#include <bit>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace {
namespace wf = worldforge;
constexpr int limit = 2'000'000;

void require(bool condition, const QString &error)
{
    if (!condition)
        throw error;
}

int integer(const QJsonValue &value, int fallback = -1)
{
    if (value.isUndefined())
        return fallback;
    const auto n = value.toDouble(-1);
    require(n >= 0 && n <= std::numeric_limits<int>::max() && n == std::floor(n),
            QStringLiteral("GLB has an invalid integer."));
    return int(n);
}

QJsonObject entry(const QJsonArray &array, int index)
{
    require(index >= 0 && index < array.size() && array.at(index).isObject(),
            QStringLiteral("GLB references a missing object."));
    return array.at(index).toObject();
}

template<std::size_t N>
std::array<float, N> vector(const QJsonValue &value, std::array<float, N> fallback = {})
{
    if (value.isUndefined())
        return fallback;
    const auto values = value.toArray();
    require(values.size() == qsizetype(N), QStringLiteral("GLB has an invalid vector."));
    for (std::size_t i = 0; i < N; ++i) {
        fallback[i] = float(values.at(i).toDouble(std::numeric_limits<double>::quiet_NaN()));
        require(std::isfinite(fallback[i]), QStringLiteral("GLB has a non-finite vector."));
    }
    return fallback;
}

wf::Point3 zUp(QVector3D p) { return {p.x(), -p.z(), p.y()}; }

struct Accessor {
    const char *data;
    int count;
    int stride;
    int type;
    int width;
    bool normalized;

    quint32 index(int row, int channel = 0) const
    {
        const auto *ptr = data + qsizetype(row)*stride + channel*width;
        if (width == 1)
            return quint8(*ptr);
        return width == 2 ? qFromLittleEndian<quint16>(ptr) : qFromLittleEndian<quint32>(ptr);
    }

    float value(int row, int channel) const
    {
        const auto raw = index(row, channel);
        if (type == 5126)
            return std::bit_cast<float>(raw);
        return float(raw) / (width == 1 ? 255.0f : 65535.0f);
    }
};

class Importer {
public:
    Importer(QJsonObject json,QByteArrayView bytes,const QByteArray &uvManifest,GlbUvLayers &layers,GlbMotion *motion)
        : json_(std::move(json)),bin_(bytes),layers_(layers),motion_(motion)
    {
        if (uvManifest.isEmpty())
            return;
        const auto manifest = QJsonDocument::fromJson(uvManifest).object();
        const auto names = manifest.value("names").toArray();
        require(uvManifest.size() <= 8192 && manifest.value("version").toInt() == 1
                && manifest.value("names").isArray() && names.size() <= 8,
                QStringLiteral("Studio UV manifest is invalid."));
        activeUv_ = integer(manifest.value("render"),0);
        require(activeUv_ < std::max<qsizetype>(1,names.size()),QStringLiteral("Studio render UV index is invalid."));
        for (const auto &value : names) {
            const auto name = value.toString();
            require(!name.trimmed().isEmpty() && name.size() <= 64 && !uvNames_.contains(name),
                    QStringLiteral("Studio UV layers must have unique names of up to 64 characters."));
            uvNames_.append(name);
        }
        if (!uvNames_.isEmpty()) {
            layers_.active = uvNames_.at(activeUv_);
            for (const auto &name : uvNames_)
                if (name != layers_.active)
                    layers_.other.insert(name,{});
        }
    }

    wf::Mesh run()
    {
        require(json_.value("asset").toObject().value("version").toString() == "2.0",
                QStringLiteral("Only glTF 2.0 is supported."));
        require(json_.value("extensionsRequired").toArray().isEmpty(),
                QStringLiteral("This GLB requires an unsupported extension."));
        const auto buffers = json_.value("buffers").toArray();
        const auto buffer = entry(buffers, 0);
        const auto length = integer(buffer.value("byteLength"));
        require(buffers.size() == 1 && !buffer.contains("uri") && length > 0 && length <= bin_.size(),
                QStringLiteral("GLB must contain one embedded binary buffer."));
        bin_ = bin_.first(length);
        for (const auto &value : json_.value("materials").toArray()) {
            const auto material = value.toObject();
            const auto pbr = material.value("pbrMetallicRoughness").toObject();
            auto &slot = mesh_.materials.emplace_back(wf::MeshMaterial{material.value("name").toString("Material").toStdString(),
                vector<4>(pbr.value("baseColorFactor"), {1,1,1,1}),
                float(pbr.value("metallicFactor").toDouble(1)), float(pbr.value("roughnessFactor").toDouble(1))});
            // glTF stores a linear base color factor; WorldForge material colors are sRGB-encoded.
            for (int channel = 0; channel < 3; ++channel)
                slot.color[channel] = wf::color::linearToSrgb(slot.color[channel]);
            slot.emissive = vector<3>(material.value("emissiveFactor"));
            const QJsonObject fields[]{pbr.value("baseColorTexture").toObject(),material.value("normalTexture").toObject(),
                pbr.value("metallicRoughnessTexture").toObject(),material.value("occlusionTexture").toObject(),
                material.value("emissiveTexture").toObject()};
            const auto channels = slot.previewTextures();
            for (unsigned i = 0; i < channels.size(); ++i)
                if (!fields[i].isEmpty())
                    readTexture(*channels[i],fields[i]);
        }
        const auto scene = entry(json_.value("scenes").toArray(), integer(json_.value("scene"), 0));
        for (const auto &node : scene.value("nodes").toArray())
            visit(integer(node), QMatrix4x4(), 0);
        const auto error = wf::validateMesh(mesh_);
        require(error.empty(), QString::fromStdString(error));
        if (motion_) {
            const auto &source = motion_->source;
            require(motionIndices_.size() == mesh_.vertices.size()
                && motionIndices_.size() <= wf::MeshClip::maxSamples/source.frameCount(),
                QStringLiteral("The animated mesh exceeds the vertex sample budget."));
            auto mapped = QSharedPointer<wf::MeshClip>::create();
            mapped->name = source.name;
            mapped->framesPerSecond = source.framesPerSecond;
            mapped->vertexCount = mesh_.vertices.size();
            mapped->samples.reserve(source.frameCount()*mapped->vertexCount);
            for (std::size_t frame = 0; frame < source.frameCount(); ++frame)
                for (const auto vertex : motionIndices_)
                    mapped->samples.push_back(source.samples[frame*source.vertexCount+vertex]);
            motion_->mapped = std::move(mapped);
        }
        return std::move(mesh_);
    }

private:
    void readTexture(std::vector<std::uint8_t> &channel,const QJsonObject &color)
    {
        const auto uv = integer(color.value("texCoord"),0);
        if (!uvNames_.isEmpty())
            require(uv == activeUv_,QStringLiteral("Studio textures must use the render UV layer for this exchange."));
        if (uv != activeUv_ || color.contains("extensions"))
            return;
        const auto texture = entry(json_.value("textures").toArray(),integer(color.value("index")));
        if (!texture.contains("source"))
            return;
        const auto image = entry(json_.value("images").toArray(),integer(texture.value("source")));
        const auto mime = image.value("mimeType").toString();
        if (!image.contains("bufferView") || (mime != "image/png" && mime != "image/jpeg"))
            return;
        const auto view = entry(json_.value("bufferViews").toArray(),integer(image.value("bufferView")));
        const auto start = integer(view.value("byteOffset"),0);
        const auto length = integer(view.value("byteLength"));
        require(integer(view.value("buffer")) == 0 && length > 0 && qint64(start)+length <= bin_.size(),
            QStringLiteral("GLB material image has an invalid buffer range."));
        textureBytes_ += std::size_t(length);
        require(textureBytes_ <= wf::MeshMaterial::maxTextureBytes,QStringLiteral("GLB material images exceed 16 MiB."));
        const auto *begin = bin_.data()+start;
        channel.assign(begin,begin+length);
    }

    std::size_t textureBytes_ = 0;
    Accessor accessor(int id, const QString &shape, int components) const
    {
        const auto field = entry(json_.value("accessors").toArray(), id);
        const auto view = entry(json_.value("bufferViews").toArray(), integer(field.value("bufferView")));
        const int type = integer(field.value("componentType"));
        const int width = type == 5121 ? 1 : type == 5123 ? 2 : (type == 5125 || type == 5126) ? 4 : 0;
        const int count = integer(field.value("count"));
        const int stride = integer(view.value("byteStride"), width*components);
        const int start = integer(field.value("byteOffset"), 0);
        const int offset = integer(view.value("byteOffset"), 0);
        const int length = integer(view.value("byteLength"));
        require(field.value("type").toString() == shape && !field.contains("sparse") && width > 0
                && integer(view.value("buffer")) == 0 && offset >= 0 && length >= 0 && count > 0 && count <= limit*3
                && stride >= width*components && stride <= 252 && stride % width == 0
                && offset % width == 0 && start % width == 0
                && qint64(start) + qint64(count-1)*stride + width*components <= length
                && qint64(offset) + length <= bin_.size(),
                QStringLiteral("GLB accessor has unsupported data or an invalid buffer range."));
        return {bin_.data() + offset + start, count, stride, type, width, field.value("normalized").toBool()};
    }

    void visit(int index, QMatrix4x4 matrix, int depth)
    {
        require(depth < 128 && !visited_.contains(index), QStringLiteral("GLB node graph is too deep or repeats a node."));
        visited_.insert(index);
        const auto node = entry(json_.value("nodes").toArray(), index);
        require(!node.contains("skin"), QStringLiteral("Skinned GLB assets are outside the static-mesh import path."));
        if (node.contains("matrix")) {
            const auto raw = vector<16>(node.value("matrix"));
            QMatrix4x4 local;
            std::copy(raw.begin(), raw.end(), local.data());
            matrix *= local;
        } else {
            const auto t = vector<3>(node.value("translation"));
            const auto r = vector<4>(node.value("rotation"), {0,0,0,1});
            const auto s = vector<3>(node.value("scale"), {1,1,1});
            const QQuaternion rotation(r[3], r[0], r[1], r[2]);
            require(!rotation.isNull(), QStringLiteral("GLB node has an invalid rotation."));
            matrix.translate(t[0], t[1], t[2]);
            matrix.rotate(rotation.normalized());
            matrix.scale(s[0], s[1], s[2]);
        }
        if (node.contains("mesh")) {
            const auto mesh = entry(json_.value("meshes").toArray(), integer(node.value("mesh")));
            for (const auto &value : mesh.value("primitives").toArray())
                primitive(value.toObject(), matrix);
        }
        for (const auto &child : node.value("children").toArray())
            visit(integer(child), matrix, depth + 1);
    }

    void primitive(const QJsonObject &primitive, const QMatrix4x4 &matrix)
    {
        require(integer(primitive.value("mode"), 4) == 4 && !primitive.contains("targets")
                && primitive.value("extensions").toObject().isEmpty(),
                QStringLiteral("GLB import accepts static triangle primitives."));
        const auto attributes = primitive.value("attributes").toObject();
        const auto positions = accessor(integer(attributes.value("POSITION")), "VEC3", 3);
        require(positions.type == 5126 && !positions.normalized && positions.count <= limit
                && mesh_.vertices.size() + positions.count <= limit,
                QStringLiteral("GLB position accessor must use float triples within the geometry limit."));
        std::optional<Accessor> normals, indices;
        std::array<std::optional<Accessor>,8> uvs;
        std::array<std::vector<wf::Point2> *,8> other{};
        if (attributes.contains("NORMAL")) {
            normals = accessor(integer(attributes.value("NORMAL")), "VEC3", 3);
            require(normals->type == 5126 && !normals->normalized && normals->count == positions.count,
                    QStringLiteral("GLB normals must match the position count."));
        }
        for (int i = 0; i < std::max<qsizetype>(1,uvNames_.size()); ++i) {
            const auto key = QStringLiteral("TEXCOORD_%1").arg(i);
            if (!attributes.contains(key)) {
                require(uvNames_.isEmpty(),QStringLiteral("The staged mesh is missing a named UV layer."));
                continue;
            }
            auto &uv = uvs[i].emplace(accessor(integer(attributes.value(key)), "VEC2", 2));
            require(uv.count == positions.count && ((uv.type == 5126 && !uv.normalized)
                    || (uv.normalized && (uv.type == 5121 || uv.type == 5123))),
                    QStringLiteral("GLB UVs have an invalid type or count."));
            if (i != activeUv_)
                other[i] = &layers_.other[uvNames_.at(i)];
        }
        if (primitive.contains("indices")) {
            indices = accessor(integer(primitive.value("indices")), "SCALAR", 1);
            require(indices->type != 5126 && !indices->normalized, QStringLiteral("GLB indices must be unsigned integers."));
        }
        const int count = indices ? indices->count : positions.count;
        require(count % 3 == 0 && mesh_.faces.size() + count/3 <= limit,
                QStringLiteral("GLB index count does not describe complete triangles within the geometry limit."));
        require(layers_.other.size() * (qsizetype(mesh_.faces.size())*3 + count) <= 4'000'000,
                QStringLiteral("The named UV layers exceed the Studio exchange limit."));
        const auto material = qint64(integer(primitive.value("material"))) + 1;
        require(material >= 0 && std::size_t(material) < mesh_.materials.size(), QStringLiteral("GLB material is missing."));
        const auto determinant = matrix.determinant();
        require(matrix.isAffine() && std::isfinite(determinant) && determinant != 0,
                QStringLiteral("GLB mesh node transform must be finite, affine, and invertible."));
        const auto normalMatrix = matrix.normalMatrix();
        const auto firstVertex = wf::VertexIndex(mesh_.vertices.size());
        std::optional<Accessor> vertexMap;
        if (motion_) {
            vertexMap = accessor(integer(attributes.value("_WORLDFORGE_VERTEX")), "SCALAR", 1);
            require(vertexMap->type == 5126 && !vertexMap->normalized && vertexMap->count == positions.count,
                    QStringLiteral("The animated GLB has an invalid vertex map."));
        }
        for (int i = 0; i < positions.count; ++i) {
            const auto position = zUp(matrix.map(QVector3D(positions.value(i,0),positions.value(i,1),positions.value(i,2))));
            mesh_.vertices.push_back(position);
            if (vertexMap) {
                const auto raw = vertexMap->value(i,0);
                require(raw >= 0 && raw < float(motion_->source.vertexCount) && raw == std::floor(raw),
                        QStringLiteral("The animated GLB references a missing rig vertex."));
                const auto index = std::size_t(raw);
                const auto &reference = motion_->source.samples[index].position;
                for (int axis = 0; axis < 3; ++axis)
                    require(std::abs(reference[axis]-position[axis]) <= 0.001f+std::abs(reference[axis])*0.00001f,
                            QStringLiteral("The animated GLB's first pose does not match its mesh."));
                motionIndices_.push_back(index);
            }
        }
        for (int i = 0; i < count; i += 3) {
            wf::MeshFace face;
            face.material = std::uint32_t(material);
            for (int j = 0; j < 3; ++j) {
                const int offset = i + (determinant < 0 && j != 0 ? 3-j : j);
                const auto vertex = indices ? indices->index(offset) : quint32(offset);
                require(vertex < quint32(positions.count), QStringLiteral("GLB triangle index is out of bounds."));
                wf::MeshCorner corner{firstVertex + vertex};
                for (std::size_t channel = 0; channel < uvs.size(); ++channel) {
                    if (!uvs[channel])
                        continue;
                    const wf::Point2 uv{uvs[channel]->value(vertex,0),1.0f-uvs[channel]->value(vertex,1)};
                    require(std::isfinite(uv[0]) && std::isfinite(uv[1])
                        && (uvNames_.isEmpty() || (std::abs(uv[0]) <= 1000 && std::abs(uv[1]) <= 1000)),
                        QStringLiteral("GLB has a UV coordinate outside the supported range."));
                    if (other[channel])
                        other[channel]->push_back(uv);
                    else
                        corner.uv = uv;
                }
                if (normals) {
                    const QVector3D source(normals->value(vertex, 0), normals->value(vertex, 1), normals->value(vertex, 2));
                    QVector3D n;
                    for (int row = 0; row < 3; ++row)
                        n[row] = normalMatrix(row,0)*source.x() + normalMatrix(row,1)*source.y() + normalMatrix(row,2)*source.z();
                    corner.normal = zUp(n.normalized());
                }
                face.corners.push_back(corner);
            }
            mesh_.faces.push_back(std::move(face));
        }
    }

    QJsonObject json_;
    QByteArrayView bin_;
    QSet<int> visited_;
    GlbUvLayers &layers_;
    QStringList uvNames_;
    int activeUv_ = 0;
    GlbMotion *motion_;
    std::vector<std::size_t> motionIndices_;
    wf::Mesh mesh_;
};
}

std::optional<worldforge::Mesh> importGlb(const QByteArray &bytes,QString &error,const QByteArray &uvManifest,
                                         GlbUvLayers *uvLayers,GlbMotion *motion)
{
    try {
        require(bytes.size() >= 20 && bytes.size() <= 64*1024*1024, QStringLiteral("GLB is empty or exceeds 64 MiB."));
        const auto u32 = [&](qsizetype at) { return qFromLittleEndian<quint32>(bytes.constData() + at); };
        require(u32(0) == 0x46546C67 && u32(4) == 2 && u32(8) == quint32(bytes.size()),
                QStringLiteral("File is not a valid GLB 2.0 container."));
        const auto jsonSize = u32(12);
        require(u32(16) == 0x4E4F534A && jsonSize % 4 == 0 && jsonSize <= quint32(bytes.size()-20),
                QStringLiteral("GLB JSON chunk is missing or truncated."));
        const auto json = QJsonDocument::fromJson(bytes.mid(20, jsonSize));
        const qsizetype binaryHeader = 20 + jsonSize;
        require(json.isObject() && binaryHeader + 8 <= bytes.size(), QStringLiteral("GLB JSON or binary chunk is missing."));
        const auto binarySize = u32(binaryHeader);
        require(u32(binaryHeader + 4) == 0x004E4942 && binarySize % 4 == 0
                && binarySize <= quint32(bytes.size() - binaryHeader - 8), QStringLiteral("GLB binary chunk is invalid."));
        error.clear();
        GlbUvLayers layers;
        require(!motion || motion->source.valid(motion->source.vertexCount),QStringLiteral("The Studio clip is invalid."));
        auto mesh = Importer(json.object(),QByteArrayView(bytes.constData()+binaryHeader+8,binarySize),uvManifest,layers,motion).run();
        if (uvLayers)
            *uvLayers = std::move(layers);
        return mesh;
    } catch (const QString &message) {
        error = message;
        return std::nullopt;
    }
}

std::optional<worldforge::Mesh> importStudioGlb(const QByteArray &bytes,const QByteArray &uvManifest,
    const QByteArray &clipData,StudioMeshData &data,QString &error)
{
    GlbUvLayers layers;
    std::optional<wf::Mesh> mesh;
    data = {};
    if (clipData.isEmpty()) {
        mesh = importGlb(bytes,error,uvManifest,&layers);
    } else {
        const auto clip = readStudioClip(clipData);
        if (!clip) {
            error = QObject::tr("The staged animation clip is corrupt or exceeds the supported sample budget.");
            return {};
        }
        GlbMotion motion{*clip,{}};
        mesh = importGlb(bytes,error,uvManifest,&layers,&motion);
        data.clip = std::move(motion.mapped);
    }
    if (mesh && !uvManifest.isEmpty())
        data.uvs = std::move(layers);
    return mesh;
}

QByteArray exportGlb(const worldforge::Mesh &mesh,const QString &name,QString &error,bool studioTopology,
                     const GlbUvLayers &uvLayers)
{
    constexpr qsizetype maxBytes = 64*1024*1024;
    error = QString::fromStdString(wf::validateMesh(mesh));
    if (!error.isEmpty())
        return {};
    struct Primitive {
        using Vertex = std::array<float,8>;
        std::map<Vertex,quint32> indices;
        std::vector<Vertex> vertices;
        std::vector<quint32> triangles;
    };
    std::map<std::uint32_t,Primitive> primitives;
    qsizetype bytes = 0;
    std::size_t vertexCount = 0, triangleCount = 0;
    for (const auto &face : mesh.faces) {
        auto &part = primitives[face.material];
        const auto normal = wf::faceNormal(mesh,face);
        const auto before = part.triangles.size();
        wf::visitFaceTriangles(mesh,face,[&](const auto &triangle) {
            for (const auto index : triangle) {
                const auto &corner = face.corners[index];
                const auto p = mesh.vertices[corner.vertex];
                auto n = QVector3D(corner.normal[0],corner.normal[1],corner.normal[2]).normalized();
                if (n.isNull())
                    n = QVector3D(normal[0],normal[1],normal[2]);
                const Primitive::Vertex vertex{p[0],p[2],-p[1],n.x(),n.z(),-n.y(),corner.uv[0],1-corner.uv[1]};
                const auto [it,inserted] = part.indices.try_emplace(vertex,quint32(part.vertices.size()));
                if (inserted) {
                    part.vertices.push_back(vertex);
                    bytes += 32;
                    ++vertexCount;
                }
                part.triangles.push_back(it->second);
                bytes += 4;
            }
            ++triangleCount;
        });
        if (part.triangles.size() == before) {
            error = QObject::tr("A mesh face could not be triangulated for GLB export.");
            return {};
        }
        if (bytes > maxBytes || vertexCount > limit || triangleCount > limit) {
            error = QObject::tr("The expanded GLB exceeds the static mesh interchange limit.");
            return {};
        }
    }
    const auto append = [](QByteArray &buffer,quint32 value) {
        char data[4];
        qToLittleEndian(value,data);
        buffer.append(data,4);
    };
    QByteArray binary;
    binary.reserve(bytes);
    QJsonArray views, accessors, entries, materials, images, textures;
    for (const auto &mat : mesh.materials) {
        QJsonArray rgba;
        for (int channel = 0; channel < 4; ++channel) {
            const float value = std::clamp(mat.color[channel],0.0f,1.0f);
            rgba.append(double(channel < 3 ? wf::color::srgbToLinear(value) : value));
        }
        QJsonObject pbr{{"baseColorFactor",rgba},{"metallicFactor",std::clamp(mat.metallic,0.0f,1.0f)},
            {"roughnessFactor",std::clamp(mat.roughness,0.0f,1.0f)}};
        QJsonObject material{{"name",QString::fromStdString(mat.name)},
            {"emissiveFactor",QJsonArray{mat.emissive[0],mat.emissive[1],mat.emissive[2]}}};
        if (studioTopology)
            material.insert("extras",QJsonObject{{"worldforgeSlot",materials.size()}});
        const QString fields[]{"baseColorTexture","normalTexture","metallicRoughnessTexture","occlusionTexture","emissiveTexture"};
        const auto channels = mat.previewTextures();
        for (unsigned i = 0; i < channels.size(); ++i) {
            const auto &imageBytes = *channels[i];
            if (imageBytes.empty())
                continue;
            const QByteArrayView data(reinterpret_cast<const char *>(imageBytes.data()),imageBytes.size());
            const auto mime = data.startsWith(QByteArrayView("\x89PNG\r\n\x1a\n",8)) ? "image/png"
                : data.startsWith(QByteArrayView("\xff\xd8\xff",3)) ? "image/jpeg" : nullptr;
            if (!mime) {
                error = QObject::tr("A material texture is not a PNG or JPEG.");
                return {};
            }
            views.append(QJsonObject{{"buffer",0},{"byteOffset",binary.size()},{"byteLength",data.size()}});
            binary.append(data.data(),data.size());
            binary.append((4-binary.size()%4)%4,'\0');
            images.append(QJsonObject{{"bufferView",int(views.size()-1)},{"mimeType",mime}});
            textures.append(QJsonObject{{"source",int(images.size()-1)}});
            auto &destination = i == 0 || i == 2 ? pbr : material;
            destination.insert(fields[i],QJsonObject{{"index",int(textures.size()-1)}});
        }
        material.insert("pbrMetallicRoughness",pbr);
        materials.append(material);
    }
    for (const auto &[slot,part] : primitives) {
        QJsonObject attributes;
        for (const auto &[key,offset,components] : {std::tuple{"POSITION",0,3},std::tuple{"NORMAL",3,3},std::tuple{"TEXCOORD_0",6,2}}) {
            const auto start = binary.size();
            std::array<float,3> low{part.vertices.front()[0],part.vertices.front()[1],part.vertices.front()[2]}, high = low;
            for (const auto &vertex : part.vertices)
                for (int i = 0; i < components; ++i) {
                    append(binary,std::bit_cast<quint32>(vertex[offset+i]));
                    if (offset == 0) {
                        low[i] = std::min(low[i],vertex[i]);
                        high[i] = std::max(high[i],vertex[i]);
                    }
                }
            QJsonObject accessor{{"bufferView",int(views.size())},{"componentType",5126},
                {"count",int(part.vertices.size())},{"type",components == 3 ? "VEC3" : "VEC2"}};
            if (offset == 0) {
                accessor.insert("min",QJsonArray{low[0],low[1],low[2]});
                accessor.insert("max",QJsonArray{high[0],high[1],high[2]});
            }
            attributes.insert(key,int(accessors.size()));
            views.append(QJsonObject{{"buffer",0},{"byteOffset",start},{"byteLength",binary.size()-start},{"target",34962}});
            accessors.append(accessor);
        }
        const auto indexStart = binary.size();
        for (const auto index : part.triangles)
            append(binary,index);
        accessors.append(QJsonObject{{"bufferView",int(views.size())},{"componentType",5125},
            {"count",int(part.triangles.size())},{"type","SCALAR"}});
        views.append(QJsonObject{{"buffer",0},{"byteOffset",indexStart},{"byteLength",binary.size()-indexStart},{"target",34963}});
        entries.append(QJsonObject{{"attributes",attributes},{"indices",int(accessors.size()-1)},{"material",int(slot)},{"mode",4}});
    }
    QJsonObject meshEntry{{"name",name},{"primitives",entries}};
    if (studioTopology) {
        QJsonArray positions, faces;
        for (const auto &p : mesh.vertices)
            positions.append(QJsonArray{p[0],p[1],p[2]});
        for (const auto &face : mesh.faces) {
            QJsonArray corners;
            for (const auto &corner : face.corners)
                corners.append(QJsonArray{int(corner.vertex),corner.uv[0],corner.uv[1],
                    corner.normal[0],corner.normal[1],corner.normal[2]});
            faces.append(QJsonArray{int(face.material),corners});
        }
        QJsonArray layers;
        for (auto it = uvLayers.other.cbegin(); it != uvLayers.other.cend(); ++it) {
            QJsonArray coordinates;
            for (const auto &uv : it.value())
                coordinates.append(QJsonArray{uv[0],uv[1]});
            layers.append(QJsonObject{{"name",it.key()},{"coordinates",coordinates}});
        }
        const auto topology = QJsonDocument(QJsonObject{{"vertices",positions},{"faces",faces},
            {"activeUv",uvLayers.active},{"uvLayers",layers}}).toJson(QJsonDocument::Compact);
        meshEntry.insert("extras",QJsonObject{{"worldforgeTopology",QString::fromUtf8(topology)}});
    }
    QJsonObject document{{"asset",QJsonObject{{"version","2.0"},{"generator","WorldForge"}}},{"scene",0},
        {"scenes",QJsonArray{QJsonObject{{"nodes",QJsonArray{0}}}}},
        {"nodes",QJsonArray{QJsonObject{{"name",name},{"mesh",0}}}},
        {"meshes",QJsonArray{meshEntry}},
        {"materials",materials},{"accessors",accessors},{"bufferViews",views},
        {"buffers",QJsonArray{QJsonObject{{"byteLength",binary.size()}}}}};
    if (!images.isEmpty()) {
        document.insert("images",images);
        document.insert("textures",textures);
    }
    auto json = QJsonDocument(document).toJson(QJsonDocument::Compact);
    json.append((4-json.size()%4)%4,' ');
    const auto total = 28+json.size()+binary.size();
    if (total > maxBytes) {
        error = QObject::tr("The GLB file would exceed 64 MiB.");
        return {};
    }
    QByteArray output;
    output.reserve(total);
    for (const auto value : {0x46546C67u,2u,quint32(total),quint32(json.size()),0x4E4F534Au})
        append(output,value);
    output += json;
    append(output,quint32(binary.size()));
    append(output,0x004E4942u);
    output += binary;
    return output;
}
