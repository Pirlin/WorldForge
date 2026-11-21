#pragma once

#include "Player/PlayerRender.h"

#include <memory>
#include <string>

namespace worldforge::player {

// Creates the Vulkan renderer for window. Returns null, with the reason in error, when the
// computer has no usable Vulkan driver or device.
std::unique_ptr<Renderer> createVulkanRenderer(HWND window,std::wstring &error);

} // namespace worldforge::player
