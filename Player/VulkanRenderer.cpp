#include "Player/VulkanRenderer.h"

#include "Engine/Render/SkyVisibility.h"
#include "Engine/Render/Vulkan/HdrFrame.h"

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>

// SPIR-V generated from Engine/Render/Shaders at build time.
#include "Sky_vert.h"
#include "Sky_frag.h"
#include "Shadow_vert.h"
#include "Display_vert.h"
#include "Display_frag.h"
#include "Temporal_frag.h"
#include "Ambient_frag.h"
#include "BloomDown_frag.h"
#include "BloomUp_frag.h"
#include "DepthOfField_frag.h"
#include "MotionBlur_frag.h"
#include "PlayScene_vert.h"
#include "PlayScene_frag.h"

namespace worldforge::player {
namespace {
using namespace DirectX;

template<std::size_t N>
std::span<const uint32_t> spirv(const uint32_t (&words)[N]) { return {words,N}; }

// Uniform blocks, laid out as std140 in Engine/Render/Shaders.
struct LightingBlock {      // Lighting.glsl
    XMFLOAT4X4 cascadeMatrices[shadow::cascadeCount];
    XMFLOAT4 cascadeScales[shadow::cascadeCount];
    XMFLOAT4 fogCenterRadius, settings; // local fog density, shadows enabled, TAA frame index, ambient scale
    XMFLOAT4 sun, skyIrradiance[9], skyHorizon, skyZenith, skyGround;
    XMFLOAT4 clusterForward, clusterViewport; // clustered lights: the camera's forward axis; viewport size in pixels
    XMFLOAT4X4 skyMatrices[sky::directionCount];
    XMFLOAT4 skyDirections[sky::directionCount], skySettings; // settings: enabled, meters per texel
    XMFLOAT4 sunTint; // sunlight color, luminance 1
    // Screen-space reflections: the previous frame's clip space without jitter; on, the longest ray in meters.
    XMFLOAT4X4 previousViewProjection;
    XMFLOAT4 reflectionSettings;
};
struct PlayViewBlock {      // PlayView.glsl
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 eye, horizon, zenith, fog, sun, lights[16], time;
};
struct CameraConstants {    // push constants of Sky and Shadow shaders
    XMFLOAT4X4 matrix;
    XMFLOAT4 eye, fog, horizon, zenith;
};
struct MaterialConstants {  // push constants of PlayScene.frag
    XMFLOAT4 emissive, factors;
};
static_assert(sizeof(LightingBlock) == 1984 && sizeof(PlayViewBlock) == 416 && sizeof(CameraConstants) == 128);

constexpr uint32_t shadowSize = shadow::atlasSize;
constexpr uint32_t framesInFlight = 2;

// View holds Direct3D row-vector matrices. Their bytes read as GLSL column-major matrices are the
// transpose, so GLSL `matrix * v` equals HLSL `mul(v, matrix)`. Vulkan clip space has y down.
XMFLOAT4X4 vulkanClip(const XMFLOAT4X4 &matrix)
{
    XMFLOAT4X4 result;
    XMStoreFloat4x4(&result,XMLoadFloat4x4(&matrix)*XMMatrixScaling(1,-1,1));
    return result;
}

// shown (the view to render) with sun shadow cascades fitted to view (without TAA jitter, so they hold still) and
// to the scene's bounds, and the TAA frame index for the shadow filter.
View withShadows(const View &shown,const View &view,const std::vector<PlayVertex> &vertices,uint32_t sample)
{
    View result = shown;
    XMFLOAT4X4 inverse;
    const auto clip = vulkanClip(view.viewProjection);
    XMStoreFloat4x4(&inverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&clip)));
    shadow::Matrix toWorld;
    std::memcpy(toWorld.data(),&inverse,sizeof(inverse));
    std::optional<shadow::Bounds> bounds;
    if (!vertices.empty()) {
        shadow::Bounds box{vertices.front().position,vertices.front().position};
        for (const auto &vertex : vertices)
            for (int axis = 0; axis < 3; ++axis) {
                box.minimum[axis] = std::min(box.minimum[axis],vertex.position[axis]);
                box.maximum[axis] = std::max(box.maximum[axis],vertex.position[axis]);
            }
        bounds = box;
        result.skyVisibility = sky::fit(box);
        result.skyMaps = 1;
    }
    result.cascades = shadow::fit(toWorld,{view.sun.x,view.sun.y,view.sun.z},bounds);
    result.temporalSample = sample;
    return result;
}

// The view rendered with a sub-pixel TAA jitter, given in pixels.
View jittered(const View &view,std::array<float,2> pixels,UINT width,UINT height)
{
    View result = view;
    XMStoreFloat4x4(&result.viewProjection,XMLoadFloat4x4(&view.viewProjection)
        *XMMatrixTranslation(2*pixels[0]/float(std::max(width,1u)),2*pixels[1]/float(std::max(height,1u)),0));
    return result;
}

// This frame's clip space to the previous one's, as GLSL reads the bytes (see vulkanClip).
std::array<float,16> reprojection(const XMFLOAT4X4 &current,const XMFLOAT4X4 &previous)
{
    XMFLOAT4X4 matrix;
    XMStoreFloat4x4(&matrix,XMMatrixInverse(nullptr,XMLoadFloat4x4(&current))*XMLoadFloat4x4(&previous));
    std::array<float,16> result;
    std::memcpy(result.data(),&matrix,sizeof(matrix));
    return result;
}

// Ambient occlusion for a view as rendered (with its jitter): clip space back to world space.
// Depth of field and motion blur for the view (View::lens, View::shutter).
vk::HdrFrame::Camera cameraEffects(const View &view)
{
    vk::HdrFrame::Camera camera;
    camera.depthOfField = view.lens.x > 0.5f;
    camera.focusDistance = view.lens.y;
    camera.fNumber = view.lens.z;
    camera.verticalFov = view.lens.w;
    camera.motionBlur = view.shutter.x > 0.5f;
    camera.shutter = view.shutter.y;
    const auto clip = vulkanClip(view.viewProjection);
    XMFLOAT4X4 inverse;
    XMStoreFloat4x4(&inverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&clip)));
    std::memcpy(camera.inverseViewProjection.data(),&inverse,sizeof(inverse));
    const auto unproject = [&](float z) {
        const auto p = XMVector4Transform(XMVectorSet(0,0,z,1),XMLoadFloat4x4(&inverse));
        return XMVectorScale(p,1/XMVectorGetW(p));
    };
    XMFLOAT3 forward;
    XMStoreFloat3(&forward,XMVector3Normalize(XMVectorSubtract(unproject(1),unproject(0))));
    camera.eye = {view.eye.x,view.eye.y,view.eye.z};
    camera.forward = {forward.x,forward.y,forward.z};
    return camera;
}

vk::HdrFrame::Occlusion occlusionFor(const View &view)
{
    vk::HdrFrame::Occlusion occlusion;
    occlusion.enabled = view.sky.z > 0.5f;
    const auto clip = vulkanClip(view.viewProjection);
    XMFLOAT4X4 inverse;
    XMStoreFloat4x4(&inverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&clip)));
    std::memcpy(occlusion.inverseViewProjection.data(),&inverse,sizeof(inverse));
    return occlusion;
}

std::wstring wide(const char *text)
{
    const int size = MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
    std::wstring result(std::size_t(std::max(size,1)),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text,-1,result.data(),size);
    result.resize(std::wcslen(result.c_str()));
    return result;
}

class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override { release(); }

    bool start(HWND window,std::wstring &error);
    int addTexture(const Image &image) override;
    bool draw(const std::vector<PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
              UINT width,UINT height,bool sky,bool verify,UINT sync) override;
    bool capture(const std::vector<PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
                 UINT width,UINT height,bool sky,Image &image) override;
    const std::wstring &adapterName() const override { return adapter_; }
    const char *api() const override { return "Vulkan"; }

