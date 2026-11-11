#include "State/GlbImport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QSet>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <limits>

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
    Importer(QJsonObject json, QByteArrayView bytes) : json_(std::move(json)), bin_(bytes) {}

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
            mesh_.materials.push_back({material.value("name").toString("Material").toStdString(),
                vector<4>(pbr.value("baseColorFactor"), {1,1,1,1}),
                float(pbr.value("metallicFactor").toDouble(1)), float(pbr.value("roughnessFactor").toDouble(1))});
        }
        const auto scene = entry(json_.value("scenes").toArray(), integer(json_.value("scene"), 0));
        for (const auto &node : scene.value("nodes").toArray())
            visit(integer(node), QMatrix4x4(), 0);
        const auto error = wf::validateMesh(mesh_);
        require(error.empty(), QString::fromStdString(error));
        return std::move(mesh_);
    }

private:
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
        std::optional<Accessor> normals, uvs, indices;
        if (attributes.contains("NORMAL")) {
            normals = accessor(integer(attributes.value("NORMAL")), "VEC3", 3);
            require(normals->type == 5126 && !normals->normalized && normals->count == positions.count,
                    QStringLiteral("GLB normals must match the position count."));
        }
        if (attributes.contains("TEXCOORD_0")) {
            uvs = accessor(integer(attributes.value("TEXCOORD_0")), "VEC2", 2);
            require(uvs->count == positions.count && ((uvs->type == 5126 && !uvs->normalized)
                    || (uvs->normalized && (uvs->type == 5121 || uvs->type == 5123))),
                    QStringLiteral("GLB UVs have an invalid type or count."));
        }
        if (primitive.contains("indices")) {
            indices = accessor(integer(primitive.value("indices")), "SCALAR", 1);
            require(indices->type != 5126 && !indices->normalized, QStringLiteral("GLB indices must be unsigned integers."));
        }
        const int count = indices ? indices->count : positions.count;
        require(count % 3 == 0 && mesh_.faces.size() + count/3 <= limit,
                QStringLiteral("GLB index count does not describe complete triangles within the geometry limit."));
        const int material = integer(primitive.value("material")) + 1;
        require(material >= 0 && std::size_t(material) < mesh_.materials.size(), QStringLiteral("GLB material is missing."));
        const auto determinant = matrix.determinant();
        require(matrix.isAffine() && std::isfinite(determinant) && determinant != 0,
                QStringLiteral("GLB mesh node transform must be finite, affine, and invertible."));
        const auto normalMatrix = matrix.normalMatrix();
        const auto firstVertex = wf::VertexIndex(mesh_.vertices.size());
        for (int i = 0; i < positions.count; ++i)
            mesh_.vertices.push_back(zUp(matrix.map(QVector3D(positions.value(i, 0), positions.value(i, 1), positions.value(i, 2)))));
        for (int i = 0; i < count; i += 3) {
            wf::MeshFace face;
            face.material = std::uint32_t(material);
            for (int j = 0; j < 3; ++j) {
                const int offset = i + (determinant < 0 && j != 0 ? 3-j : j);
                const auto vertex = indices ? indices->index(offset) : quint32(offset);
                require(vertex < quint32(positions.count), QStringLiteral("GLB triangle index is out of bounds."));
                wf::MeshCorner corner{firstVertex + vertex};
                if (uvs)
                    corner.uv = {uvs->value(vertex, 0), 1.0f - uvs->value(vertex, 1)};
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
    wf::Mesh mesh_;
};
}

std::optional<worldforge::Mesh> importGlb(const QByteArray &bytes, QString &error)
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
        return Importer(json.object(), QByteArrayView(bytes.constData() + binaryHeader + 8, binarySize)).run();
    } catch (const QString &message) {
        error = message;
        return std::nullopt;
    }
}
