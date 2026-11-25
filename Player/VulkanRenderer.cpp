#include "Player/VulkanRenderer.h"

#include "Engine/Geometry/Meshlets.h"
#include "Engine/Render/SkyVisibility.h"
#include "Engine/Render/Vulkan/HdrFrame.h"
#include "Engine/Render/Vulkan/ProbeTrace.h"
#include "Engine/Render/Vulkan/DrawCulling.h"
#include "Engine/Render/Vulkan/OcclusionDepth.h"
#include "Engine/Render/Vulkan/FogVolume.h"

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <array>
#include <bit>
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
#include "PlayMeshlets_mesh.h"

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
    std::array<float,4> probeGrid{}, probeSettings{};
    XMFLOAT4 skyIrradianceBand4[9]; // band-4 sky irradiance coefficients
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
    XMFLOAT4 emissive, factors, sheen, scattering;
    XMFLOAT2 clearCoat, optics;
    float anisotropyRotation, coatNormalScale;
    uint32_t textureFlags;
    uint32_t padding;
    XMFLOAT4 streetLayers;
};
static_assert(sizeof(LightingBlock) == 2160 && sizeof(PlayViewBlock) == 416 && sizeof(CameraConstants) == 128);
static_assert(offsetof(MaterialConstants,streetLayers) == 96 && sizeof(MaterialConstants) == 112);

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
    occlusion.screenSpaceGi = view.screenSpaceGi;
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
                 UINT width,UINT height,bool sky,Image &image,float *meteredLuminance) override;
    const std::wstring &adapterName() const override { return adapter_; }
    const char *api() const override { return "Vulkan"; }
    RenderStats stats() const override
    {
        auto result = stats_;
        result.gpuMs = gpuMs_;
        result.occludedOpaqueDraws = occludedDraws_;
        result.rejectedOpaqueDraws = rejectedDraws_;
        result.rejectedOpaqueTriangles = rejectedTriangles_;
        result.texturesPromoted = texturesPromoted_;
        result.textureWaitFrames = textureWaitFrames_;
        result.textureDetailBytes = textureDetailBytes_;
        result.textureStallMs = textureStallMs_;
        return result;
    }