private:
    struct Frame {
        VkCommandBuffer command{};
        VkFence fence{};
        VkSemaphore acquired{};
        // environment: the sky's map (binding 5); clusters: the lights and their clusters (binding 6).
        vk::Buffer vertices, lighting, view, environment, clusters;
        std::uint64_t environmentRevision = ~std::uint64_t(0);
        vk::Image shadow;
        VkFramebuffer shadowFramebuffer{};
        VkDescriptorSet set{};
    };

    bool createInstance(std::wstring &error);
    bool chooseDevice(std::wstring &error);
    bool createLayouts();
    bool createPipelines();
    bool createFrames();
    bool createSwapchain(UINT width,UINT height,VkPresentModeKHR mode);
    void destroySwapchain();
    VkPipeline pipeline(std::span<const uint32_t> vertexCode,std::span<const uint32_t> fragmentCode,VkPipelineLayout layout,
                        VkRenderPass pass,bool scene,bool shadow);
    VkDescriptorSet materialSet(const MaterialDraw &material);
    bool prepare(Frame &frame,const std::vector<PlayVertex> &vertices,const View &view,UINT width,UINT height);
    // meterSlot >= 0 meters the scene into that histogram of target for auto exposure.
    void record(Frame &frame,vk::HdrFrame &target,std::size_t output,const std::vector<DrawRange> &ranges,
                const View &view,bool sky,std::size_t vertexCount,float exposureStops,int meterSlot,
                const vk::HdrFrame::Temporal &temporal,const vk::HdrFrame::Occlusion &occlusion);
    // The exposure in stops for the frame in slot, adapting toward the histogram that slot metered last time.
    float exposureFor(const View &view,uint32_t slot);
    bool submitAndWait(VkCommandBuffer command);
    VkCommandBuffer beginOneTime();
    void release();

    HWND window_{};
    vk::Api api_;
    vk::Device device_;
    VkInstance instance_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physical_{};
    VkDevice device{};
    VkQueue queue_{};
    uint32_t family_ = 0;
    bool anisotropy_ = false;
    std::wstring adapter_;
    VkCommandPool pool_{};

    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkSwapchainKHR swapchain_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> rendered_;
    VkExtent2D requested_{};
    VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool swapchainCopies_ = false;
    vk::HdrFrame frame_;

    VkDescriptorSetLayout frameLayout_{}, materialLayout_{};
    VkPipelineLayout skyLayout_{}, sceneLayout_{}, shadowLayout_{};
    VkRenderPass shadowPass_{};
    VkPipeline sky_{}, scene_{}, shadowPipeline_{};
    VkSampler shadowSampler_{}, textureSampler_{};
    VkDescriptorPool framePool_{};
    std::vector<VkDescriptorPool> materialPools_;
    std::map<std::array<int,5>,VkDescriptorSet> materialSets_;
    std::vector<vk::Image> textures_;
    int white_ = -1;
    std::array<Frame,framesInFlight> frames_{};
    uint32_t frameIndex_ = 0;
    // Sky visibility maps, shared by all frames: they change slowly, so they are re-rendered every skyRefreshFrames
    // frames, or at once when the scene's bounds change a lot.
    static constexpr uint32_t skyRefreshFrames = 30;
    vk::Image skyImage_;
    VkFramebuffer skyFramebuffer_{};
    sky::Visibility skyRendered_;
    bool skyValid_ = false, skyInitialized_ = false;
    uint32_t skyAge_ = 0;
    void updateSky(View &view);
    // Point light shadow cubes (LightClusters.h), shared by all frames like the sky maps: re-rendered when the shadowed
    // lights change, and every skyRefreshFrames frames for moving objects.
    vk::Image pointShadowImage_;
    VkFramebuffer pointShadowFramebuffer_{};
    std::vector<std::array<float,4>> pointShadowsRendered_;
    uint32_t pointShadowAge_ = 0;
    bool pointShadowsInitialized_ = false;
    lights::Clusters clusters_; // the clusters prepare built for this frame
    vk::Buffer readback_;
    float adaptedStops_ = std::numeric_limits<float>::quiet_NaN();
    std::chrono::steady_clock::time_point adaptedAt_{};
    // TAA: the previous frame's unjittered Vulkan view-projection, for reprojecting the history.
    XMFLOAT4X4 previousClip_{};
    bool previousValid_ = false;
    // Screen-space reflections for the frame being prepared: the previous frame's clip space without jitter, and whether
    // the target holds a previous frame to trace.
    XMFLOAT4X4 reflectionClip_{};
    bool reflectionReady_ = false;
};

bool VulkanRenderer::start(HWND window,std::wstring &error)
{
    window_ = window;
    if (!api_.loadLibrary()) {
        error = L"No Vulkan driver is installed (vulkan-1.dll was not found).";
        return false;
    }
    if (!createInstance(error) || !chooseDevice(error))
        return false;
    const VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,family_};
    if (api_.vkCreateCommandPool(device,&pool,nullptr,&pool_) != VK_SUCCESS) {
        error = L"Vulkan could not create a command pool.";
        return false;
    }
    uint32_t count = 0;
    api_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_,surface_,&count,nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    api_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_,surface_,&count,formats.data());
    for (const auto preferred : {VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM})
        for (const auto &candidate : formats)
            if (format_ == VK_FORMAT_UNDEFINED && candidate.format == preferred
                && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format_ = candidate.format;
                colorSpace_ = candidate.colorSpace;
            }
    if (format_ == VK_FORMAT_UNDEFINED && !formats.empty()) {
        format_ = formats.front().format;
        colorSpace_ = formats.front().colorSpace;
    }
    if (format_ == VK_FORMAT_UNDEFINED || !frame_.create(device_,format_,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        || !frame_.createDisplayPipeline(spirv(spirv_Display_vert),spirv(spirv_Display_frag),spirv(spirv_Temporal_frag),
                                    spirv(spirv_Ambient_frag),spirv(spirv_BloomDown_frag),spirv(spirv_BloomUp_frag),
                                    spirv(spirv_DepthOfField_frag),spirv(spirv_MotionBlur_frag))) {
        error = L"Vulkan could not create the HDR frame pass.";
        return false;
    }
    if (!createLayouts() || !createPipelines() || !createFrames()) {
        error = L"Vulkan could not create the Player pipelines.";
        return false;
    }
    white_ = addTexture({1,1,{0xffffffffu}});
    if (white_ < 0) {
        error = L"Vulkan could not upload a texture.";
        return false;
    }
    return true;
}

bool VulkanRenderer::createInstance(std::wstring &error)
{
    const VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr,"WorldForge Player",1,"WorldForge",1,
        VK_API_VERSION_1_1};
    const char *extensions[]{VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char *> layers;
    wchar_t validation[4]{};
    if (GetEnvironmentVariableW(L"WORLDFORGE_VULKAN_VALIDATION",validation,4) && validation[0] == L'1') {
        uint32_t count = 0;
        api_.vkEnumerateInstanceLayerProperties(&count,nullptr);
        std::vector<VkLayerProperties> available(count);
        api_.vkEnumerateInstanceLayerProperties(&count,available.data());
        for (const auto &layer : available)
            if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation")
                layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    const VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,nullptr,0,&application,
        uint32_t(layers.size()),layers.data(),2,extensions};
    if (api_.vkCreateInstance(&info,nullptr,&instance_) != VK_SUCCESS || !api_.loadInstance(instance_)) {
        error = L"The Vulkan driver could not create an instance with window support.";
        return false;
    }
    const auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        api_.vkGetInstanceProcAddr(instance_,"vkCreateWin32SurfaceKHR"));
    const VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,nullptr,0,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_,GWLP_HINSTANCE)),window_};
    if (!createSurface || createSurface(instance_,&surface,nullptr,&surface_) != VK_SUCCESS) {
        error = L"Vulkan could not create a surface for the Player window.";
        return false;
    }
    return true;
}

