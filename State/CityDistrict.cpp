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
    std::uint32_t next() { uniform(); return state_; }

private:
    std::uint32_t state_;
};

std::vector<std::uint8_t> facadeTexture(int style,const std::array<float,4> &color,int size = 512)
{
    const int bay = size/4;
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
            // Measures in pixels of a 128-pixel bay, scaled to this size (512 pixels gives the original maps).
            const auto px = [bay](int pixels) { return std::max(1,pixels*bay/128); };
            const int inset = px(style == 3 ? 10 : 24);
            const bool window = horizontal >= inset && horizontal < bay-inset && vertical >= px(20) && vertical < px(102);
            if (window) {
                const bool trim = horizontal < inset+px(4) || horizontal >= bay-inset-px(4) || std::abs(horizontal-bay/2) <= px(2)
                    || vertical < px(24) || vertical >= px(98);
                const float reflection = 0.84f+0.003f*float(vertical)*128.0f/float(bay)+0.025f*float((x/bay+3*(y/bay)+style)%4);
                row[x] = trim ? shade(frame,0.82f) : shade(glass,reflection);
                continue;
            }
            const int brick = px(16);
            const bool joint = style == 1 ? y%brick < px(2) || (x+((y/brick)%2)*brick)%(2*brick) < px(2) : vertical < px(3) || horizontal < px(2);
            const int panel = (x/brick+11*(y/brick)+7*style)%9;
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

// Adds an axis-aligned box to mesh: its six faces in material, each mapped 0 to 1 in uv.
void appendBox(wf::Mesh &mesh,wf::Point3 center,wf::Point3 size,std::uint32_t material)
{
    const auto first = wf::VertexIndex(mesh.vertices.size());
    for (int corner = 0; corner < 8; ++corner)
        mesh.vertices.push_back({center[0]+size[0]*((corner&1) ? 0.5f : -0.5f),center[1]+size[1]*((corner&2) ? 0.5f : -0.5f),
                                 center[2]+size[2]*((corner&4) ? 0.5f : -0.5f)});
    // Corner bits: x 1, y 2, z 4; each face wound counterclockwise seen from outside.
    constexpr std::array<std::array<int,4>,6> faces{{{0,2,3,1},{4,5,7,6},{0,1,5,4},{1,3,7,5},{3,2,6,7},{2,0,4,6}}};
    constexpr std::array<wf::Point2,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
    for (const auto &indices : faces) {
        wf::MeshFace face;
        face.material = material;
        for (std::size_t i = 0; i < indices.size(); ++i)
            face.corners.push_back({first+wf::VertexIndex(indices[i]),uv[i]});
        mesh.faces.push_back(std::move(face));
    }
}

// A building with detail, in meters around its center: the body (material 1, its facade texture), a ledge around every
// floor and pilasters up each side (material 0), a roof slab and rooftop units. About 1,000-2,500 vertices once drawn.
wf::Mesh detailedBuilding(float width,float depth,float height,std::uint32_t seed)
{
    wf::Mesh mesh;
    mesh.materials.resize(2);
    appendBox(mesh,{0,0,0},{width,depth,height},1);
    const float bottom = -height/2;
    const int floors = std::clamp(int(height/3.5f),1,12);
    for (int floor = 1; floor <= floors; ++floor) {
        const float z = bottom+height*float(floor)/float(floors+1);
        appendBox(mesh,{0,depth/2+0.08f,z},{width+0.3f,0.16f,0.22f},0);
        appendBox(mesh,{0,-depth/2-0.08f,z},{width+0.3f,0.16f,0.22f},0);
        appendBox(mesh,{width/2+0.08f,0,z},{0.16f,depth,0.22f},0);
        appendBox(mesh,{-width/2-0.08f,0,z},{0.16f,depth,0.22f},0);
    }
    const auto pilasters = [&](float length,bool alongX) {
        const int count = std::clamp(int(length/4),1,5);
        for (int i = 0; i <= count; ++i) {
            const float at = -length/2+length*float(i)/float(count);
            for (const float side : {-1.0f,1.0f}) {
                const wf::Point3 center = alongX ? wf::Point3{at,side*(depth/2+0.1f),0} : wf::Point3{side*(width/2+0.1f),at,0};
                appendBox(mesh,center,alongX ? wf::Point3{0.35f,0.2f,height} : wf::Point3{0.2f,0.35f,height},0);
            }
        }
    };
    pilasters(width,true);
    pilasters(depth,false);
    appendBox(mesh,{0,0,height/2+0.15f},{width*0.96f,depth*0.96f,0.3f},0);
    // Rooftop units: a few boxes placed by the seed.
    std::uint32_t state = seed*747796405u+2891336453u;
    const auto next = [&] { state = state*747796405u+2891336453u; return float((state >> 8) & 0xffff)/65535.0f; };
    for (int unit = 0; unit < 3; ++unit) {
        const float w = 1.2f+2.0f*next(), d = 1.2f+2.0f*next(), h = 0.8f+1.6f*next();
        appendBox(mesh,{(next()-0.5f)*(width-w-1),(next()-0.5f)*(depth-d-1),height/2+0.3f+h/2},{w,d,h},0);
    }
    return mesh;
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
                    const auto style = std::size_t(random.uniform()*float(facades.size()))%facades.size();
                    const auto &facade = facades[style].first;
                    const auto name = QString("%1 %2").arg(block).arg(ly*layout.lotsPerSide+lx+1);
                    if (layout.detailed) {
                        // Its own mesh at its real size and its own facade: the style's color, varied per building.
                        auto mesh = detailedBuilding(width,depth,height,random.next());
                        auto color = facades[style].second;
                        for (int c = 0; c < 3; ++c)
                            color[std::size_t(c)] = std::clamp(color[std::size_t(c)]*random.between(0.85f,1.12f),0.0f,1.0f);
                        mesh.materials[0].name = "Trim";
                        mesh.materials[0].color = {color[0]*0.7f,color[1]*0.7f,color[2]*0.7f,1};
                        mesh.materials[1].name = "Facade";
                        mesh.materials[1].color = {1,1,1,1};
                        mesh.materials[1].roughness = style == 3 ? 0.25f : 0.7f;
                        mesh.materials[1].baseColorTexture = facadeTexture(int(style),color,128);
                        MeshAsset created{id(),tr("Building %1").arg(name),{},{},false,QSharedPointer<wf::Mesh>::create(std::move(mesh)),{}};
                        SceneObject object;
                        object.id = id();
                        object.name = tr("Building %1").arg(name);
                        object.type = "Static Mesh";
                        object.group = tr("Buildings");
                        object.position = lotCenter+QVector3D(0,0,0.2f+height/2);
                        object.scale = {1,1,1};
                        object.meshAsset = created.id;
                        project.assets.insert(created.id,std::move(created));
                        project.objects.append(object);
                        continue;
                    }
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