private:
    struct Frame {
        VkCommandBuffer command{};
        VkFence fence{};
        VkSemaphore acquired{};
        // environment: the sky's map (binding 5); clusters: the lights and their clusters (binding 6).
        vk::Buffer lighting, view, environment, clusters;
        std::uint64_t environmentRevision = ~std::uint64_t(0);
        vk::Image shadow;
        VkFramebuffer shadowFramebuffer{};
        VkDescriptorSet set{};
        std::shared_ptr<const vk::ProbeTrace> probes;
        uint32_t cullingInputDraws = 0, cullingInputTriangles = 0;
        bool meshletsBound = false;
    };

    bool createInstance(std::wstring &error);
    bool chooseDevice(std::wstring &error);
    bool createLayouts();
    bool createPipelines();
    bool createFrames();
    bool createSwapchain(UINT width,UINT height,VkPresentModeKHR mode);
    void destroySwapchain();
    VkPipeline pipeline(std::span<const uint32_t> vertexCode,std::span<const uint32_t> fragmentCode,VkPipelineLayout layout,
                        VkRenderPass pass,bool scene,bool shadow,vk::MaterialPass materialPass = vk::MaterialPass::Opaque,bool mesh = false);
    VkDescriptorSet materialSet(const MaterialDraw &material);
    bool uploadTexture(const Image &image,vk::Image &texture);
    void requestTextureDetail(std::span<const PlayVertex> vertices,std::span<const DrawRange> ranges,const View &view,
                              UINT width,UINT height,bool capture);
    void refreshTextureDescriptors(int index);
    void prepareMeshlets(Frame &frame,std::span<const PlayVertex> vertices,std::span<const DrawRange> ranges,const View &view);
    bool prepare(Frame &frame,const std::vector<PlayVertex> &vertices,const View &view,UINT width,UINT height);
    void updateProbes(Frame &frame,std::span<const PlayVertex> vertices,std::span<const DrawRange> ranges,const View &view);
    // meterSlot >= 0 meters the scene into that histogram of target for auto exposure.
    void record(Frame &frame,vk::HdrFrame &target,std::size_t output,const std::vector<DrawRange> &ranges,
                const View &view,bool sky,std::span<const PlayVertex> vertices,float exposureStops,int meterSlot,
                const vk::HdrFrame::Temporal &temporal,const vk::HdrFrame::Occlusion &occlusion);
    // The exposure in stops for the frame in slot, adapting toward the histogram that slot metered last time.
    float exposureFor(const View &view,uint32_t slot);
    bool submitAndWait(VkCommandBuffer command);
    VkCommandBuffer beginOneTime();
    void release();

    HWND window_{};
    vk::Api api_;
    vk::Device device_;
    vk::ProbeField probeField_;
    vk::DrawCulling drawCulling_;
    vk::OcclusionDepth occlusionDepth_;
    vk::FogVolume fogVolume_;
    bool fogVolumeNeeded_ = false; // this frame fills the fog volume (updateProbes)
    std::vector<vk::DrawBounds> cullBatches_;
    std::vector<std::pair<float,uint32_t>> opaqueOrder_; // squared distance to the camera, range index
    std::vector<vk::DrawBounds> orderBounds_; // range boxes for that order when the frame is not GPU-culled
    uint32_t rejectedDraws_ = 0, rejectedTriangles_ = 0, occludedDraws_ = 0;
    bool probeCompute_ = false;
    VkInstance instance_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physical_{};
    VkDevice device{};
    VkQueue queue_{};
    uint32_t family_ = 0;
    uint32_t instanceVersion_ = VK_API_VERSION_1_1;
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
    VkRenderPass shadowPass_{}, pointUpdatePass_{};
    VkPipeline sky_{}, scene_{}, shadowPipeline_{};
    VkPipeline glassTransmission_{}, glassRadiance_{}, meshScene_{};
    struct MeshletRange { uint32_t firstMeshlet = 0, meshlets = 0, firstIndex = 0; };
    std::vector<MeshletRange> meshletRanges_;
    vk::Buffer meshletData_, meshletIndices_;
    bool useMeshlets_ = false, meshShaders_ = false, meshletsPrepared_ = false;
    uint32_t maxMeshDispatch_ = 0;
    VkSampler shadowSampler_{}, textureSampler_{};
    VkDescriptorPool framePool_{};
    std::vector<VkDescriptorPool> materialPools_;
    std::map<std::array<int,materialTextureCount>,VkDescriptorSet> materialSets_;
    struct MaterialTexture {
        vk::Image tail, detail;
        Image source;
        std::size_t detailBytes = 0;
        uint32_t requestedSince = 0;
        uint32_t lastVisible = 0;
        VkImageView view() const { return detail.view ? detail.view : tail.view; }
    };
    std::vector<MaterialTexture> textures_;
    static constexpr std::size_t textureDetailBudget = 64*1024*1024;
    bool streamTextures_ = false;
    uint32_t textureFrame_ = 0;
    std::size_t textureDetailBytes_ = 0;
    uint32_t texturesPromoted_ = 0, textureWaitFrames_ = 0;
    double textureStallMs_ = 0;
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
    // Vertex buffers in a ring of three: each frame writes the next one, and its scene shader reads the one before as
    // the previous frame's positions (SceneMotion.glsl). With two frames in flight, the buffer written is never one
    // the GPU still reads. ringCounts_ holds each buffer's vertex count; vertexMotion_ is off for still captures.
    std::array<vk::Buffer,3> vertexRing_;
    std::array<std::size_t,3> ringCounts_{};
    std::array<std::size_t,3> ringCachedMeshVertices_{};
    std::size_t ring_ = 0;
    bool vertexMotion_ = true;
    RenderStats stats_; // what the last recorded frame submitted (stats)
    // This frame's exposure scale, which sizes lamp ranges in the light clusters (LightClusters.h).
    float clusterExposure_ = 0;
    // GPU timing (RenderStats::gpuMs): a timestamp before the frame and after each pass, per frame slot, read back once
    // the slot's fence has signaled so timing never waits on the GPU.
    static constexpr uint32_t timestampsPerFrame = RenderStats::passCount+1;
    VkQueryPool timestamps_{};
    float timestampPeriod_ = 0; // nanoseconds per tick
    std::array<bool,framesInFlight> timed_{};
    std::array<float,RenderStats::passCount> gpuMs_{};
    // The previous frame's wind time (for swaying foliage), and the one the frame being prepared uses.
    float previousWindTime_ = 0, motionWindTime_ = 0;
    bool previousWindValid_ = false;
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
    if (!createLayouts() || !fogVolume_.create(device_,frameLayout_) || !createPipelines() || !createFrames()) {
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
    wchar_t mode[16]{};
    useMeshlets_ = GetEnvironmentVariableW(L"WORLDFORGE_MESHLETS",mode,16) > 0 && (mode[0] == L'1' || mode[0] == L'i');
    if (useMeshlets_ && mode[0] == L'1' && api_.vkEnumerateInstanceVersion) {
        uint32_t version = VK_API_VERSION_1_0;
        if (api_.vkEnumerateInstanceVersion(&version) == VK_SUCCESS && version >= VK_API_VERSION_1_2)
            instanceVersion_ = VK_API_VERSION_1_2;
    }
    const VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr,"WorldForge Player",1,"WorldForge",1,
        instanceVersion_};
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
    bool indirectCount = false;
    bool meshExtension = false;
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
                indirectCount = std::ranges::any_of(extensions,[](const auto &extension) {
                    return std::string_view(extension.extensionName) == VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME;
                });
                meshExtension = instanceVersion_ >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2
                    && std::ranges::any_of(extensions,[](const auto &extension) {
                        return std::string_view(extension.extensionName) == VK_EXT_MESH_SHADER_EXTENSION_NAME;
                    });
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
    features.multiDrawIndirect = supported.multiDrawIndirect;
    wchar_t fixed[4]{};
    indirectCount = indirectCount && features.multiDrawIndirect
        && !(GetEnvironmentVariableW(L"WORLDFORGE_FIXED_INDIRECT",fixed,4) && fixed[0] == L'1');
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family_,1,&priority};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    if (meshExtension && api_.vkGetPhysicalDeviceFeatures2 && api_.vkGetPhysicalDeviceProperties2) {
        const VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&mesh};
        auto supportedMesh = query;
        api_.vkGetPhysicalDeviceFeatures2(physical_,&supportedMesh);
        VkPhysicalDeviceMeshShaderPropertiesEXT meshLimits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&meshLimits};
        api_.vkGetPhysicalDeviceProperties2(physical_,&limits);
        maxMeshDispatch_ = std::min(meshLimits.maxMeshWorkGroupCount[0],meshLimits.maxMeshWorkGroupTotalCount);
        meshShaders_ = mesh.meshShader && maxMeshDispatch_ && meshLimits.maxMeshWorkGroupInvocations >= 32
            && meshLimits.maxMeshOutputVertices >= meshletVertexLimit && meshLimits.maxMeshOutputPrimitives >= meshletTriangleLimit;
        mesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        mesh.meshShader = meshShaders_;
    }
    std::vector<const char *> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (indirectCount)
        extensions.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
    if (meshShaders_)
        extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    const VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,meshShaders_ ? &mesh : nullptr,0,1,&queue,0,nullptr,
        uint32_t(extensions.size()),extensions.data(),&features};
    if (api_.vkCreateDevice(physical_,&info,nullptr,&device) != VK_SUCCESS || !api_.loadDevice(device)) {
        error = L"Vulkan could not create a logical device.";
        return false;
    }
    api_.vkGetDeviceQueue(device,family_,0,&queue_);
    meshShaders_ = meshShaders_ && api_.vkCmdDrawMeshTasksEXT;
    device_.init(api_,physical_,device);
    probeCompute_ = device_.supportsCompute(family_);
    // Keep the direct draw path when compute or indirect resources are unavailable.
    if (probeCompute_)
        drawCulling_.create(device_,framesInFlight,features.multiDrawIndirect != VK_FALSE,indirectCount);
    wchar_t occlusion[4]{};
    if (probeCompute_ && GetEnvironmentVariableW(L"WORLDFORGE_OCCLUSION",occlusion,4) && occlusion[0] == L'1')
        occlusionDepth_.create(device_,framesInFlight);
    wchar_t streaming[4]{};
    streamTextures_ = GetEnvironmentVariableW(L"WORLDFORGE_TEXTURE_STREAMING",streaming,4) && streaming[0] == L'1';
    // GPU pass timing, when the queue writes timestamps.
    VkPhysicalDeviceProperties properties{};
    api_.vkGetPhysicalDeviceProperties(physical_,&properties);
    uint32_t familyCount = 0;
    api_.vkGetPhysicalDeviceQueueFamilyProperties(physical_,&familyCount,nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    api_.vkGetPhysicalDeviceQueueFamilyProperties(physical_,&familyCount,families.data());
    if (properties.limits.timestampPeriod > 0 && family_ < familyCount && families[family_].timestampValidBits > 0) {
        const VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,nullptr,0,VK_QUERY_TYPE_TIMESTAMP,
            framesInFlight*timestampsPerFrame,0};
        if (api_.vkCreateQueryPool(device,&queries,nullptr,&timestamps_) == VK_SUCCESS)
            timestampPeriod_ = properties.limits.timestampPeriod;
        else
            timestamps_ = VK_NULL_HANDLE;
    }
    return true;
}