bool VulkanRenderer::chooseDevice(std::wstring &error)
{
    uint32_t count = 0;
    api_.vkEnumeratePhysicalDevices(instance_,&count,nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    api_.vkEnumeratePhysicalDevices(instance_,&count,devices.data());
    int bestScore = -1;
    for (const auto candidate : devices) {
        uint32_t extensionCount = 0;
        api_.vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extensionCount,nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        api_.vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extensionCount,extensions.data());
        if (std::ranges::none_of(extensions,[](const auto &extension) {
                return std::string_view(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME; }))
            continue;
        uint32_t familyCount = 0;
        api_.vkGetPhysicalDeviceQueueFamilyProperties(candidate,&familyCount,nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        api_.vkGetPhysicalDeviceQueueFamilyProperties(candidate,&familyCount,families.data());
        for (uint32_t family = 0; family < familyCount; ++family) {
            VkBool32 present = VK_FALSE;
            api_.vkGetPhysicalDeviceSurfaceSupportKHR(candidate,family,surface_,&present);
            if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
                continue;
            VkPhysicalDeviceProperties properties{};
            api_.vkGetPhysicalDeviceProperties(candidate,&properties);
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > bestScore) {
                bestScore = score;
                physical_ = candidate;
                family_ = family;
                adapter_ = wide(properties.deviceName);
            }
            break;
        }
    }
    if (!physical_) {
        error = L"No Vulkan device can draw to the Player window.";
        return false;
    }
    VkPhysicalDeviceFeatures supported{};
    api_.vkGetPhysicalDeviceFeatures(physical_,&supported);
    VkPhysicalDeviceFeatures features{};
    anisotropy_ = supported.samplerAnisotropy;
    features.samplerAnisotropy = supported.samplerAnisotropy;
    features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics; // auto exposure histogram
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family_,1,&priority};
    const char *extensions[]{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,nullptr,0,1,&queue,0,nullptr,1,extensions,&features};
    if (api_.vkCreateDevice(physical_,&info,nullptr,&device) != VK_SUCCESS || !api_.loadDevice(device)) {
        error = L"Vulkan could not create a logical device.";
        return false;
    }
    api_.vkGetDeviceQueue(device,family_,0,&queue_);
    device_.init(api_,physical_,device);
    return true;
}

bool VulkanRenderer::createLayouts()
{
    constexpr auto both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding frameBindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,both,nullptr},
        {3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,both,nullptr},
        {4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {7,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {8,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    std::array<VkDescriptorSetLayoutBinding,5> materialBindings{};
    for (uint32_t i = 0; i < materialBindings.size(); ++i)
        materialBindings[i] = {i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    const VkDescriptorSetLayoutCreateInfo frameInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,8,frameBindings};
    const VkDescriptorSetLayoutCreateInfo materialInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,
        uint32_t(materialBindings.size()),materialBindings.data()};
    if (api_.vkCreateDescriptorSetLayout(device,&frameInfo,nullptr,&frameLayout_) != VK_SUCCESS
        || api_.vkCreateDescriptorSetLayout(device,&materialInfo,nullptr,&materialLayout_) != VK_SUCCESS)
        return false;
    const VkPushConstantRange camera{both,0,sizeof(CameraConstants)};
    const VkPushConstantRange shadowCamera{VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(CameraConstants)};
    const VkPushConstantRange material{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(MaterialConstants)};
    const VkDescriptorSetLayout sceneSets[]{frameLayout_,materialLayout_};
    const VkPipelineLayoutCreateInfo sky{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,1,&frameLayout_,1,&camera};
    const VkPipelineLayoutCreateInfo scene{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,2,sceneSets,1,&material};
    const VkPipelineLayoutCreateInfo shadow{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,0,nullptr,1,&shadowCamera};
    if (api_.vkCreatePipelineLayout(device,&sky,nullptr,&skyLayout_) != VK_SUCCESS
        || api_.vkCreatePipelineLayout(device,&scene,nullptr,&sceneLayout_) != VK_SUCCESS
        || api_.vkCreatePipelineLayout(device,&shadow,nullptr,&shadowLayout_) != VK_SUCCESS)
        return false;

    const VkAttachmentDescription depth{0,frame_.depthFormat(),VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkAttachmentReference depthReference{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,nullptr,0,nullptr,nullptr,&depthReference,0,nullptr};
    constexpr auto tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkSubpassDependency dependencies[]{
        {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,tests,VK_ACCESS_SHADER_READ_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0},
        {0,VK_SUBPASS_EXTERNAL,tests,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_SHADER_READ_BIT,0}};
    const VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,1,&depth,1,&subpass,2,dependencies};
    if (api_.vkCreateRenderPass(device,&pass,nullptr,&shadowPass_) != VK_SUCCESS)
        return false;

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (api_.vkCreateSampler(device,&sampler,nullptr,&shadowSampler_) != VK_SUCCESS)
        return false;
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.anisotropyEnable = anisotropy_;
    sampler.maxAnisotropy = anisotropy_ ? 8.0f : 1.0f;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    return api_.vkCreateSampler(device,&sampler,nullptr,&textureSampler_) == VK_SUCCESS;
}

VkPipeline VulkanRenderer::pipeline(std::span<const uint32_t> vertexCode,std::span<const uint32_t> fragmentCode,
                                    VkPipelineLayout layout,VkRenderPass pass,bool scene,bool shadow)
{
    const auto vertex = device_.shader(vertexCode);
    const auto fragment = shadow ? VK_NULL_HANDLE : device_.shader(fragmentCode);
    VkPipeline result{};
    if (vertex && (fragment || shadow)) {
        const VkPipelineShaderStageCreateInfo stages[]{
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vertex,"main",nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fragment,"main",nullptr}};
        static_assert(sizeof(PlayVertex) == 48);
        const VkVertexInputBindingDescription binding{0,sizeof(PlayVertex),VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[]{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,position)},
            {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,normal)},
            {2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,color)},
            {3,0,VK_FORMAT_R32_SFLOAT,offsetof(PlayVertex,wind)},
            {4,0,VK_FORMAT_R32G32_SFLOAT,offsetof(PlayVertex,uv)}};
        // Shadow.vert reads position at location 0 and wind at location 2.
        const VkVertexInputAttributeDescription shadowAttributes[]{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,position)},
            {2,0,VK_FORMAT_R32_SFLOAT,offsetof(PlayVertex,wind)}};
        const bool geometry = scene || shadow;
        const VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,nullptr,0,
            geometry ? 1u : 0u,&binding,shadow ? 2u : scene ? 5u : 0u,shadow ? shadowAttributes : attributes};
        const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            nullptr,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_FALSE};
        const VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            nullptr,0,1,nullptr,1,nullptr};
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1;
        raster.depthBiasEnable = shadow;
        raster.depthBiasConstantFactor = 1.25f;
        raster.depthBiasSlopeFactor = 1.3f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = geometry ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
            | VK_COLOR_COMPONENT_A_BIT;
        // The scene pass has targets for the sky-light part and the surface albedo, which only the scene shader writes.
        VkPipelineColorBlendAttachmentState blends[vk::HdrFrame::sceneColorAttachments]{blend,blend,blend};
        if (!geometry)
            blends[1].colorWriteMask = blends[2].colorWriteMask = 0;
        VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colors.attachmentCount = vk::HdrFrame::sceneColorAttachments;
        colors.pAttachments = blends;
        const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,nullptr,0,2,states};
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = shadow ? 1 : 2;
        info.pStages = stages;
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &samples;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = shadow ? nullptr : &colors;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = pass;
        info.subpass = shadow ? 0 : vk::HdrFrame::sceneSubpass;
        if (api_.vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&result) != VK_SUCCESS)
            result = VK_NULL_HANDLE;
    }
    if (vertex)
        api_.vkDestroyShaderModule(device,vertex,nullptr);
    if (fragment)
        api_.vkDestroyShaderModule(device,fragment,nullptr);
    return result;
}

