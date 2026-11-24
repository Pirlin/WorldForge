#include "State/CityDistrict.h"

#include "Engine/Geometry/Mesh.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QUuid>

#include <algorithm>
#include <array>

namespace wf = worldforge;

namespace district {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("CityDistrict",text);
}

QString id()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// A small deterministic generator, so a layout and seed always give the same district.
class Random {
public:
    explicit Random(std::uint32_t seed) : state_(seed ? seed : 1) {}
    float uniform()
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return float(state_ & 0xffffff)/float(0x1000000);
    }
    float between(float low,float high) { return low+(high-low)*uniform(); }

private:
    std::uint32_t state_;
};

std::vector<std::uint8_t> facadeTexture(int style,const std::array<float,4> &color)
{
    constexpr int size = 512, bay = 128;
    QImage image(size,size,QImage::Format_RGB32);
    const auto shade = [](const std::array<float,4> &base,float light) {
        const auto channel = [&](float value) { return int(std::clamp(value*light,0.0f,1.0f)*255+0.5f); };
        return qRgb(channel(base[0]),channel(base[1]),channel(base[2]));
    };
    constexpr std::array glass{0.12f,0.20f,0.28f,1.0f};
    constexpr std::array frame{0.20f,0.22f,0.23f,1.0f};
    for (int y = 0; y < size; ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        const int vertical = y%bay;
        for (int x = 0; x < size; ++x) {
            const int horizontal = x%bay;
            const int inset = style == 3 ? 10 : 24;
            const bool window = horizontal >= inset && horizontal < bay-inset && vertical >= 20 && vertical < 102;
            if (window) {
                const bool trim = horizontal < inset+4 || horizontal >= bay-inset-4 || std::abs(horizontal-bay/2) <= 2
                    || vertical < 24 || vertical >= 98;
                const float reflection = 0.84f+0.003f*float(vertical)+0.025f*float((x/bay+3*(y/bay)+style)%4);
                row[x] = trim ? shade(frame,0.82f) : shade(glass,reflection);
                continue;
            }
            const bool joint = style == 1 ? y%16 < 2 || (x+((y/16)%2)*16)%32 < 2 : vertical < 3 || horizontal < 2;
            const int panel = (x/16+11*(y/16)+7*style)%9;
            row[x] = shade(color,joint ? 0.66f : 0.91f+0.013f*float(panel));
        }
    }
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer,"PNG"))
        return {};
    const auto *begin = reinterpret_cast<const std::uint8_t *>(png.constData());
    return {begin,begin+png.size()};
}

}