bool VulkanRenderer::createLayouts()
{
    constexpr auto both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkShaderStageFlags geometryStages = both | (meshShaders_ ? uint32_t(VK_SHADER_STAGE_MESH_BIT_EXT) : 0u);
    const VkDescriptorSetLayoutBinding frameBindings[]{
        // FogVolume.comp reads the shadow map, the Lighting block, the lamps and their shadows.
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,geometryStages | VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,geometryStages,nullptr},
        {4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {7,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {8,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {9,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {vk::FogVolume::binding,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_MESH_BIT_EXT,nullptr},
        {11,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_MESH_BIT_EXT,nullptr}};
    std::array<VkDescriptorSetLayoutBinding,materialTextureCount> materialBindings{};
    for (uint32_t i = 0; i < materialBindings.size(); ++i)
        materialBindings[i] = {i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    const VkDescriptorSetLayoutCreateInfo frameInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,
        meshShaders_ ? 12u : 10u,frameBindings};
    const VkDescriptorSetLayoutCreateInfo materialInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,
        uint32_t(materialBindings.size()),materialBindings.data()};
    if (api_.vkCreateDescriptorSetLayout(device,&frameInfo,nullptr,&frameLayout_) != VK_SUCCESS
        || api_.vkCreateDescriptorSetLayout(device,&materialInfo,nullptr,&materialLayout_) != VK_SUCCESS)
        return false;
    const VkPushConstantRange camera{both,0,sizeof(CameraConstants)};
    const VkPushConstantRange shadowCamera{VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(CameraConstants)};
    const VkPushConstantRange material[]{{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(MaterialConstants)},
        {VK_SHADER_STAGE_MESH_BIT_EXT,sizeof(MaterialConstants),sizeof(uint32_t)}};
    const VkDescriptorSetLayout sceneSets[]{frameLayout_,materialLayout_};
    const VkPipelineLayoutCreateInfo sky{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,1,&frameLayout_,1,&camera};
    const VkPipelineLayoutCreateInfo scene{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,2,sceneSets,
        meshShaders_ ? 2u : 1u,material};
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
        {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | tests,tests,
         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0},
        {0,VK_SUBPASS_EXTERNAL,tests,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_SHADER_READ_BIT,0}};
    const VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,1,&depth,1,&subpass,2,dependencies};
    if (api_.vkCreateRenderPass(device,&pass,nullptr,&shadowPass_) != VK_SUCCESS)
        return false;
    auto updateDepth = depth;
    updateDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    updateDepth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    auto updatePass = pass;
    updatePass.pAttachments = &updateDepth;
    if (api_.vkCreateRenderPass(device,&updatePass,nullptr,&pointUpdatePass_) != VK_SUCCESS)
        pointUpdatePass_ = VK_NULL_HANDLE;

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
                                    VkPipelineLayout layout,VkRenderPass pass,bool scene,bool shadow,vk::MaterialPass materialPass,bool mesh)
{
    const auto vertex = device_.shader(vertexCode);
    const auto fragment = shadow ? VK_NULL_HANDLE : device_.shader(fragmentCode);
    VkPipeline result{};
    if (vertex && (fragment || shadow)) {
        const uint32_t mode = uint32_t(materialPass);
        const VkSpecializationMapEntry entry{1,0,sizeof(mode)};
        const VkSpecializationInfo specialization{1,&entry,sizeof(mode),&mode};
        const VkPipelineShaderStageCreateInfo stages[]{
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
                mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,vertex,"main",nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fragment,"main",
                scene ? &specialization : nullptr}};
        static_assert(sizeof(PlayVertex) == 48);
        // PlayScene.vert also reads the previous frame's buffer (vertexRing_) as each vertex's previous position.
        const VkVertexInputBindingDescription bindings[]{{0,sizeof(PlayVertex),VK_VERTEX_INPUT_RATE_VERTEX},
            {1,sizeof(PlayVertex),VK_VERTEX_INPUT_RATE_VERTEX}};
        const VkVertexInputAttributeDescription attributes[]{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,position)},
            {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,normal)},
            {2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,color)},
            {3,0,VK_FORMAT_R32_SFLOAT,offsetof(PlayVertex,wind)},
            {4,0,VK_FORMAT_R32G32_SFLOAT,offsetof(PlayVertex,uv)},
            {5,1,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,position)}};
        // Shadow.vert reads position at location 0 and wind at location 2.
        const VkVertexInputAttributeDescription shadowAttributes[]{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(PlayVertex,position)},
            {2,0,VK_FORMAT_R32_SFLOAT,offsetof(PlayVertex,wind)}};
        const bool geometry = scene || shadow;
        const VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,nullptr,0,
            shadow ? 1u : scene ? 2u : 0u,bindings,shadow ? 2u : scene ? 6u : 0u,shadow ? shadowAttributes : attributes};
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
        depth.depthWriteEnable = geometry && materialPass == vk::MaterialPass::Opaque ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
            | VK_COLOR_COMPONENT_A_BIT;
        blend = vk::materialBlend(materialPass,blend);
        // The scene pass has targets for the sky-light part, the surface albedo and motion, which only the scene shader writes.
        VkPipelineColorBlendAttachmentState blends[vk::HdrFrame::sceneColorAttachments]{blend,blend,blend,blend};
        if (!geometry)
            blends[1].colorWriteMask = blends[2].colorWriteMask = blends[3].colorWriteMask = 0;
        VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colors.attachmentCount = vk::HdrFrame::sceneColorAttachments;
        colors.pAttachments = blends;
        const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,nullptr,0,2,states};
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = shadow ? 1 : 2;
        info.pStages = stages;
        info.pVertexInputState = mesh ? nullptr : &input;
        info.pInputAssemblyState = mesh ? nullptr : &assembly;
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
    glassTransmission_ = pipeline(spirv(spirv_PlayScene_vert),spirv(spirv_PlayScene_frag),sceneLayout_,frame_.scenePass(),
                                  true,false,vk::MaterialPass::Transmission);
    glassRadiance_ = pipeline(spirv(spirv_PlayScene_vert),spirv(spirv_PlayScene_frag),sceneLayout_,frame_.scenePass(),
                              true,false,vk::MaterialPass::Radiance);
    shadowPipeline_ = pipeline(spirv(spirv_Shadow_vert),{},shadowLayout_,shadowPass_,false,true);
    if (meshShaders_)
        meshScene_ = pipeline(spirv(spirv_PlayMeshlets_mesh),spirv(spirv_PlayScene_frag),sceneLayout_,frame_.scenePass(),
                              true,false,vk::MaterialPass::Opaque,true);
    return sky_ && scene_ && shadowPipeline_;
}

bool VulkanRenderer::createFrames()
{
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,5*framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2*framesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,(meshShaders_ ? 5u : 3u)*framesInFlight}};
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
        const auto fogVolume = fogVolume_.descriptor();
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&shadow,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,1,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&lighting,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,3,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&view,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,4,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&skyMaps,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,5,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&environment,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,6,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&clusters,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,7,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&pointShadows,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,vk::FogVolume::binding,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&fogVolume,nullptr,nullptr}};
        api_.vkUpdateDescriptorSets(device,8,writes,0,nullptr);
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
    MaterialTexture texture;
    constexpr UINT tailSide = 64;
    const auto size = std::max(image.width,image.height);
    if (streamTextures_ && size > tailSide && image.pixels.size()*6 <= textureDetailBudget) {
        const UINT shrink = std::bit_ceil((size+tailSide-1)/tailSide);
        Image tail{std::max(1u,image.width/shrink),std::max(1u,image.height/shrink),{}};
        tail.pixels.resize(std::size_t(tail.width)*tail.height);
        for (UINT y = 0; y < tail.height; ++y)
            for (UINT x = 0; x < tail.width; ++x) {
                const UINT left = x*image.width/tail.width, right = (x+1)*image.width/tail.width;
                const UINT top = y*image.height/tail.height, bottom = (y+1)*image.height/tail.height;
                std::array<uint64_t,4> sum{};
                for (UINT row = top; row < bottom; ++row)
                    for (UINT col = left; col < right; ++col) {
                        const auto pixel = image.pixels[std::size_t(row)*image.width+col];
                        for (uint32_t channel = 0; channel < sum.size(); ++channel)
                            sum[channel] += (pixel >> (channel*8)) & 255u;
                    }
                const auto count = uint64_t(right-left)*(bottom-top);
                uint32_t pixel = 0;
                for (uint32_t channel = 0; channel < sum.size(); ++channel)
                    pixel |= uint32_t((sum[channel]+count/2)/count) << (channel*8);
                tail.pixels[std::size_t(y)*tail.width+x] = pixel;
            }
        if (!uploadTexture(tail,texture.tail))
            return -1;
        texture.source = image;
        texture.detailBytes = image.pixels.size()*6; // upper bound for RGBA8 with all mip levels
    } else if (!uploadTexture(image,texture.tail)) {
        return -1;
    }
    textures_.push_back(std::move(texture));
    return int(textures_.size()-1);
}