bool VulkanRenderer::createPipelines()
{
    sky_ = pipeline(spirv(spirv_Sky_vert),spirv(spirv_Sky_frag),skyLayout_,frame_.scenePass(),false,false);
    scene_ = pipeline(spirv(spirv_PlayScene_vert),spirv(spirv_PlayScene_frag),sceneLayout_,frame_.scenePass(),true,false);
    shadowPipeline_ = pipeline(spirv(spirv_Shadow_vert),{},shadowLayout_,shadowPass_,false,true);
    return sky_ && scene_ && shadowPipeline_;
}

bool VulkanRenderer::createFrames()
{
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4*framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2*framesInFlight},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2*framesInFlight}};
    const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,framesInFlight,3,sizes};
    if (api_.vkCreateDescriptorPool(device,&pool,nullptr,&framePool_) != VK_SUCCESS)
        return false;
    if (!skyImage_.create(device_,frame_.depthFormat(),{sky::atlasSize,sky::atlasSize},
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    const VkFramebufferCreateInfo skyFramebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,shadowPass_,
        1,&skyImage_.view,sky::atlasSize,sky::atlasSize,1};
    if (api_.vkCreateFramebuffer(device,&skyFramebuffer,nullptr,&skyFramebuffer_) != VK_SUCCESS)
        return false;
    if (!pointShadowImage_.create(device_,frame_.depthFormat(),{lights::shadowAtlasWidth,lights::shadowAtlasHeight},
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    const VkFramebufferCreateInfo pointShadowFramebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,shadowPass_,
        1,&pointShadowImage_.view,lights::shadowAtlasWidth,lights::shadowAtlasHeight,1};
    if (api_.vkCreateFramebuffer(device,&pointShadowFramebuffer,nullptr,&pointShadowFramebuffer_) != VK_SUCCESS)
        return false;
    pointShadowsRendered_.clear();
    pointShadowsInitialized_ = false;
    skyValid_ = skyInitialized_ = false;
    for (auto &frame : frames_) {
        const VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,pool_,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
        const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,nullptr,VK_FENCE_CREATE_SIGNALED_BIT};
        const VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,framePool_,1,&frameLayout_};
        if (api_.vkAllocateCommandBuffers(device,&command,&frame.command) != VK_SUCCESS
            || api_.vkCreateFence(device,&fence,nullptr,&frame.fence) != VK_SUCCESS
            || api_.vkCreateSemaphore(device,&semaphore,nullptr,&frame.acquired) != VK_SUCCESS
            || !frame.lighting.create(device_,sizeof(LightingBlock),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
            || !frame.view.create(device_,sizeof(PlayViewBlock),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
            || !frame.environment.create(device_,color::mapTexels*sizeof(std::array<float,4>),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
            || !frame.clusters.create(device_,lights::Clusters::bufferBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
            || !frame.shadow.create(device_,frame_.depthFormat(),{shadowSize,shadowSize},
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT)
            || api_.vkAllocateDescriptorSets(device,&set,&frame.set) != VK_SUCCESS)
            return false;
        const VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,shadowPass_,
            1,&frame.shadow.view,shadowSize,shadowSize,1};
        if (api_.vkCreateFramebuffer(device,&framebuffer,nullptr,&frame.shadowFramebuffer) != VK_SUCCESS)
            return false;
        const VkDescriptorImageInfo skyMaps{shadowSampler_,skyImage_.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo pointShadows{shadowSampler_,pointShadowImage_.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo shadow{shadowSampler_,frame.shadow.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo lighting{frame.lighting.buffer,0,sizeof(LightingBlock)};
        const VkDescriptorBufferInfo view{frame.view.buffer,0,sizeof(PlayViewBlock)};
        const VkDescriptorBufferInfo environment{frame.environment.buffer,0,color::mapTexels*sizeof(std::array<float,4>)};
        const VkDescriptorBufferInfo clusters{frame.clusters.buffer,0,lights::Clusters::bufferBytes};
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&shadow,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,1,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&lighting,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,3,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&view,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,4,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&skyMaps,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,5,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&environment,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,6,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&clusters,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,7,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&pointShadows,nullptr,nullptr}};
        api_.vkUpdateDescriptorSets(device,7,writes,0,nullptr);
    }
    return true;
}

bool VulkanRenderer::createSwapchain(UINT width,UINT height,VkPresentModeKHR mode)
{
    api_.vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR capabilities{};
    if (api_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_,surface_,&capabilities) != VK_SUCCESS)
        return false;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX)
        extent = {std::clamp<uint32_t>(width,capabilities.minImageExtent.width,capabilities.maxImageExtent.width),
                  std::clamp<uint32_t>(height,capabilities.minImageExtent.height,capabilities.maxImageExtent.height)};
    requested_ = {width,height};
    mode_ = mode;
    if (!extent.width || !extent.height) {
        destroySwapchain();
        return false;
    }
    uint32_t images = std::max(capabilities.minImageCount+1,2u);
    if (capabilities.maxImageCount)
        images = std::min(images,capabilities.maxImageCount);
    swapchainCopies_ = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = images;
    info.imageFormat = format_;
    info.imageColorSpace = colorSpace_;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swapchainCopies_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR created{};
    const bool ok = api_.vkCreateSwapchainKHR(device,&info,nullptr,&created) == VK_SUCCESS;
    destroySwapchain();
    if (!ok)
        return false;
    swapchain_ = created;
    uint32_t count = 0;
    api_.vkGetSwapchainImagesKHR(device,swapchain_,&count,nullptr);
    images_.resize(count);
    api_.vkGetSwapchainImagesKHR(device,swapchain_,&count,images_.data());
    for (const auto image : images_) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView imageView{};
        const VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore rendered{};
        if (api_.vkCreateImageView(device,&view,nullptr,&imageView) != VK_SUCCESS
            || api_.vkCreateSemaphore(device,&semaphore,nullptr,&rendered) != VK_SUCCESS)
            return false;
        views_.push_back(imageView);
        rendered_.push_back(rendered);
    }
    return frame_.createTargets(extent,views_);
}

