#pragma once

#include "State/ProjectStore.h"

#include <cstdint>

// A generated city district: a grid of blocks with sidewalks and buildings of varied height, roads between them, and a
// street lamp at every intersection. It is the repeatable test scene for city-scale rendering (R4 in
// Docs/WorldForge-Strategy-and-Roadmap.md): the same layout and seed always give the same district.
namespace district {

struct Layout {
    int blocks = 6;           // blocks along each axis
    float blockSize = 40;     // meters, sidewalk included
    float roadWidth = 10;     // meters
    int lotsPerSide = 2;      // building lots along each side of a block
    std::uint32_t seed = 7;
    bool texturedFacades = false;
    // Each building its own mesh with ledges, pilasters and rooftop units and its own 128-pixel facade texture: dense
    // content for world partitions (R4.6). Implies textured facades.
    bool detailed = false;
    // The width of the whole district in meters.
    float extent() const { return float(blocks)*blockSize+float(blocks+1)*roadWidth; }
};

// The largest layout that stays within the playable format's limits (4096 meshes, 256 lights).
inline constexpr int maximumBlocks = 14;

// Adds the district's assets and objects to a new project. Blocks are clamped to 1..maximumBlocks.
void addStarter(Project &project,Layout layout = {});

}
