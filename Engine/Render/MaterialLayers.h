#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace worldforge {

enum LayerMap : std::size_t {
    CoatWeight, CoatRoughness, CoatNormal, SheenColor, SheenRoughness, Transmission, Anisotropy, LayerMapCount
};
inline constexpr std::size_t baseMaterialTextureCount = 5;
inline constexpr std::size_t materialTextureCount = baseMaterialTextureCount+LayerMapCount;

struct MaterialLayers {
    std::array<float,3> sheenColor{}; // linear RGB reflectance
    float sheenRoughness = 0;
    float transmission = 0;
    float ior = 1.5f;
    float subsurface = 0;
    float thickness = 0.02f, scatteringDistance = 0.01f; // meters
    float anisotropy = 0, anisotropyRotation = 0; // rotation in radians
    float coatNormalScale = 1;
    float wetness = 0, puddles = 0, heightScale = 0; // height is measured in UV units
    bool decal = false;
    float decalOpacity = 1;
    static constexpr float maxCoatNormalScale = 16;
    static constexpr float maxHeightScale = 0.2f;
    static constexpr float fullTurn = 6.28318530718f;

    bool operator==(const MaterialLayers &) const = default;

    bool valid() const
    {
        return std::ranges::all_of(sheenColor,unit) && unit(sheenRoughness) && unit(transmission)
            && ior >= 1 && ior <= 3 && unit(subsurface)
            && thickness >= 0 && thickness <= 2 && scatteringDistance >= 0.0001f && scatteringDistance <= 2
            && unit(anisotropy) && anisotropyRotation >= -fullTurn && anisotropyRotation <= fullTurn
            && coatNormalScale >= -maxCoatNormalScale && coatNormalScale <= maxCoatNormalScale
            && unit(wetness) && unit(puddles) && heightScale >= 0 && heightScale <= maxHeightScale && unit(decalOpacity);
    }

    bool hasOptics() const { return transmission != 0 || ior != 1.5f; }
    bool isTransparent() const { return transmission > 0 || decal; }
    bool hasStreetLayers() const { return wetness != 0 || puddles != 0 || heightScale != 0 || decal || decalOpacity != 1; }
    bool hasScatteringOrAnisotropy() const
    {
        return subsurface != 0 || thickness != 0.02f || scatteringDistance != 0.01f || anisotropy != 0 || anisotropyRotation != 0;
    }

private:
    static bool unit(float value) { return value >= 0 && value <= 1; }
};

}
