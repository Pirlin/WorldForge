#include "Player/ReferenceScene.h"

#include <algorithm>
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
    json << "  \"sky\": {\"visible\": " << (sky ? "true" : "false") << ", \"horizon\": ";
    write(json,{view.horizon.x,view.horizon.y,view.horizon.z});
    json << ", \"zenith\": ";
    write(json,{view.zenith.x,view.zenith.y,view.zenith.z});
    json << "},\n  \"sun\": {\"direction\": ";
    write(json,{view.sun.x,view.sun.y,view.sun.z});
    json << ", \"intensity\": ";
    write(json,view.sun.w);
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