void VulkanRenderer::destroySwapchain()
{
    frame_.releaseTargets();
    for (const auto view : views_)
        api_.vkDestroyImageView(device,view,nullptr);
    for (const auto semaphore : rendered_)
        api_.vkDestroySemaphore(device,semaphore,nullptr);
    views_.clear();
    rendered_.clear();
    images_.clear();
    if (swapchain_)
        api_.vkDestroySwapchainKHR(device,swapchain_,nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

VkCommandBuffer VulkanRenderer::beginOneTime()
{
    const VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,pool_,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer command{};
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
    if (api_.vkAllocateCommandBuffers(device,&allocation,&command) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    api_.vkBeginCommandBuffer(command,&begin);
    return command;
}

bool VulkanRenderer::submitAndWait(VkCommandBuffer command)
{
    api_.vkEndCommandBuffer(command);
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&command,0,nullptr};
    const bool ok = api_.vkQueueSubmit(queue_,1,&submit,VK_NULL_HANDLE) == VK_SUCCESS
        && api_.vkQueueWaitIdle(queue_) == VK_SUCCESS;
    api_.vkFreeCommandBuffers(device,pool_,1,&command);
    return ok;
}

int VulkanRenderer::addTexture(const Image &image)
{
    if (image.pixels.empty() || image.pixels.size() != std::size_t(image.width)*image.height)
        return -1;
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const bool blits = device_.supports(format,VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    const uint32_t levels = blits ? uint32_t(std::floor(std::log2(double(std::max(image.width,image.height)))))+1 : 1;
    vk::Image texture;
    vk::Buffer staging;
    const auto bytes = VkDeviceSize(image.pixels.size()*4);
    if (!texture.create(device_,format,{image.width,image.height},
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT,levels)
        || !staging.create(device_,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        texture.destroy(device_);
        return -1;
    }
    std::memcpy(staging.mapped,image.pixels.data(),bytes);
    const auto command = beginOneTime();
    const auto barrier = [&](uint32_t level,uint32_t count,VkImageLayout from,VkImageLayout to,VkAccessFlags source,
                             VkAccessFlags destination,VkPipelineStageFlags sourceStage,VkPipelineStageFlags destinationStage) {
        const VkImageMemoryBarrier info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,source,destination,from,to,
            VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,texture.image,{VK_IMAGE_ASPECT_COLOR_BIT,level,count,0,1}};
        api_.vkCmdPipelineBarrier(command,sourceStage,destinationStage,0,0,nullptr,0,nullptr,1,&info);
    };
    barrier(0,levels,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkBufferImageCopy copy{0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{image.width,image.height,1}};
    api_.vkCmdCopyBufferToImage(command,staging.buffer,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    int32_t width = int32_t(image.width), height = int32_t(image.height);
    for (uint32_t level = 1; level < levels; ++level) {
        barrier(level-1,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        const int32_t nextWidth = std::max(width/2,1), nextHeight = std::max(height/2,1);
        const VkImageBlit blit{{VK_IMAGE_ASPECT_COLOR_BIT,level-1,0,1},{{0,0,0},{width,height,1}},
                               {VK_IMAGE_ASPECT_COLOR_BIT,level,0,1},{{0,0,0},{nextWidth,nextHeight,1}}};
        api_.vkCmdBlitImage(command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,texture.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
        barrier(level-1,1,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        width = nextWidth;
        height = nextHeight;
    }
    barrier(levels-1,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const bool uploaded = command && submitAndWait(command);
    staging.destroy(device_);
    if (!uploaded) {
        texture.destroy(device_);
        return -1;
    }
    textures_.push_back(texture);
    return int(textures_.size()-1);
}

VkDescriptorSet VulkanRenderer::materialSet(const MaterialDraw &material)
{
    if (const auto found = materialSets_.find(material.textures); found != materialSets_.end())
        return found->second;
    VkDescriptorSet set{};
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,
        materialPools_.empty() ? VK_NULL_HANDLE : materialPools_.back(),1,&materialLayout_};
    if (!allocation.descriptorPool || api_.vkAllocateDescriptorSets(device,&allocation,&set) != VK_SUCCESS) {
        constexpr uint32_t sets = 256;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,sets*5};
        const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,sets,1,&size};
        VkDescriptorPool created{};
        if (api_.vkCreateDescriptorPool(device,&pool,nullptr,&created) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        materialPools_.push_back(created);
        allocation.descriptorPool = created;
        if (api_.vkAllocateDescriptorSets(device,&allocation,&set) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }
    std::array<VkDescriptorImageInfo,5> images{};
    std::array<VkWriteDescriptorSet,5> writes{};
    for (std::size_t i = 0; i < images.size(); ++i) {
        const int index = material.textures[i] >= 0 && std::size_t(material.textures[i]) < textures_.size()
            ? material.textures[i] : white_;
        images[i] = {textureSampler_,textures_[std::size_t(index)].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,uint32_t(i),0,1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&images[i],nullptr,nullptr};
    }
    api_.vkUpdateDescriptorSets(device,uint32_t(writes.size()),writes.data(),0,nullptr);
    materialSets_.emplace(material.textures,set);
    return set;
}

bool VulkanRenderer::prepare(Frame &frame,const std::vector<PlayVertex> &vertices,const View &view,UINT width,UINT height)
{
    const auto bytes = VkDeviceSize(vertices.size()*sizeof(PlayVertex));
    if (bytes > 240000000)
        return false;
    if (bytes > frame.vertices.size) {
        frame.vertices.destroy(device_);
        if (!frame.vertices.create(device_,bytes+bytes/4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
            return false;
    }
    if (bytes)
        std::memcpy(frame.vertices.mapped,vertices.data(),bytes);
    LightingBlock lighting{};
    for (int i = 0; i < shadow::cascadeCount; ++i) {
        std::memcpy(&lighting.cascadeMatrices[i],view.cascades.matrices[i].data(),sizeof(lighting.cascadeMatrices[i]));
        lighting.cascadeScales[i] = {view.cascades.scales[i][0],view.cascades.scales[i][1],view.cascades.scales[i][2],0};
    }
    lighting.fogCenterRadius = view.fogCenterRadius;
    lighting.settings = {view.lighting.x,view.lighting.y,float(view.temporalSample),view.sky.x};
    XMFLOAT3 sunDirection;
    XMStoreFloat3(&sunDirection,XMVector3Normalize(XMVectorSet(view.sun.x,view.sun.y,view.sun.z,0)));
    lighting.sun = {sunDirection.x,sunDirection.y,sunDirection.z,view.sun.w};
    for (int i = 0; i < sky::directionCount; ++i) {
        std::memcpy(&lighting.skyMatrices[i],view.skyVisibility.matrices[i].data(),sizeof(lighting.skyMatrices[i]));
        const auto &direction = view.skyVisibility.directions[i];
        // w: the sky's luminance in that direction, which weights it in skyOpenness.
        float luminance = 0;
        if (view.skyEnvironment && view.skyEnvironment->size() >= color::mapTexels) {
            const float d[3]{direction[0],direction[1],direction[2]};
            const auto value = color::sky::sample(*view.skyEnvironment,0,color::mapWidth,color::mapHeight,d);
            luminance = 0.2126f*value[0]+0.7152f*value[1]+0.0722f*value[2];
        }
        lighting.skyDirections[i] = {direction[0],direction[1],direction[2],luminance};
    }
    lighting.skySettings = {view.skyMaps && view.sky.z > 0.5f ? 1.0f : 0.0f,view.skyVisibility.metersPerTexel,view.sky.w,0};
    std::copy(std::begin(view.skyIrradiance),std::end(view.skyIrradiance),std::begin(lighting.skyIrradiance));
    lighting.skyHorizon = {view.skyHorizon.x,view.skyHorizon.y,view.skyHorizon.z,0};
    lighting.skyZenith = view.skyZenith;
    lighting.skyGround = view.skyGround;
    lighting.sunTint = view.sunTint;
    // Screen-space reflections and contact shadows trace the previous frame (View::shutter z and w).
    const bool reflections = view.shutter.z > 0.5f && reflectionReady_;
    const bool contactShadows = view.shutter.w > 0.5f && view.lighting.y > 0 && reflectionReady_;
    XMStoreFloat4x4(&lighting.previousViewProjection,reflectionReady_ ? XMLoadFloat4x4(&reflectionClip_) : XMMatrixIdentity());
    lighting.reflectionSettings = {reflections ? 1.0f : 0.0f,vk::HdrFrame::reflectionRange,contactShadows ? 1.0f : 0.0f,0};
    if (view.skyEnvironment && frame.environmentRevision != view.skyRevision
        && view.skyEnvironment->size()*sizeof(std::array<float,4>) <= frame.environment.size) {
        std::memcpy(frame.environment.mapped,view.skyEnvironment->data(),view.skyEnvironment->size()*sizeof(std::array<float,4>));
        frame.environmentRevision = view.skyRevision;
    }
    // Clustered lights: every light but the sun, binned by the clusters of this view.
    {
        const auto clip = vulkanClip(view.viewProjection);
        std::array<float,16> matrix;
        std::memcpy(matrix.data(),&clip,sizeof(matrix));
        XMFLOAT4X4 inverse;
        XMStoreFloat4x4(&inverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&clip)));
        const auto unproject = [&](float z) {
            const auto p = XMVector4Transform(XMVectorSet(0,0,z,1),XMLoadFloat4x4(&inverse));
            return XMVectorScale(p,1/XMVectorGetW(p));
        };
        XMFLOAT3 forward;
        XMStoreFloat3(&forward,XMVector3Normalize(XMVectorSubtract(unproject(1),unproject(0))));
        const std::array eye{view.eye.x,view.eye.y,view.eye.z};
        clusters_ = lights::build(view.sceneLights ? std::span<const lights::Light>(*view.sceneLights)
                                                   : std::span<const lights::Light>{},
                                  matrix,eye,{forward.x,forward.y,forward.z});
        clusters_.write(frame.clusters.mapped);
        lighting.clusterForward = {forward.x,forward.y,forward.z,0};
        lighting.clusterViewport = {float(width),float(height),0,0};
    }
    std::memcpy(frame.lighting.mapped,&lighting,sizeof(lighting));
    PlayViewBlock block{vulkanClip(view.viewProjection),view.eye,view.horizon,view.zenith,view.fog,view.sun,{},
        {view.lighting.z,view.sky.x,0,0}};
    std::copy(std::begin(view.lights),std::end(view.lights),std::begin(block.lights));
    std::memcpy(frame.view.mapped,&block,sizeof(block));
    return true;
}

// Decides whether this frame re-renders the sky visibility maps, and otherwise points view at the maps as they
// were last rendered.
void VulkanRenderer::updateSky(View &view)
{
    view.skyRefresh = 0;
    if (!view.skyMaps || view.sky.z <= 0.5f)
        return;
    const auto &fitted = view.skyVisibility;
    const float moved = std::hypot(fitted.center[0]-skyRendered_.center[0],fitted.center[1]-skyRendered_.center[1],
                                   fitted.center[2]-skyRendered_.center[2]);
    // A game's vertices and bounds change a little every frame (moving bodies, beacon pulses, level of detail),
    // so only a large change re-renders the maps at once; smaller ones wait for the periodic refresh.
    const bool refit = !skyValid_ || std::abs(fitted.radius-skyRendered_.radius) > 0.1f*skyRendered_.radius
        || moved > 8*skyRendered_.metersPerTexel;
    if (refit || ++skyAge_ >= skyRefreshFrames) {
        view.skyRefresh = 1;
        skyRendered_ = fitted;
        skyValid_ = true;
        skyAge_ = 0;
    } else {
        view.skyVisibility = skyRendered_;
    }
}

float VulkanRenderer::exposureFor(const View &view,uint32_t slot)
{
    const float manual = std::log2(std::max(view.display.x,1e-6f));
    if (view.display.z < 0.5f) {
        adaptedStops_ = std::numeric_limits<float>::quiet_NaN();
        return manual;
    }
    const auto now = std::chrono::steady_clock::now();
    const float seconds = adaptedAt_ == std::chrono::steady_clock::time_point{} ? 0.0f
        : std::min(0.25f,std::chrono::duration<float>(now-adaptedAt_).count());
    adaptedAt_ = now;
    float average = 0;
    if (frame_.takeMeter(slot,average)) {
        const float target = color::autoExposureStops(average,view.display.w);
        adaptedStops_ = std::isnan(adaptedStops_) ? target : color::adaptExposure(adaptedStops_,target,seconds);
    }
    return std::isnan(adaptedStops_) ? manual : adaptedStops_;
}

void VulkanRenderer::record(Frame &frame,vk::HdrFrame &target,std::size_t output,const std::vector<DrawRange> &ranges,
                            const View &view,bool sky,std::size_t vertexCount,float exposureStops,int meterSlot,
                            const vk::HdrFrame::Temporal &temporal,const vk::HdrFrame::Occlusion &occlusion)
{
    // Before anything binds the set: the previous frame's TAA result, which alternates between two images.
    const VkDescriptorImageInfo previous{target.nearestSampler(),target.previousFrame(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet previousWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,8,0,1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&previous,nullptr,nullptr};
    api_.vkUpdateDescriptorSets(device,1,&previousWrite,0,nullptr);
    const auto command = frame.command;
    const VkDeviceSize offset = 0;
    // Shadow map: always cleared so it is ready to sample; filled when sun shadows are on.
    const VkClearValue clearDepth{.depthStencil = {1,0}};
    const VkRect2D shadowArea{{0,0},{shadowSize,shadowSize}};
    const VkRenderPassBeginInfo shadowBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,shadowPass_,
        frame.shadowFramebuffer,shadowArea,1,&clearDepth};
    api_.vkCmdBeginRenderPass(command,&shadowBegin,VK_SUBPASS_CONTENTS_INLINE);
    if (view.lighting.y > 0 && vertexCount) {
        api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline_);
        api_.vkCmdBindVertexBuffers(command,0,1,&frame.vertices.buffer,&offset);
        // One tile of the atlas per cascade.
        for (int cascade = 0; cascade < shadow::cascadeCount; ++cascade) {
            const auto origin = shadow::tileOrigin(cascade);
            const VkViewport viewport{float(origin[0]),float(origin[1]),float(shadow::tileSize),float(shadow::tileSize),0,1};
            const VkRect2D tile{{int32_t(origin[0]),int32_t(origin[1])},{shadow::tileSize,shadow::tileSize}};
            api_.vkCmdSetViewport(command,0,1,&viewport);
            api_.vkCmdSetScissor(command,0,1,&tile);
            CameraConstants camera{};
            std::memcpy(&camera.matrix,view.cascades.matrices[cascade].data(),sizeof(camera.matrix));
            camera.eye.w = view.lighting.z;
            api_.vkCmdPushConstants(command,shadowLayout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(camera),&camera);
            api_.vkCmdDraw(command,uint32_t(vertexCount),1,0,0);
        }
    }
    api_.vkCmdEndRenderPass(command);

    // Sky visibility maps: the scene's depth seen from each sky direction, for large-scale sky occlusion. They are
    // drawn only when updateSky asks (and cleared once before first use); the render pass orders the writes after
    // earlier frames' reads.
    if (view.skyRefresh || !skyInitialized_) {
        skyInitialized_ = true;
        const VkRect2D skyArea{{0,0},{sky::atlasSize,sky::atlasSize}};
        const VkRenderPassBeginInfo skyBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,shadowPass_,
            skyFramebuffer_,skyArea,1,&clearDepth};
        api_.vkCmdBeginRenderPass(command,&skyBegin,VK_SUBPASS_CONTENTS_INLINE);
        if (view.skyRefresh && vertexCount) {
            api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline_);
            api_.vkCmdBindVertexBuffers(command,0,1,&frame.vertices.buffer,&offset);
            for (int direction = 0; direction < sky::directionCount; ++direction) {
                const auto origin = sky::tileOrigin(direction);
                const VkViewport viewport{float(origin[0]),float(origin[1]),float(sky::tileSize),float(sky::tileSize),0,1};
                const VkRect2D tile{{int32_t(origin[0]),int32_t(origin[1])},{sky::tileSize,sky::tileSize}};
                api_.vkCmdSetViewport(command,0,1,&viewport);
                api_.vkCmdSetScissor(command,0,1,&tile);
                CameraConstants camera{};
                std::memcpy(&camera.matrix,view.skyVisibility.matrices[direction].data(),sizeof(camera.matrix));
                camera.eye.w = view.lighting.z;
                api_.vkCmdPushConstants(command,shadowLayout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(camera),&camera);
                api_.vkCmdDraw(command,uint32_t(vertexCount),1,0,0);
            }
        }
        api_.vkCmdEndRenderPass(command);
    }


    // Point light shadow cubes (LightClusters.h): re-rendered when the shadowed lights change, and every
    // skyRefreshFrames frames for moving objects; cleared once before first use.
    const bool pointShadowsChanged = clusters_.shadowed != pointShadowsRendered_;
    if (!pointShadowsInitialized_
        || (!clusters_.shadowed.empty() && (pointShadowsChanged || ++pointShadowAge_ >= skyRefreshFrames))) {
        pointShadowsInitialized_ = true;
        pointShadowsRendered_ = clusters_.shadowed;
        pointShadowAge_ = 0;
        const VkRect2D atlas{{0,0},{lights::shadowAtlasWidth,lights::shadowAtlasHeight}};
        const VkRenderPassBeginInfo pointBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,shadowPass_,
            pointShadowFramebuffer_,atlas,1,&clearDepth};
        api_.vkCmdBeginRenderPass(command,&pointBegin,VK_SUBPASS_CONTENTS_INLINE);
        if (vertexCount && !clusters_.shadowed.empty()) {
            api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline_);
            api_.vkCmdBindVertexBuffers(command,0,1,&frame.vertices.buffer,&offset);
            for (std::size_t tile = 0; tile < clusters_.shadowed.size()*6; ++tile) {
                const auto x = uint32_t(tile%lights::shadowColumns)*lights::faceSize;
                const auto y = uint32_t(tile/lights::shadowColumns)*lights::faceSize;
                const VkViewport viewport{float(x),float(y),float(lights::faceSize),float(lights::faceSize),0,1};
                const VkRect2D face{{int32_t(x),int32_t(y)},{lights::faceSize,lights::faceSize}};
                api_.vkCmdSetViewport(command,0,1,&viewport);
                api_.vkCmdSetScissor(command,0,1,&face);
                CameraConstants camera{};
                std::memcpy(&camera.matrix,clusters_.shadowFaces[tile].data(),sizeof(camera.matrix));
                camera.eye.w = view.lighting.z;
                api_.vkCmdPushConstants(command,shadowLayout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(camera),&camera);
                api_.vkCmdDraw(command,uint32_t(vertexCount),1,0,0);
            }
        }
        api_.vkCmdEndRenderPass(command);
    }

    const auto background = color::srgbToLinear({0.05f,0.06f,0.07f});
    target.begin(command,output,background.data());
    if (vertexCount) {
        api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,scene_);
        api_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,sceneLayout_,0,1,&frame.set,0,nullptr);
        api_.vkCmdBindVertexBuffers(command,0,1,&frame.vertices.buffer,&offset);
        for (const auto &range : ranges) {
            const auto set = materialSet(range.material);
            if (!set || std::size_t(range.start)+range.count > vertexCount)
                continue;
            const auto &material = range.material;
            const MaterialConstants constants{{material.emissive[0],material.emissive[1],material.emissive[2],0},
                {material.metallic,material.roughness,float(material.advanced),float(material.textures[1] >= 0)}};
            api_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,sceneLayout_,1,1,&set,0,nullptr);
            api_.vkCmdPushConstants(command,sceneLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),&constants);
            api_.vkCmdDraw(command,range.count,1,range.start,0);
        }
    }
    // The far-plane sky shades only pixels left uncovered by geometry.
    if (sky) {
        XMFLOAT4X4 inverse;
        const auto clip = vulkanClip(view.viewProjection);
        XMStoreFloat4x4(&inverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&clip)));
        const CameraConstants camera{inverse,view.eye,view.fog,view.horizon,view.zenith};
        api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,sky_);
        api_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,skyLayout_,0,1,&frame.set,0,nullptr);
        api_.vkCmdPushConstants(command,skyLayout_,VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,0,
                                sizeof(camera),&camera);
        api_.vkCmdDraw(command,3,1,0,0);
    }
    target.display(command,exposureStops,view.display.y > 0.5f ? color::ViewTransform::AgX : color::ViewTransform::Standard,
                   meterSlot,temporal,occlusion,view.bloom,cameraEffects(view));
    api_.vkCmdEndRenderPass(command);
    if (meterSlot >= 0)
        target.finishMeter(command);
}