void addStarter(Project &project,Layout layout)
{
    layout.blocks = std::clamp(layout.blocks,1,maximumBlocks);
    layout.lotsPerSide = std::clamp(layout.lotsPerSide,1,3);
    layout.blockSize = std::clamp(layout.blockSize,12.0f,120.0f);
    layout.roadWidth = std::clamp(layout.roadWidth,4.0f,30.0f);
    QHash<QString,QString> assets;
    const auto asset = [&](const QString &name,std::array<float,4> color,float roughness = 0.65f,int facade = -1) {
        auto mesh = wf::makeCube();
        auto &surface = mesh.materials.front();
        surface.color = color;
        surface.roughness = roughness;
        surface.name = name.toStdString();
        if (layout.texturedFacades && facade >= 0) {
            auto exterior = surface;
            exterior.baseColorTexture = facadeTexture(facade,color);
            if (!exterior.baseColorTexture.empty()) {
                exterior.color = {1,1,1,1};
                mesh.materials.push_back(std::move(exterior));
                constexpr std::array<wf::Point2,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
                for (auto &face : std::span(mesh.faces).subspan(2)) {
                    face.material = 1;
                    for (std::size_t corner = 0; corner < face.corners.size(); ++corner)
                        face.corners[corner].uv = uv[corner];
                }
            }
        }
        MeshAsset created{id(),name,{},{},false,QSharedPointer<wf::Mesh>::create(std::move(mesh)),{}};
        assets.insert(name,created.id);
        project.assets.insert(created.id,std::move(created));
    };
    asset(tr("Asphalt"),{0.16f,0.17f,0.19f,1});
    asset(tr("Sidewalk"),{0.56f,0.57f,0.58f,1});
    asset(tr("Lamp Post"),{0.22f,0.23f,0.25f,1},0.4f);
    asset(tr("Roof"),{0.30f,0.31f,0.34f,1});
    const std::array facades{
        std::pair{tr("Concrete Facade"),std::array{0.62f,0.61f,0.58f,1.0f}},
        std::pair{tr("Brick Facade"),std::array{0.58f,0.33f,0.25f,1.0f}},
        std::pair{tr("Sandstone Facade"),std::array{0.74f,0.64f,0.48f,1.0f}},
        std::pair{tr("Dark Glass Facade"),std::array{0.18f,0.22f,0.27f,1.0f}},
        std::pair{tr("Painted Facade"),std::array{0.66f,0.70f,0.72f,1.0f}}};
    for (std::size_t style = 0; style < facades.size(); ++style) {
        const auto &[name,color] = facades[style];
        asset(name,color,style == 3 ? 0.25f : 0.7f,int(style));
    }

    const auto add = [&](const QString &name,const QString &type,const QString &assetName,const QString &group,
                         QVector3D position,QVector3D scale,QJsonObject details = {}) {
        SceneObject object;
        object.id = id();
        object.name = name;
        object.type = type;
        object.group = group;
        object.position = position;
        object.scale = scale;
        object.details = std::move(details);
        object.meshAsset = assets.value(assetName);
        project.objects.append(object);
    };

    const float extent = layout.extent();
    const float half = extent/2;
    // One asphalt ground under the whole district carries the roads and the only collider.
    add(tr("District Ground"),"Static Mesh",tr("Asphalt"),tr("Roads"),{0,0,-0.25f},{extent,extent,0.5f},
        {{"physics","Static"}});
    Random random(layout.seed);
    const float pitch = layout.blockSize+layout.roadWidth;
    const float lot = layout.blockSize/float(layout.lotsPerSide);
    for (int by = 0; by < layout.blocks; ++by)
        for (int bx = 0; bx < layout.blocks; ++bx) {
            const QVector3D center{-half+layout.roadWidth+layout.blockSize/2+float(bx)*pitch,
                                   -half+layout.roadWidth+layout.blockSize/2+float(by)*pitch,0};
            const auto block = QString("%1-%2").arg(bx+1).arg(by+1);
            add(tr("Sidewalk %1").arg(block),"Static Mesh",tr("Sidewalk"),tr("Sidewalks"),center+QVector3D(0,0,0.1f),
                {layout.blockSize,layout.blockSize,0.2f});
            // Taller towers toward the district's center, low buildings at its edge.
            const float centrality = 1-std::max(std::abs(center.x()),std::abs(center.y()))/half;
            for (int ly = 0; ly < layout.lotsPerSide; ++ly)
                for (int lx = 0; lx < layout.lotsPerSide; ++lx) {
                    const QVector3D lotCenter{center.x()-layout.blockSize/2+lot*(float(lx)+0.5f),
                                              center.y()-layout.blockSize/2+lot*(float(ly)+0.5f),0};
                    const float width = lot*random.between(0.62f,0.84f), depth = lot*random.between(0.62f,0.84f);
                    const float height = random.between(6,14)+random.between(0,46)*centrality*centrality;
                    const auto &facade = facades[std::size_t(random.uniform()*float(facades.size()))%facades.size()].first;
                    const auto name = QString("%1 %2").arg(block).arg(ly*layout.lotsPerSide+lx+1);
                    add(tr("Building %1").arg(name),"Static Mesh",facade,tr("Buildings"),
                        lotCenter+QVector3D(0,0,0.2f+height/2),{width,depth,height});
                    add(tr("Roof %1").arg(name),"Static Mesh",tr("Roof"),tr("Buildings"),
                        lotCenter+QVector3D(0,0,0.2f+height+0.15f),{width*0.96f,depth*0.96f,0.3f});
                }
        }
    // A lamp at every intersection: a post and a point light above it.
    for (int iy = 0; iy <= layout.blocks; ++iy)
        for (int ix = 0; ix <= layout.blocks; ++ix) {
            const QVector3D corner{-half+layout.roadWidth/2+float(ix)*pitch,-half+layout.roadWidth/2+float(iy)*pitch,0};
            const auto name = QString("%1-%2").arg(ix+1).arg(iy+1);
            add(tr("Lamp Post %1").arg(name),"Static Mesh",tr("Lamp Post"),tr("Street Lamps"),corner+QVector3D(0,0,2.6f),
                {0.18f,0.18f,5.2f});
            SceneObject light;
            light.id = id();
            light.name = tr("Street Lamp %1").arg(name);
            light.type = "Point Light";
            light.group = tr("Street Lamps");
            light.position = corner+QVector3D(0,0,5.6f);
            light.details = {{"luminousIntensity",1800}};
            project.objects.append(light);
        }
    SceneObject start;
    start.id = id();
    start.name = tr("Player Start");
    start.type = "Player Start";
    start.group = tr("Gameplay");
    start.position = {-half+layout.roadWidth/2,-half+layout.roadWidth/2+pitch/2,1};
    project.objects.append(start);
}

}