bool VulkanRenderer::uploadTexture(const Image &image,vk::Image &texture)
{
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const bool blits = device_.supports(format,VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    const uint32_t levels = blits ? uint32_t(std::floor(std::log2(double(std::max(image.width,image.height)))))+1 : 1;
    vk::Buffer staging;
    const auto bytes = VkDeviceSize(image.pixels.size()*4);
    if (!texture.create(device_,format,{image.width,image.height},
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT,levels)
        || !staging.create(device_,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        texture.destroy(device_);
        return false;
    }
    std::memcpy(staging.mapped,image.pixels.data(),bytes);
    const auto command = beginOneTime();
    if (!command) {
        staging.destroy(device_);
        texture.destroy(device_);
        return false;
    }
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
    const bool uploaded = submitAndWait(command);
    staging.destroy(device_);
    if (!uploaded)
        texture.destroy(device_);
    return uploaded;
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
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,sets*uint32_t(materialTextureCount)};
        const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,sets,1,&size};
        VkDescriptorPool created{};
        if (api_.vkCreateDescriptorPool(device,&pool,nullptr,&created) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        materialPools_.push_back(created);
        allocation.descriptorPool = created;
        if (api_.vkAllocateDescriptorSets(device,&allocation,&set) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }
    std::array<VkDescriptorImageInfo,materialTextureCount> images{};
    std::array<VkWriteDescriptorSet,materialTextureCount> writes{};
    for (std::size_t i = 0; i < images.size(); ++i) {
        const int index = material.textures[i] >= 0 && std::size_t(material.textures[i]) < textures_.size()
            ? material.textures[i] : white_;
        images[i] = {textureSampler_,textures_[std::size_t(index)].view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,uint32_t(i),0,1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&images[i],nullptr,nullptr};
    }
    api_.vkUpdateDescriptorSets(device,uint32_t(writes.size()),writes.data(),0,nullptr);
    materialSets_.emplace(material.textures,set);
    return set;
}

void VulkanRenderer::refreshTextureDescriptors(int index)
{
    const VkDescriptorImageInfo image{textureSampler_,textures_[index].view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (const auto &[maps,set] : materialSets_)
        for (uint32_t binding = 0; binding < maps.size(); ++binding)
            if (maps[binding] == index) {
                const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,binding,0,1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&image,nullptr,nullptr};
                api_.vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            }
}

void VulkanRenderer::requestTextureDetail(std::span<const PlayVertex> vertices,std::span<const DrawRange> ranges,const View &view,
                                          UINT width,UINT height,bool capture)
{
    texturesPromoted_ = textureWaitFrames_ = 0;
    textureStallMs_ = 0;
    if (!streamTextures_ || std::ranges::none_of(textures_,[](const auto &texture) { return texture.detailBytes && !texture.detail.view; }))
        return;
    ++textureFrame_;
    std::vector<float> demand(textures_.size());
    const auto clip = XMLoadFloat4x4(&view.viewProjection);
    const auto streamable = [&](int index) { return index >= 0 && std::size_t(index) < textures_.size() && textures_[index].detailBytes; };
    const float focalScale = 1/std::tan(view.lens.w*0.5f);
    for (const auto &range : ranges) {
        if (!range.count || std::size_t(range.start)+range.count > vertices.size()
            || std::ranges::none_of(range.material.textures,streamable))
            continue;
        const auto bounds = vk::drawBounds(vertices,range.start,range.count);
        const auto &c = bounds.center;
        const auto projected = XMVector4Transform(XMVectorSet(c[0],c[1],c[2],1),clip);
        const auto &e = bounds.extent;
        const float radius = std::sqrt(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]);
        const float w = XMVectorGetW(projected);
        const float spanY = radius*focalScale, spanX = spanY*float(height)/float(std::max(width,1u));
        if (w+radius <= 0 || std::abs(XMVectorGetX(projected)) > std::max(w,0.0f)+radius+spanX
            || std::abs(XMVectorGetY(projected)) > std::max(w,0.0f)+radius+spanY)
            continue;
        const float pixels = spanY*float(height)/std::max(w-radius,0.05f);
        for (const int index : range.material.textures)
            if (streamable(index))
                demand[index] = std::max(demand[index],pixels);
    }
    std::vector<int> requests;
    constexpr float promoteAt = 80;
    for (int i = 0; i < int(textures_.size()); ++i) {
        auto &texture = textures_[i];
        if (!texture.detailBytes || demand[i] < promoteAt) {
            texture.requestedSince = 0;
            continue;
        }
        texture.lastVisible = textureFrame_;
        if (!texture.detail.view) {
            if (!texture.requestedSince)
                texture.requestedSince = textureFrame_;
            textureWaitFrames_ = std::max(textureWaitFrames_,textureFrame_-texture.requestedSince);
            requests.push_back(i);
        }
    }
    std::ranges::sort(requests,[&](int a,int b) {
        if (textures_[a].requestedSince != textures_[b].requestedSince)
            return textures_[a].requestedSince < textures_[b].requestedSince;
        return demand[a] > demand[b];
    });
    const auto start = std::chrono::steady_clock::now();
    for (const int index : requests) {
        auto &texture = textures_[index];
        if (textureDetailBytes_+texture.detailBytes > textureDetailBudget) {
            // Reclaim detail from textures that have been out of view for at least half a second at 60 fps.
            std::vector<int> evictable;
            for (int i = 0; i < int(textures_.size()); ++i)
                if (textures_[i].detail.view && textureFrame_-textures_[i].lastVisible > 30)
                    evictable.push_back(i);
            std::ranges::sort(evictable,[&](int a,int b) { return textures_[a].lastVisible < textures_[b].lastVisible; });
            if (!evictable.empty() && api_.vkQueueWaitIdle(queue_) != VK_SUCCESS)
                break;
            for (const int i : evictable) {
                auto &old = textures_[i];
                old.detail.destroy(device_);
                textureDetailBytes_ -= old.detailBytes;
                refreshTextureDescriptors(i);
                if (textureDetailBytes_+texture.detailBytes <= textureDetailBudget)
                    break;
            }
        }
        if (textureDetailBytes_+texture.detailBytes > textureDetailBudget || !uploadTexture(texture.source,texture.detail))
            continue;
        textureDetailBytes_ += texture.detailBytes;
        texture.requestedSince = 0;
        ++texturesPromoted_;
        refreshTextureDescriptors(index);
        if (!capture)
            break;
    }
    if (!requests.empty())
        textureStallMs_ = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}