bool VulkanRenderer::draw(const std::vector<PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
                          UINT width,UINT height,bool sky,bool verify,UINT sync)
{
    if (!width || !height)
        return true;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!sync) {
        uint32_t count = 0;
        api_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_,surface_,&count,nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        api_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_,surface_,&count,modes.data());
        for (const auto candidate : {VK_PRESENT_MODE_IMMEDIATE_KHR,VK_PRESENT_MODE_MAILBOX_KHR})
            if (mode == VK_PRESENT_MODE_FIFO_KHR && std::ranges::find(modes,candidate) != modes.end())
                mode = candidate;
    }
    if ((!swapchain_ || requested_.width != width || requested_.height != height || mode != mode_)
        && !createSwapchain(width,height,mode))
        return !swapchain_; // A minimized or hidden window has no surface to draw to yet.
    auto &frame = frames_[frameIndex_];
    api_.vkWaitForFences(device,1,&frame.fence,VK_TRUE,UINT64_MAX);
    uint32_t image = 0;
    const auto acquired = api_.vkAcquireNextImageKHR(device,swapchain_,UINT64_MAX,frame.acquired,VK_NULL_HANDLE,&image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        requested_ = {};
        return true;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        return false;
    api_.vkResetFences(device,1,&frame.fence);
    // TAA: render with this frame's jitter and reproject the history with the camera's motion. Moving objects
    // rely on the history clamp.
    vk::HdrFrame::Temporal temporal;
    temporal.enabled = view.sky.y > 0.5f;
    const auto clip = vulkanClip(view.viewProjection);
    temporal.reprojection = reprojection(clip,previousValid_ ? previousClip_ : clip);
    reflectionClip_ = previousValid_ ? previousClip_ : clip;
    reflectionReady_ = previousValid_ && frame_.previousFrameReady();
    previousClip_ = clip;
    previousValid_ = true;
    auto shown = withShadows(temporal.enabled ? jittered(view,frame_.jitter(),width,height) : view,view,vertices,
                             frame_.sampleIndex());
    updateSky(shown);
    if (!prepare(frame,vertices,shown,width,height))
        return false;
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
    api_.vkResetCommandBuffer(frame.command,0);
    api_.vkBeginCommandBuffer(frame.command,&begin);
    const auto meterSlot = view.display.z > 0.5f ? int(frameIndex_) : -1;
    record(frame,frame_,image,ranges,shown,sky,vertices.size(),exposureFor(view,frameIndex_),meterSlot,temporal,
           occlusionFor(shown));
    const bool readPixel = verify && swapchainCopies_
        && (readback_.size >= 4 || readback_.create(device_,4096,VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    if (readPixel) {
        // Read the top-left pixel to check that the frame drew the sky.
        const auto transition = [&](VkImageLayout from,VkImageLayout to,VkAccessFlags source,VkAccessFlags destination) {
            const VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,source,destination,from,to,
                VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,images_[image],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            api_.vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        };
        transition(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferImageCopy copy{0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{1,1,1}};
        api_.vkCmdCopyImageToBuffer(frame.command,images_[image],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback_.buffer,1,&copy);
        transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_READ_BIT,0);
    }
    api_.vkEndCommandBuffer(frame.command);
    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,1,&frame.acquired,&wait,1,&frame.command,
        1,&rendered_[image]};
    if (api_.vkQueueSubmit(queue_,1,&submit,frame.fence) != VK_SUCCESS)
        return false;
    frameIndex_ = (frameIndex_+1)%framesInFlight;
    if (verify) {
        api_.vkWaitForFences(device,1,&frame.fence,VK_TRUE,UINT64_MAX);
        const auto *pixel = static_cast<const unsigned char *>(readback_.mapped);
        if (!readPixel || pixel[0]+pixel[1]+pixel[2] <= 120)
            return false;
    }
    const VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,nullptr,1,&rendered_[image],1,&swapchain_,&image,nullptr};
    const auto presented = api_.vkQueuePresentKHR(queue_,&present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        requested_ = {};
    return presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR;
}

bool VulkanRenderer::capture(const std::vector<PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
                             UINT width,UINT height,bool sky,Image &result)
{
    api_.vkDeviceWaitIdle(device);
    vk::HdrFrame target;
    vk::Image output;
    const auto bytes = VkDeviceSize(width)*height*4;
    auto &frame = frames_[frameIndex_];
    bool ok = output.create(device_,format_,{width,height},VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)
        && target.create(device_,format_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,1)
        && target.createDisplayPipeline(spirv(spirv_Display_vert),spirv(spirv_Display_frag),spirv(spirv_Temporal_frag),
                                    spirv(spirv_Ambient_frag),spirv(spirv_BloomDown_frag),spirv(spirv_BloomUp_frag),
                                    spirv(spirv_DepthOfField_frag),spirv(spirv_MotionBlur_frag))
        && target.createTargets({width,height},std::span(&output.view,1))
        && (readback_.size >= bytes || (readback_.destroy(device_),readback_.create(device_,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT)));
    // A capture is a still view, so it converges at once. Auto exposure meters the first frame and the later frames
    // use the metered exposure (metering reads the scene before exposure). With TAA the capture averages
    // temporalSamples jittered frames, as the editor does for a still view.
    const bool automatic = view.display.z > 0.5f;
    vk::HdrFrame::Temporal temporal;
    temporal.enabled = view.sky.y > 0.5f;
    temporal.still = true;
    // Bloom reads the previous frame, so a capture with bloom renders at least two. Screen-space reflections and contact
    // shadows trace the previous frame too, which the first frame lacks: a capture with them renders one frame first and
    // then starts the TAA average over, so every averaged frame has one, as in the editor.
    const bool warmUp = view.shutter.z > 0.5f || view.shutter.w > 0.5f;
    const int passes = std::max({temporal.enabled ? int(vk::HdrFrame::temporalSamples) : 1,automatic ? 2 : 1,
                                 view.bloom > 0 ? 2 : 1})+(warmUp ? 1 : 0);
    // A still view: the previous frame saw it through the same camera.
    reflectionClip_ = vulkanClip(view.viewProjection);
    float stops = std::log2(std::max(view.display.x,1e-6f));
    for (int pass = 0; ok && pass < passes; ++pass) {
        auto shown = withShadows(temporal.enabled ? jittered(view,target.jitter(),width,height) : view,view,vertices,
                                 target.sampleIndex());
        updateSky(shown);
        reflectionReady_ = target.previousFrameReady();
        if (!prepare(frame,vertices,shown,width,height)) {
            ok = false;
            break;
        }
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
        api_.vkResetFences(device,1,&frame.fence);
        api_.vkResetCommandBuffer(frame.command,0);
        api_.vkBeginCommandBuffer(frame.command,&begin);
        record(frame,target,0,ranges,shown,sky,vertices.size(),stops,automatic ? 0 : -1,temporal,occlusionFor(shown));
        const VkBufferImageCopy copy{0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{width,height,1}};
        api_.vkCmdCopyImageToBuffer(frame.command,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback_.buffer,1,&copy);
        api_.vkEndCommandBuffer(frame.command);
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&frame.command,0,nullptr};
        ok = api_.vkQueueSubmit(queue_,1,&submit,frame.fence) == VK_SUCCESS
            && api_.vkWaitForFences(device,1,&frame.fence,VK_TRUE,UINT64_MAX) == VK_SUCCESS;
        float average = 0;
        if (ok && automatic && target.takeMeter(0,average))
            stops = color::autoExposureStops(average,view.display.w);
        if (ok && warmUp && pass == 0)
            target.resetHistory();
    }
    if (ok) {
        // The output uses the swap chain's format; convert BGRA to RGBA when needed.
        result = {width,height,std::vector<std::uint32_t>(std::size_t(width)*height)};
        std::memcpy(result.pixels.data(),readback_.mapped,bytes);
        const bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
        for (auto &pixel : result.pixels)
            pixel = (bgra ? (pixel & 0x0000ff00u) | ((pixel & 0xffu) << 16) | ((pixel >> 16) & 0xffu) : pixel & 0x00ffffffu)
                | 0xff000000u;
    }
    api_.vkDeviceWaitIdle(device);
    target.destroy();
    output.destroy(device_);
    return ok;
}

