#pragma once

// LDR-FLIP image difference (Andersson et al., "FLIP: A Difference Evaluator for Alternating Images",
// 2020), following NVIDIA's BSD-licensed reference implementation. It models how different two
// display images look to a viewer: 0 means indistinguishable, 1 the largest difference.

#include <cstdint>
#include <span>
#include <vector>

namespace worldforge::flip {

// Pixels per degree of a 0.7 m wide 4K monitor seen from 0.7 m, FLIP's default viewing condition.
float defaultPixelsPerDegree();

struct Result {
    std::vector<float> error; // per pixel, rows top to bottom
    double mean = 1;
};

// Compares two sRGB images of width x height pixels, given as packed RGB bytes.
Result compare(std::span<const std::uint8_t> reference,std::span<const std::uint8_t> test,int width,int height,
               float pixelsPerDegree = defaultPixelsPerDegree());

// The color of error value e (0 to 1) on FLIP's magma-like heat map, as RGB bytes.
void heat(float e,std::uint8_t rgb[3]);

}