void VulkanRenderer::prepareMeshlets(Frame &frame,std::span<const PlayVertex> vertices,std::span<const DrawRange> ranges,const View &view)
{
    frame.meshletsBound = false;
    constexpr auto maxStorageBytes = std::size_t(128)*1024*1024;
    if (!useMeshlets_ || !view.drawCulling || !view.movingVertices || !view.cachedMeshVertices
        || view.cachedMeshVertices > vertices.size() || view.cachedMeshVertices*sizeof(PlayVertex) > maxStorageBytes)
        return;
    if (!meshletsPrepared_) {
        meshletsPrepared_ = true;
        meshletRanges_.resize(ranges.size());
        std::vector<Meshlet> meshlets;
        std::vector<uint32_t> indices;
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            const auto &range = ranges[i];
            const auto end = std::size_t(range.start)+range.count;
            // Animated topology and blended materials keep their original, ordered draw path.
            if (!range.count || range.count%3 != 0 || end > view.cachedMeshVertices || range.material.layers.isTransparent()
                || std::ranges::any_of(*view.movingVertices,[&](const VertexUpload &update) {
                    return update.first < end && update.first+update.count > range.start;
                }))
                continue;
            if (meshlets.size()+std::size_t(range.count)/3 > maxStorageBytes/sizeof(Meshlet))
                break;
            auto &output = meshletRanges_[i];
            output.firstMeshlet = uint32_t(meshlets.size());
            output.firstIndex = uint32_t(indices.size());
            appendMeshlets(meshlets,vertices,range.start,range.count);
            output.meshlets = uint32_t(meshlets.size())-output.firstMeshlet;
            for (const auto &meshlet : std::span(meshlets).subspan(output.firstMeshlet))
                for (uint32_t triangle = 0; triangle < meshlet.counts[1]; ++triangle)
                    for (uint32_t part = 0; part < 3; ++part)
                        indices.push_back(meshlet.vertices[(meshlet.triangles[triangle] >> (6*part)) & 63u]);
        }
        if (meshlets.empty())
            return;
        if (!meshletIndices_.create(device_,indices.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
            return;
        std::memcpy(meshletIndices_.mapped,indices.data(),indices.size()*sizeof(uint32_t));
        if (meshScene_ && meshletData_.create(device_,meshlets.size()*sizeof(Meshlet),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
            std::memcpy(meshletData_.mapped,meshlets.data(),meshlets.size()*sizeof(Meshlet));
    }
    if (!meshletIndices_.buffer)
        return;
    frame.meshletsBound = true;
    if (meshletData_.buffer) {
        const VkDescriptorBufferInfo storage[]{{meshletData_.buffer,0,meshletData_.size},
            {vertexRing_[ring_].buffer,0,view.cachedMeshVertices*sizeof(PlayVertex)}};
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,10,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&storage[0],nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,11,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&storage[1],nullptr}};
        api_.vkUpdateDescriptorSets(device,2,writes,0,nullptr);
    }
}

bool VulkanRenderer::prepare(Frame &frame,const std::vector<PlayVertex> &vertices,const View &view,UINT width,UINT height)
{
    const auto bytes = VkDeviceSize(vertices.size()*sizeof(PlayVertex));
    if (bytes > 240000000)
        return false;
    // The next buffer in the ring (vertexRing_); the one before it holds the previous frame's positions.
    ring_ = (ring_+1)%vertexRing_.size();
    auto &buffer = vertexRing_[ring_];
    // Always at least a little, so the scene pipeline has a buffer to bind.
    if (bytes > buffer.size || !buffer.buffer) {
        buffer.destroy(device_);
        ringCachedMeshVertices_[ring_] = 0;
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (meshScene_ ? uint32_t(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) : 0u);
        if (!buffer.create(device_,std::max<VkDeviceSize>(bytes+bytes/4,4096),usage))
            return false;
    }
    const auto upload = [&](std::size_t first,std::size_t count) {
        if (count)
            std::memcpy(static_cast<PlayVertex *>(buffer.mapped)+first,vertices.data()+first,count*sizeof(PlayVertex));
    };
    if (view.movingVertices && view.cachedMeshVertices == ringCachedMeshVertices_[ring_]) {
        for (const auto &range : *view.movingVertices)
            upload(range.first,range.count);
        upload(view.cachedMeshVertices,vertices.size()-view.cachedMeshVertices);
    } else {
        upload(0,vertices.size());
        ringCachedMeshVertices_[ring_] = view.cachedMeshVertices;
    }
    ringCounts_[ring_] = vertices.size();
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
    lighting.skySettings = {view.skyMaps && view.sky.z > 0.5f ? 1.0f : 0.0f,view.skyVisibility.metersPerTexel,view.sky.w,view.weather.x};
    std::copy(std::begin(view.skyIrradiance),std::end(view.skyIrradiance),std::begin(lighting.skyIrradiance));
    std::copy(std::begin(view.skyIrradianceBand4),std::end(view.skyIrradianceBand4),std::begin(lighting.skyIrradianceBand4));
    lighting.skyHorizon = view.skyHorizon;
    lighting.skyZenith = view.skyZenith;
    lighting.skyGround = view.skyGround;
    lighting.sunTint = view.sunTint;
    lighting.sunTint.w = view.weather.y;
    // Screen-space reflections and contact shadows trace the previous frame (View::shutter z and w).
    const bool reflections = view.shutter.z > 0.5f && reflectionReady_;
    const bool contactShadows = view.shutter.w > 0.5f && view.lighting.y > 0 && reflectionReady_;
    // The previous camera also gives each surface's motion (SceneMotion.glsl), so it is always set.
    XMStoreFloat4x4(&lighting.previousViewProjection,XMLoadFloat4x4(&reflectionClip_));
    lighting.reflectionSettings = {reflections ? 1.0f : 0.0f,vk::HdrFrame::reflectionRange,contactShadows ? 1.0f : 0.0f,
                                   motionWindTime_};
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
                                  matrix,eye,{forward.x,forward.y,forward.z},pointShadowsRendered_,clusterExposure_);
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

void VulkanRenderer::updateProbes(Frame &frame,std::span<const PlayVertex> vertices,std::span<const DrawRange> ranges,const View &view)
{
    auto *data = static_cast<std::byte *>(frame.lighting.mapped);
    LightingBlock lighting;
    std::memcpy(&lighting,data,sizeof(lighting));
    const auto &forward = lighting.clusterForward;
    const auto volume = probes::Volume::around({view.eye.x+6*forward.x,view.eye.y+6*forward.y,view.eye.z+6*forward.z});
    // The sky and the lamps light the surfaces the probe rays hit, so walls get the light the street bounces onto them.
    std::array<std::array<float,4>,9> sky{};
    for (std::size_t i = 0; i < sky.size(); ++i)
        sky[i] = {lighting.skyIrradiance[i].x,lighting.skyIrradiance[i].y,lighting.skyIrradiance[i].z,0};
    const auto hit = probes::hitLighting(sky,lights::probeSources(view.sceneLights ? std::span<const lights::Light>(*view.sceneLights)
        : std::span<const lights::Light>{}),volume);
    const auto &sun = lighting.sun;
    const auto &tint = lighting.sunTint;
    if (probeField_.needsUpdate(probeCompute_ && (view.probeGi || view.reflectionProbes),view.reflectionProbes,volume,0,
            {sun.x,sun.y,sun.z},{sun.w*tint.x,sun.w*tint.y,sun.w*tint.z},view.movingGeometry != 0,hit)) {
        std::vector<probes::SurfaceTriangle> surfaces;
        for (const auto &draw : ranges) {
            if (draw.material.layers.isTransparent() || std::size_t(draw.start)+draw.count > vertices.size())
                continue;
            const auto range = vertices.subspan(draw.start,draw.count);
            const float diffuse = 1-draw.material.metallic;
            for (std::size_t i = 0; i+2 < range.size() && surfaces.size() < probes::Volume::triangleBudget; i += 3) {
                probes::SurfaceTriangle surface{{range[i].position,range[i+1].position,range[i+2].position},{},
                    draw.material.emissive};
                if (!volume.intersects(surface.vertices))
                    continue;
                const auto linear = color::srgbToLinear(range[i].color);
                for (int c = 0; c < 3; ++c)
                    surface.diffuse[c] = std::clamp(linear[c]*diffuse,0.f,1.f);
                surfaces.push_back(surface);
            }
        }
        probeField_.update(device_,frame.command,surfaces);
    }
    frame.probes = probeField_.trace();
    const auto grid = probeField_.grid();
    // w: how far the fog volume reaches, when this frame fills it.
    const float mist[4]{view.fogCenterRadius.x,view.fogCenterRadius.y,view.fogCenterRadius.z,view.fogCenterRadius.w};
    const float eye[3]{view.eye.x,view.eye.y,view.eye.z};
    fogVolumeNeeded_ = fogVolume_.ready() && vk::FogVolume::needed(view.fog.w,view.lighting.y > 0.5f,view.sun.w,mist,view.lighting.x);
    const std::array settings{view.probeGi ? 1.f : 0.f,view.reflectionProbes ? 1.f : 0.f,view.sky.y > 0.5f ? 1.f : 0.f,
        fogVolumeNeeded_ ? vk::FogVolume::reach(view.cascades.scales[3][2],eye,mist) : 0.f};
    std::memcpy(data+offsetof(LightingBlock,probeGrid),grid.data(),sizeof(grid));
    std::memcpy(data+offsetof(LightingBlock,probeSettings),settings.data(),sizeof(settings));
    const VkDescriptorBufferInfo info{frame.probes ? frame.probes->output().buffer : frame.clusters.buffer,0,
        frame.probes ? frame.probes->output().size : VkDeviceSize(16)};
    const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,9,0,1,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&info,nullptr};
    api_.vkUpdateDescriptorSets(device,1,&write,0,nullptr);
}

