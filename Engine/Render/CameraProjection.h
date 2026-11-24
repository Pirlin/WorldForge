#pragma once

#include <numbers>

namespace worldforge::camera {

// The perspective lens shared by the Player and authored camera previews.
inline constexpr float verticalFovDegrees = 45.0f;
inline constexpr float verticalFovRadians = verticalFovDegrees*std::numbers::pi_v<float>/180.0f;
inline constexpr float nearPlane = 0.05f;
inline constexpr float farPlane = 1800.0f;

}