void VulkanRenderer::release()
{
    if (device) {
        api_.vkDeviceWaitIdle(device);
        destroySwapchain();
        frame_.destroy();
        for (auto &frame : frames_) {
            frame.vertices.destroy(device_);
            frame.lighting.destroy(device_);
            frame.view.destroy(device_);
            frame.environment.destroy(device_);
            frame.clusters.destroy(device_);
            frame.environmentRevision = ~std::uint64_t(0);
            if (frame.shadowFramebuffer)
                api_.vkDestroyFramebuffer(device,frame.shadowFramebuffer,nullptr);
            frame.shadow.destroy(device_);
            if (frame.fence)
                api_.vkDestroyFence(device,frame.fence,nullptr);
            if (frame.acquired)
                api_.vkDestroySemaphore(device,frame.acquired,nullptr);
        }
        if (skyFramebuffer_)
            api_.vkDestroyFramebuffer(device,skyFramebuffer_,nullptr);
        skyFramebuffer_ = VK_NULL_HANDLE;
        skyImage_.destroy(device_);
        if (pointShadowFramebuffer_)
            api_.vkDestroyFramebuffer(device,pointShadowFramebuffer_,nullptr);
        pointShadowFramebuffer_ = VK_NULL_HANDLE;
        pointShadowImage_.destroy(device_);
        readback_.destroy(device_);
        for (auto &texture : textures_)
            texture.destroy(device_);
        for (const auto pool : materialPools_)
            api_.vkDestroyDescriptorPool(device,pool,nullptr);
        if (framePool_)
            api_.vkDestroyDescriptorPool(device,framePool_,nullptr);
        for (const auto handle : {sky_,scene_,shadowPipeline_})
            if (handle)
                api_.vkDestroyPipeline(device,handle,nullptr);
        for (const auto handle : {skyLayout_,sceneLayout_,shadowLayout_})
            if (handle)
                api_.vkDestroyPipelineLayout(device,handle,nullptr);
        for (const auto handle : {frameLayout_,materialLayout_})
            if (handle)
                api_.vkDestroyDescriptorSetLayout(device,handle,nullptr);
        for (const auto handle : {shadowSampler_,textureSampler_})
            if (handle)
                api_.vkDestroySampler(device,handle,nullptr);
        if (shadowPass_)
            api_.vkDestroyRenderPass(device,shadowPass_,nullptr);
        if (pool_)
            api_.vkDestroyCommandPool(device,pool_,nullptr);
        api_.vkDestroyDevice(device,nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface_)
        api_.vkDestroySurfaceKHR(instance_,surface_,nullptr);
    if (instance_)
        api_.vkDestroyInstance(instance_,nullptr);
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}
} // namespace

std::unique_ptr<Renderer> createVulkanRenderer(HWND window,std::wstring &error)
{
    auto renderer = std::make_unique<VulkanRenderer>();
    if (!renderer->start(window,error))
        return nullptr;
    return renderer;
}

} // namespace worldforge::player
