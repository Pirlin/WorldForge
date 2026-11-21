#pragma once

#include "Engine/Geometry/Mesh.h"

#include <algorithm>
#include <cmath>

namespace worldforge {

struct MeshClip {
    static constexpr std::size_t maxSamples = 500'000;
    static constexpr std::size_t maxFrames = 120;
    struct Vertex {
        Point3 position, normal;
    };
    struct Sample {
        std::size_t first, second;
        float blend;
    };
    std::string name;
    float framesPerSecond = 24;
    std::size_t vertexCount = 0;
    std::vector<Vertex> samples;

    std::size_t frameCount() const { return vertexCount ? samples.size()/vertexCount : 0; }

    bool valid(std::size_t vertices) const {
        if (name.empty() || name.size() > 128 || !vertices || vertexCount != vertices
            || !(framesPerSecond >= 1 && framesPerSecond <= 120) || samples.size() > maxSamples
            || frameCount() < 2 || frameCount() > maxFrames || samples.size()%vertices)
            return false;
        return std::ranges::all_of(samples,[](const Vertex &vertex) {
            const auto bounded = [](float value) { return std::isfinite(value) && std::abs(value) <= 1'000'000; };
            const auto normalized = [](float value) { return std::isfinite(value) && std::abs(value) <= 1.01f; };
            return std::ranges::all_of(vertex.position,bounded) && std::ranges::all_of(vertex.normal,normalized);
        });
    }

    double duration() const { return double(frameCount()-1)/framesPerSecond; }

    Sample phase(double seconds) const {
        const double frame = std::fmod(std::max(0.0,seconds)*framesPerSecond,double(frameCount()-1));
        const auto first = std::size_t(frame);
        return {first,first+1,float(frame-first)};
    }

    Vertex vertex(const Sample &phase,std::size_t index) const {
        const auto &a = samples[phase.first*vertexCount+index];
        const auto &b = samples[phase.second*vertexCount+index];
        Vertex result;
        for (int i = 0; i < 3; ++i) {
            result.position[i] = std::lerp(a.position[i],b.position[i],phase.blend);
            result.normal[i] = std::lerp(a.normal[i],b.normal[i],phase.blend);
        }
        return result;
    }
};

}