void VulkanRenderer::record(Frame &frame,vk::HdrFrame &target,std::size_t output,const std::vector<DrawRange> &ranges,
                            const View &view,bool sky,std::span<const PlayVertex> vertices,float exposureStops,int meterSlot,
                            const vk::HdrFrame::Temporal &temporal,const vk::HdrFrame::Occlusion &occlusion)
{
    stats_ = {};
    // GPU timing: a timestamp before the frame (probe tracing counts toward the sun shadows) and after each pass.
    const auto slot = uint32_t(&frame-frames_.data());
    const uint32_t firstQuery = slot*timestampsPerFrame;
    if (timestamps_) {
        api_.vkCmdResetQueryPool(frame.command,timestamps_,firstQuery,timestampsPerFrame);
        api_.vkCmdWriteTimestamp(frame.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps_,firstQuery);
    }
    const auto stamp = [&](RenderStats::Pass pass) {
        if (timestamps_)
            api_.vkCmdWriteTimestamp(frame.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps_,firstQuery+1+pass);
    };
    // Before anything binds the set: the previous frame's TAA result, which alternates between two images.
    const VkDescriptorImageInfo previousFrame{target.nearestSampler(),target.previousFrame(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet previousWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,8,0,1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&previousFrame,nullptr,nullptr};
    api_.vkUpdateDescriptorSets(device,1,&previousWrite,0,nullptr);
    updateProbes(frame,vertices,ranges,view);
    const auto command = frame.command;
    const VkDeviceSize offset = 0;
    const auto vertexCount = vertices.size();
    const bool transparencyAvailable = glassTransmission_ && glassRadiance_;
    const auto isTransparent = [transparencyAvailable](const DrawRange &range) {
        return transparencyAvailable && range.material.layers.isTransparent();
    };
    std::vector<vk::TriangleRange> transparent;
    std::vector<const DrawRange *> blended;
    for (const auto &range : ranges)
        if (isTransparent(range) && range.count >= 3 && std::size_t(range.start)+range.count <= vertexCount) {
            transparent.push_back({range.start,range.count});
            blended.push_back(&range);
        }
    std::ranges::sort(transparent,{},&vk::TriangleRange::first);
    bool culled = false;
    frame.cullingInputDraws = 0;
    frame.cullingInputTriangles = 0;
    if (view.drawCulling && ranges.size() >= 64) {
        cullBatches_.resize(ranges.size());
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            const auto &range = ranges[i];
            auto &batch = cullBatches_[i];
            if (isTransparent(range) || !range.count || std::size_t(range.start)+range.count > vertexCount) {
                batch = {};
                continue;
            }
            const auto end = std::size_t(range.start)+range.count;
            const bool moves = !view.movingVertices || end > view.cachedMeshVertices
                || std::ranges::any_of(*view.movingVertices,[&](const VertexUpload &update) {
                    return update.first < end && update.first+update.count > range.start;
                });
            if (moves || batch.vertices[0] != range.start || batch.vertices[1] != range.count)
                batch = vk::drawBounds(vertices,range.start,range.count);
            ++frame.cullingInputDraws;
            frame.cullingInputTriangles += range.count/3;
        }
        std::array<std::array<float,16>,1+shadow::cascadeCount> matrices{};
        const auto camera = vulkanClip(view.viewProjection);
        std::memcpy(matrices[0].data(),&camera,sizeof(camera));
        std::ranges::copy(view.cascades.matrices,matrices.begin()+1);
        const vk::Buffer *pyramid = nullptr;
        const auto size = target.extent();
        if (ranges.size() >= 128 && occlusionDepth_.begin(command,slot,size,target.depthFormat(),shadowPass_)) {
            const auto seeds = vk::occlusionSeeds(cullBatches_,drawCulling_.commands(slot,0),matrices[0],{view.eye.x,view.eye.y,view.eye.z});
            api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline_);
            api_.vkCmdBindVertexBuffers(command,0,1,&vertexRing_[ring_].buffer,&offset);
            CameraConstants seedCamera{};
            seedCamera.matrix = camera;
            seedCamera.eye.w = view.lighting.z;
            api_.vkCmdPushConstants(command,shadowLayout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(seedCamera),&seedCamera);
            for (const auto seed : seeds) {
                const auto &batch = cullBatches_[seed].vertices;
                api_.vkCmdDraw(command,batch[1],1,batch[0],0);
            }
            pyramid = &occlusionDepth_.finish(command,slot);
        }
        // Camera ranges still bind individual materials. Only the sun cascades share draw state across the whole list.
        culled = drawCulling_.record(command,slot,cullBatches_,std::span(matrices).first(view.lighting.y > 0 ? matrices.size() : 1),
                                    1,pyramid,size);
        stats_.compactedShadowDraws = culled && view.lighting.y > 0 && drawCulling_.compacted(slot);
        if (!culled) {
            frame.cullingInputDraws = 0;
            frame.cullingInputTriangles = 0;
        }
    }
    const auto drawShadowCasters = [&] {
        vk::drawOpaqueRanges(vertexCount,transparent,[&](uint32_t first,uint32_t count) {
            api_.vkCmdDraw(command,count,1,first,0);
            ++stats_.depthDraws;
            stats_.depthTriangles += count/3;
        });
    };
    // Shadow map: always cleared so it is ready to sample; filled when sun shadows are on.
    const VkClearValue clearDepth{.depthStencil = {1,0}};
    const VkRect2D shadowArea{{0,0},{shadowSize,shadowSize}};
    const VkRenderPassBeginInfo shadowBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,shadowPass_,
        frame.shadowFramebuffer,shadowArea,1,&clearDepth};
    api_.vkCmdBeginRenderPass(command,&shadowBegin,VK_SUBPASS_CONTENTS_INLINE);
    if (view.lighting.y > 0 && vertexCount) {
        api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline_);
        api_.vkCmdBindVertexBuffers(command,0,1,&vertexRing_[ring_].buffer,&offset);
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
            if (culled) {
                drawCulling_.draw(command,slot,uint32_t(cascade+1),0,uint32_t(ranges.size()));
                ++stats_.depthDraws;
                stats_.depthTriangles += frame.cullingInputTriangles;
            } else
                drawShadowCasters();
        }
    }
    api_.vkCmdEndRenderPass(command);
    stamp(RenderStats::SunShadows);

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
            api_.vkCmdBindVertexBuffers(command,0,1,&vertexRing_[ring_].buffer,&offset);
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
                drawShadowCasters();
            }
        }
        api_.vkCmdEndRenderPass(command);
    }
    stamp(RenderStats::SkyMaps);


    // Refresh changed cubemaps immediately. The periodic refresh also accounts for moving casters.
    const bool refreshAll = !pointShadowsInitialized_ || (!clusters_.shadowed.empty() && ++pointShadowAge_ >= skyRefreshFrames);
    const auto dirtySlots = lights::shadowUpdates(clusters_.shadowed,pointShadowsRendered_,refreshAll);
    if (refreshAll || std::ranges::any_of(dirtySlots,std::identity{})) {
        pointShadowsInitialized_ = true;
        const bool clearAll = refreshAll || !pointUpdatePass_;
        if (clearAll)
            pointShadowAge_ = 0;
        const VkRect2D atlas{{0,0},{lights::shadowAtlasWidth,lights::shadowAtlasHeight}};
        const VkRenderPassBeginInfo pointBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,
            clearAll ? shadowPass_ : pointUpdatePass_,pointShadowFramebuffer_,atlas,clearAll ? 1u : 0u,&clearDepth};
        api_.vkCmdBeginRenderPass(command,&pointBegin,VK_SUBPASS_CONTENTS_INLINE);
        if (vertexCount && !clusters_.shadowed.empty()) {
            api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline_);
            api_.vkCmdBindVertexBuffers(command,0,1,&vertexRing_[ring_].buffer,&offset);
            for (std::size_t tile = 0; tile < clusters_.shadowed.size()*6; ++tile) {
                if (!clearAll && !dirtySlots[tile/6])
                    continue;
                const auto x = uint32_t(tile%lights::shadowColumns)*lights::faceSize;
                const auto y = uint32_t(tile/lights::shadowColumns)*lights::faceSize;
                const VkViewport viewport{float(x),float(y),float(lights::faceSize),float(lights::faceSize),0,1};
                const VkRect2D face{{int32_t(x),int32_t(y)},{lights::faceSize,lights::faceSize}};
                if (!clearAll) {
                    const VkClearAttachment attachment{VK_IMAGE_ASPECT_DEPTH_BIT,0,clearDepth};
                    const VkClearRect region{face,0,1};
                    api_.vkCmdClearAttachments(command,1,&attachment,1,&region);
                }
                api_.vkCmdSetViewport(command,0,1,&viewport);
                api_.vkCmdSetScissor(command,0,1,&face);
                CameraConstants camera{};
                std::memcpy(&camera.matrix,clusters_.shadowFaces[tile].data(),sizeof(camera.matrix));
                camera.eye.w = view.lighting.z;
                api_.vkCmdPushConstants(command,shadowLayout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(camera),&camera);
                drawShadowCasters();
                ++stats_.pointShadowFaces;
            }
        }
        api_.vkCmdEndRenderPass(command);
    }
    pointShadowsRendered_ = clusters_.shadowed;

    // The fog volume reads the shadow maps drawn above.
    if (fogVolumeNeeded_) {
        vk::FogVolume::View volumeView;
        volumeView.inverseViewProjection = occlusion.inverseViewProjection;
        volumeView.eye = {view.eye.x,view.eye.y,view.eye.z,0};
        volumeView.fog = {view.fog.w,view.horizon.w,view.zenith.w,0};
        fogVolume_.record(command,frame.set,volumeView);
    } else {
        fogVolume_.prepare(command);
    }
    stamp(RenderStats::PointShadows);
    const auto background = color::srgbToLinear({0.05f,0.06f,0.07f});
    target.begin(command,output,background.data());
    VkPipeline boundScene = scene_;
    const auto drawRange = [&](const DrawRange &range,bool transmissive) {
        const auto set = materialSet(range.material);
        if (!set || std::size_t(range.start)+range.count > vertexCount)
            return;
        const auto &material = range.material;
        uint32_t textureFlags = 0;
        for (std::size_t i = 0; i < material.textures.size(); ++i)
            if (material.textures[i] >= 0 && std::size_t(material.textures[i]) < textures_.size())
                textureFlags |= 1u << i;
        const MaterialConstants constants{{material.emissive[0],material.emissive[1],material.emissive[2],0},
            {material.metallic,material.roughness,float(material.advanced),float(material.textures[1] >= 0)},
            {material.layers.sheenColor[0],material.layers.sheenColor[1],material.layers.sheenColor[2],
                material.layers.sheenRoughness},
            {material.layers.subsurface,material.layers.thickness,material.layers.scatteringDistance,material.layers.anisotropy},
            {material.clearCoat,material.clearCoatRoughness},{material.layers.transmission,material.layers.ior},
            material.layers.anisotropyRotation,material.layers.coatNormalScale,textureFlags,0,
            {material.layers.wetness,material.layers.puddles,material.layers.heightScale,
                material.layers.decal ? material.layers.decalOpacity*material.surfaceAlpha : -1.0f}};
        api_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,sceneLayout_,1,1,&set,0,nullptr);
        api_.vkCmdPushConstants(command,sceneLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),&constants);
        if (transmissive) {
            for (const auto pipeline : {glassTransmission_,glassRadiance_}) {
                api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
                api_.vkCmdDraw(command,range.count,1,range.start,0);
                ++stats_.sceneDraws;
                stats_.sceneTriangles += range.count/3;
            }
        } else {
            const auto index = std::size_t(&range-ranges.data());
            const MeshletRange *meshlets = frame.meshletsBound && index < meshletRanges_.size() && meshletRanges_[index].meshlets
                ? &meshletRanges_[index] : nullptr;
            const auto wanted = meshlets && meshletData_.buffer ? meshScene_ : scene_;
            if (wanted != boundScene) {
                api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,wanted);
                boundScene = wanted;
            }
            if (meshlets && wanted == meshScene_) {
                uint32_t left = meshlets->meshlets;
                uint32_t first = meshlets->firstMeshlet;
                while (left) {
                    const uint32_t count = std::min(left,maxMeshDispatch_);
                    api_.vkCmdPushConstants(command,sceneLayout_,VK_SHADER_STAGE_MESH_BIT_EXT,sizeof(MaterialConstants),sizeof(first),&first);
                    api_.vkCmdDrawMeshTasksEXT(command,count,1,1);
                    first += count;
                    left -= count;
                }
                stats_.meshletShaderGroups += meshlets->meshlets;
            } else if (meshlets) {
                api_.vkCmdDrawIndexed(command,range.count,1,meshlets->firstIndex,0,0);
                ++stats_.meshletIndexedDraws;
            } else if (culled)
                drawCulling_.draw(command,slot,0,uint32_t(&range-ranges.data()),1);
            else
                api_.vkCmdDraw(command,range.count,1,range.start,0);
            ++stats_.sceneDraws;
            stats_.sceneTriangles += range.count/3;
        }
    };
    if (vertexCount) {
        api_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,scene_);
        api_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,sceneLayout_,0,1,&frame.set,0,nullptr);
        // Binding 1: the previous frame's buffer as each vertex's previous position, when it holds the same vertices.
        const std::size_t previous = (ring_+vertexRing_.size()-1)%vertexRing_.size();
        const bool moving = vertexMotion_ && vertexRing_[previous].buffer && ringCounts_[previous] == vertexCount;
        const VkBuffer buffers[]{vertexRing_[ring_].buffer,moving ? vertexRing_[previous].buffer : vertexRing_[ring_].buffer};
        const VkDeviceSize offsets[]{0,0};
        api_.vkCmdBindVertexBuffers(command,0,2,buffers,offsets);
        if (frame.meshletsBound)
            api_.vkCmdBindIndexBuffer(command,meshletIndices_.buffer,0,VK_INDEX_TYPE_UINT32);
        // Nearest first, so the depth test rejects most hidden fragments before they run the shading (the fog
        // marches, shadows and lights). The culling bounds give each range's box; without culling, the boxes are kept
        // here, recomputed only for ranges that changed or move.
        opaqueOrder_.clear();
        for (std::size_t i = 0; i < ranges.size(); ++i)
            if (!isTransparent(ranges[i]))
                opaqueOrder_.push_back({0.0f,uint32_t(i)});
        const bool cullBounds = culled && cullBatches_.size() == ranges.size();
        if (!cullBounds) {
            orderBounds_.resize(ranges.size());
            for (const auto &[distance,index] : opaqueOrder_) {
                const auto &range = ranges[index];
                const auto end = std::size_t(range.start)+range.count;
                if (!range.count || end > vertexCount)
                    continue;
                auto &bounds = orderBounds_[index];
                const bool moves = !view.movingVertices || end > view.cachedMeshVertices
                    || std::ranges::any_of(*view.movingVertices,[&](const VertexUpload &update) {
                        return update.first < end && update.first+update.count > range.start;
                    });
                if (moves || bounds.vertices[0] != range.start || bounds.vertices[1] != range.count)
                    bounds = vk::drawBounds(vertices,range.start,range.count);
            }
        }
        const auto &boxes = cullBounds ? cullBatches_ : orderBounds_;
        {
            for (auto &[distance,index] : opaqueOrder_) {
                const auto &bounds = boxes[index];
                const float eye[3]{view.eye.x,view.eye.y,view.eye.z};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float gap = std::max(std::abs(eye[axis]-bounds.center[axis])-bounds.extent[axis],0.0f);
                    distance += gap*gap;
                }
            }
        }
        std::ranges::stable_sort(opaqueOrder_,{},&std::pair<float,uint32_t>::first);
        for (const auto &entry : opaqueOrder_)
            drawRange(ranges[entry.second],false);
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
    if (!blended.empty()) {
        const auto depth = [&](const DrawRange *range) {
            const auto &a = vertices[range->start].position;
            const auto &b = vertices[range->start+1].position;
            const auto &c = vertices[range->start+2].position;
            const auto &m = view.viewProjection;
            return m._14*(a[0]+b[0]+c[0])+m._24*(a[1]+b[1]+c[1])+m._34*(a[2]+b[2]+c[2]);
        };
        std::ranges::stable_sort(blended,std::greater<>{},depth);
        api_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,sceneLayout_,0,1,&frame.set,0,nullptr);
        for (const auto *range : blended)
            drawRange(*range,true);
    }
    stamp(RenderStats::Scene);
    auto ambient = occlusion;
    ambient.probeGi = view.probeGi && frame.probes;
    target.display(command,exposureStops,view.display.y > 0.5f ? color::ViewTransform::AgX : color::ViewTransform::Standard,
                   meterSlot,temporal,ambient,view.bloom,cameraEffects(view));
    api_.vkCmdEndRenderPass(command);
    stamp(RenderStats::PostProcess);
    timed_[slot] = timestamps_ != VK_NULL_HANDLE;
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
    rejectedDraws_ = frame.cullingInputDraws;
    rejectedTriangles_ = frame.cullingInputTriangles;
    occludedDraws_ = 0;
    if (rejectedDraws_)
        for (const auto &draw : drawCulling_.commands(frameIndex_,0)) {
            if (draw.vertexCount > 0) {
                --rejectedDraws_;
                rejectedTriangles_ -= draw.vertexCount/3;
            } else if (draw.instanceCount == 0)
                ++occludedDraws_;
        }
    // The GPU has finished this slot's last frame: read its pass timings (RenderStats::gpuMs).
    if (timestamps_ && timed_[frameIndex_]) {
        std::array<std::uint64_t,timestampsPerFrame> ticks{};
        if (api_.vkGetQueryPoolResults(device,timestamps_,uint32_t(frameIndex_)*timestampsPerFrame,timestampsPerFrame,
                sizeof(ticks),ticks.data(),sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (std::size_t pass = 0; pass < gpuMs_.size(); ++pass)
                gpuMs_[pass] = float(double(ticks[pass+1]-ticks[pass])*timestampPeriod_/1e6);
        timed_[frameIndex_] = false;
    }
    uint32_t image = 0;
    const auto acquired = api_.vkAcquireNextImageKHR(device,swapchain_,UINT64_MAX,frame.acquired,VK_NULL_HANDLE,&image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        requested_ = {};
        return true;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        return false;
    requestTextureDetail(vertices,ranges,view,width,height,false);
    api_.vkResetFences(device,1,&frame.fence);
    // TAA: render with this frame's jitter and reproject the history with the camera's motion and each surface's own
    // (the scene's velocity target).
    vk::HdrFrame::Temporal temporal;
    temporal.enabled = view.sky.y > 0.5f;
    const auto clip = vulkanClip(view.viewProjection);
    temporal.reprojection = reprojection(clip,previousValid_ ? previousClip_ : clip);
    reflectionClip_ = previousValid_ ? previousClip_ : clip;
    vertexMotion_ = true;
    motionWindTime_ = previousWindValid_ ? previousWindTime_ : view.lighting.z;
    previousWindTime_ = view.lighting.z;
    previousWindValid_ = true;
    reflectionReady_ = previousValid_ && frame_.previousFrameReady();
    previousClip_ = clip;
    previousValid_ = true;
    auto shown = withShadows(temporal.enabled ? jittered(view,frame_.jitter(),width,height) : view,view,vertices,
                             frame_.sampleIndex());
    updateSky(shown);
    // Auto exposure once per frame (it consumes the slot's histogram); the light clusters size lamp ranges by it.
    const float exposureStops = exposureFor(view,frameIndex_);
    clusterExposure_ = color::exposureScale(exposureStops);
    if (!prepare(frame,vertices,shown,width,height))
        return false;
    prepareMeshlets(frame,vertices,ranges,shown);
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
    api_.vkResetCommandBuffer(frame.command,0);
    api_.vkBeginCommandBuffer(frame.command,&begin);
    const auto meterSlot = view.display.z > 0.5f ? int(frameIndex_) : -1;
    record(frame,frame_,image,ranges,shown,sky,vertices,exposureStops,meterSlot,temporal,
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
                             UINT width,UINT height,bool sky,Image &result,float *meteredLuminance)
{
    api_.vkDeviceWaitIdle(device);
    requestTextureDetail(vertices,ranges,view,width,height,true);
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
    // Nothing moves in a capture: no motion from the previous frame's vertices or wind.
    vertexMotion_ = false;
    motionWindTime_ = view.lighting.z;
    float stops = std::log2(std::max(view.display.x,1e-6f));
    for (int pass = 0; ok && pass < passes; ++pass) {
        auto shown = withShadows(temporal.enabled ? jittered(view,target.jitter(),width,height) : view,view,vertices,
                                 target.sampleIndex());
        updateSky(shown);
        reflectionReady_ = target.previousFrameReady();
        clusterExposure_ = color::exposureScale(stops);
        if (!prepare(frame,vertices,shown,width,height)) {
            ok = false;
            break;
        }
        prepareMeshlets(frame,vertices,ranges,shown);
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
        api_.vkResetFences(device,1,&frame.fence);
        api_.vkResetCommandBuffer(frame.command,0);
        api_.vkBeginCommandBuffer(frame.command,&begin);
        record(frame,target,0,ranges,shown,sky,vertices,stops,automatic ? 0 : -1,temporal,occlusionFor(shown));
        const VkBufferImageCopy copy{0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{width,height,1}};
        api_.vkCmdCopyImageToBuffer(frame.command,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback_.buffer,1,&copy);
        api_.vkEndCommandBuffer(frame.command);
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&frame.command,0,nullptr};
        ok = api_.vkQueueSubmit(queue_,1,&submit,frame.fence) == VK_SUCCESS
            && api_.vkWaitForFences(device,1,&frame.fence,VK_TRUE,UINT64_MAX) == VK_SUCCESS;
        float average = 0;
        if (ok && automatic && target.takeMeter(0,average)) {
            stops = color::autoExposureStops(average,view.display.w);
            if (meteredLuminance)
                *meteredLuminance = average;
        }
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
        for (auto &buffer : vertexRing_)
            buffer.destroy(device_);
        meshletIndices_.destroy(device_);
        meshletData_.destroy(device_);
        ringCounts_ = {};
        ringCachedMeshVertices_ = {};
        drawCulling_.destroy();
        occlusionDepth_.destroy();
        fogVolume_.destroy();
        probeField_.clear();
        for (auto &frame : frames_) {
            frame.probes.reset();
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
        for (auto &texture : textures_) {
            texture.detail.destroy(device_);
            texture.tail.destroy(device_);
        }
        for (const auto pool : materialPools_)
            api_.vkDestroyDescriptorPool(device,pool,nullptr);
        if (framePool_)
            api_.vkDestroyDescriptorPool(device,framePool_,nullptr);
        if (timestamps_)
            api_.vkDestroyQueryPool(device,timestamps_,nullptr);
        timestamps_ = VK_NULL_HANDLE;
        timed_ = {};
        for (const auto handle : {sky_,scene_,shadowPipeline_,glassTransmission_,glassRadiance_,meshScene_})
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
        if (pointUpdatePass_)
            api_.vkDestroyRenderPass(device,pointUpdatePass_,nullptr);
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
