#pragma once

#include "Player/PlayerRender.h"

#include <filesystem>
#include <string>

namespace worldforge::player {

struct ReferenceCamera {
    Point3 eye, target;
    float verticalFov = 0;
    UINT width = 0, height = 0;
};

// Writes one Player frame for the Cycles reference render (Tests/CyclesReference.py): scene.json with
// the camera, lights, sky, display settings and draw ranges, and scene.bin with the vertices as they
// are drawn (world space, wind sway applied, colors in linear light). Texture n of the ranges is
// expected beside them as texture-n.png.
bool writeReference(const std::filesystem::path &folder,const std::vector<PlayVertex> &vertices,
                    const std::vector<DrawRange> &ranges,const View &view,const ReferenceCamera &camera,bool sky,
                    float time,std::string &error);

}
