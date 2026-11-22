#include "Player/ReferenceScene.h"

#include "Engine/Render/Atmosphere.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace worldforge::player {
namespace {

void write(std::ostream &json,float value)
{
    json << (std::isfinite(value) ? value : 0.0f);
}

void write(std::ostream &json,std::initializer_list<float> values)
{
    json << '[';
    bool first = true;
    for (const float value : values) {
        if (!first)
            json << ',';
        write(json,value);
        first = false;
    }
    json << ']';
}

// The physical sky as Blender's equirectangular environment (u = 0.5 - atan2(y, x) / 2pi, v = elevation / pi + 0.5,
// first row at the top), in the Radiance RGBE format Cycles reads.
bool writeSky(const std::filesystem::path &file,const View &view)
{
    constexpr int width = 2048, height = 1024; // fine enough for a horizon as sharp as the engine's
    constexpr float pi = 3.14159265358979f;
    const std::array towardSun{view.sun.x,view.sun.y,view.sun.z};
    // The atmosphere is built from the authored sun (with the sun overhead), as the Player built it.
    const atmosphere::Sky sky(towardSun,view.authoredSun);
    std::ofstream out(file,std::ios::binary);
    out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << height << " +X " << width << "\n";
    std::vector<unsigned char> row(std::size_t(width)*4);
    for (int y = 0; y < height; ++y) {
        const float elevation = (0.5f-(float(y)+0.5f)/height)*pi;
        for (int x = 0; x < width; ++x) {
            const float azimuth = (0.5f-(float(x)+0.5f)/width)*2*pi;
            const float direction[3]{std::cos(azimuth)*std::cos(elevation),std::sin(azimuth)*std::cos(elevation),
                                     std::sin(elevation)};
            const auto radiance = sky.radiance(direction);
            const float largest = std::max({radiance[0],radiance[1],radiance[2]});
            auto *texel = &row[std::size_t(x)*4];
            if (!(largest > 1e-32f)) {
                texel[0] = texel[1] = texel[2] = texel[3] = 0;
                continue;
            }
            int exponent = 0;
            const float scale = std::frexp(largest,&exponent)*256.0f/largest;
            for (int c = 0; c < 3; ++c)
                texel[c] = static_cast<unsigned char>(std::clamp(radiance[std::size_t(c)]*scale,0.0f,255.0f));
            texel[3] = static_cast<unsigned char>(exponent+128);
        }
        out.write(reinterpret_cast<const char *>(row.data()),std::streamsize(row.size()));
    }
    return bool(out);
}

} // namespace

bool writeReference(const std::filesystem::path &folder,const std::vector<PlayVertex> &vertices,
                    const std::vector<DrawRange> &ranges,const View &view,const ReferenceCamera &camera,bool sky,
                    float time,std::string &error)
{
    std::error_code created;
    std::filesystem::create_directories(folder,created);
    std::ofstream binary(folder / "scene.bin",std::ios::binary);
    if (!binary) {
        error = "Could not write scene.bin.";
        return false;
    }
    // Per vertex: position, normal, linear color, uv. Wind sway matches PlayScene.vert at this time.
    std::vector<float> packed;
    packed.reserve(vertices.size()*11);
    for (const auto &v : vertices) {
        auto p = v.position;
        const float phase = p[0]*0.36f+p[1]*0.29f+time*1.7f;
        p[0] += std::sin(phase)*v.wind;
        p[1] += 0.6f*std::cos(phase*0.83f)*v.wind;
        packed.insert(packed.end(),{p[0],p[1],p[2],v.normal[0],v.normal[1],v.normal[2],
            color::srgbToLinear(v.color[0]),color::srgbToLinear(v.color[1]),color::srgbToLinear(v.color[2]),v.uv[0],v.uv[1]});
    }
    binary.write(reinterpret_cast<const char *>(packed.data()),std::streamsize(packed.size()*sizeof(float)));
    if (!binary) {
        error = "Could not write scene.bin.";
        return false;
    }

    std::ostringstream json;
    json << std::setprecision(9);
    json << "{\n  \"format\": 1,\n  \"vertices\": " << vertices.size() << ",\n  \"camera\": {\"eye\": ";
    write(json,{camera.eye[0],camera.eye[1],camera.eye[2]});
    json << ", \"target\": ";
    write(json,{camera.target[0],camera.target[1],camera.target[2]});
    json << ", \"verticalFov\": ";
    write(json,camera.verticalFov);
    json << ", \"width\": " << camera.width << ", \"height\": " << camera.height << ", \"near\": 0.05, \"far\": 1800},\n";
    const bool physical = view.sky.w > 0.5f;
    if (physical && !writeSky(folder / "sky.hdr",view)) {
        error = "Could not write sky.hdr.";
        return false;
    }
    json << "  \"sky\": {\"visible\": " << (sky ? "true" : "false") << ", \"physical\": " << (physical ? "true" : "false");
    if (physical)
        json << ", \"map\": \"sky.hdr\"";
    json << ", \"horizon\": ";
    write(json,{view.horizon.x,view.horizon.y,view.horizon.z});
    json << ", \"zenith\": ";
    write(json,{view.zenith.x,view.zenith.y,view.zenith.z});
    json << "},\n  \"sun\": {\"direction\": ";
    write(json,{view.sun.x,view.sun.y,view.sun.z});
    json << ", \"intensity\": ";
    write(json,view.sun.w);
    json << ", \"color\": ";
    write(json,{view.sunTint.x,view.sunTint.y,view.sunTint.z});
    json << ", \"shadows\": " << (view.lighting.y > 0.5f ? "true" : "false") << "},\n  \"lights\": [";
    for (int i = 0; i < int(view.eye.w); ++i) {
        const auto &light = view.lights[i];
        json << (i ? ",\n    " : "\n    ") << "{\"directional\": " << (light.w < 0 ? "true" : "false") << ", \"vector\": ";
        write(json,{light.x,light.y,light.z});
        json << ", \"intensity\": ";
        write(json,std::abs(light.w));
        json << '}';
    }
    json << "],\n  \"display\": {\"exposure\": ";
    write(json,std::log2(std::max(1e-6f,view.display.x)));
    json << ", \"viewTransform\": \"" << (view.display.y > 0.5f ? "AgX" : "Standard") << "\"},\n  \"ranges\": [";
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto &range = ranges[i];
        const auto &material = range.material;
        json << (i ? ",\n    " : "\n    ") << "{\"start\": " << range.start << ", \"count\": " << range.count
             << ", \"textures\": [";
        for (std::size_t t = 0; t < material.textures.size(); ++t)
            json << (t ? "," : "") << material.textures[t];
        json << "], \"emissive\": ";
        write(json,{material.emissive[0],material.emissive[1],material.emissive[2]});
        json << ", \"metallic\": ";
        write(json,material.metallic);
        json << ", \"roughness\": ";
        write(json,material.roughness);
        json << ", \"advanced\": " << (material.advanced ? "true" : "false") << '}';
    }
    json << "]\n}\n";
    std::ofstream description(folder / "scene.json",std::ios::binary);
    description << json.str();
    if (!description) {
        error = "Could not write scene.json.";
        return false;
    }
    return true;
}

}
