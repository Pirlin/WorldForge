#include "Engine/World/PlayableScene.h"
#include "Player/WorldShaders.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <psapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <string_view>

extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
}

namespace {
namespace wf = worldforge;
using Microsoft::WRL::ComPtr;
using namespace DirectX;

XMVECTOR vector(const wf::Point3 &point) { return XMVectorSet(point[0],point[1],point[2],0); }
wf::Point3 point(FXMVECTOR vector)
{
    XMFLOAT3 result;
    XMStoreFloat3(&result,vector);
    return {result.x,result.y,result.z};
}

struct View {
    XMFLOAT4X4 viewProjection, inverseProjection, lightProjection;
    XMFLOAT4 eye, horizon, zenith, fog, sun, lights[16];
    XMFLOAT4 fogCenterRadius, lighting;
};

// Consecutive vertices drawn with one base-color map. A negative texture uses plain white.
struct DrawRange {
    UINT start = 0, count = 0;
    int texture = -1;
};

XMFLOAT4 parameter(wf::Point3 p,float w) { return {p[0],p[1],p[2],w}; }

ComPtr<ID3DBlob> shader(const char *entry,const char *profile)
{
    ComPtr<ID3DBlob> bytecode, errors;
    if (FAILED(D3DCompile(worldShaders,sizeof(worldShaders)-1,"WorldForgePlayer",nullptr,nullptr,entry,profile,
                          D3DCOMPILE_ENABLE_STRICTNESS,0,&bytecode,&errors))) {
        if (errors)
            OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
    }
    return bytecode;
}

std::string utf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(std::size_t(std::max(size,0)),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
    std::wstring result(std::size_t(std::max(size,0)),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),result.data(),size);
    return result;
}

std::string escape(std::string_view text)
{
    std::string result;
    for (const char c : text) {
        if (c == '"' || c == '\\')
            result += '\\';
        if (static_cast<unsigned char>(c) >= 0x20)
            result += c;
    }
    return result;
}

class Renderer {
public:
    bool start(HWND window,bool forceSoftware)
    {
        DXGI_SWAP_CHAIN_DESC chain{};
        chain.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        chain.SampleDesc.Count = 1;
        chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        chain.BufferCount = 2;
        chain.OutputWindow = window;
        chain.Windowed = TRUE;
        chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        if (!forceSoftware && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT index = 0; factory->EnumAdapters1(index,adapter.ReleaseAndGetAddressOf()) == S_OK; ++index) {
                DXGI_ADAPTER_DESC1 info{};
                if (SUCCEEDED(adapter->GetDesc1(&info)) && info.VendorId == 0x10de
                    && std::wstring_view(info.Description).find(L"GTX 1650") != std::wstring_view::npos)
                    break;
                adapter.Reset();
            }
        }
        if (adapter) {
            if (FAILED(D3D11CreateDeviceAndSwapChain(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,
                        D3D11_SDK_VERSION,&chain,&swap_,&device_,nullptr,&context_)))
                return false;
        } else {
            for (const auto driver : {D3D_DRIVER_TYPE_HARDWARE,D3D_DRIVER_TYPE_WARP}) {
                if (forceSoftware && driver != D3D_DRIVER_TYPE_WARP)
                    continue;
                if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
                                                            &chain,&swap_,&device_,nullptr,&context_)))
                    break;
            }
        }
        if (!device_)
            return false;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> used;
        DXGI_ADAPTER_DESC usedInfo{};
        if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&used)) && SUCCEEDED(used->GetDesc(&usedInfo)))
            adapterName_ = usedInfo.Description;
        const auto sceneVS = shader("sceneVS","vs_5_0"), scenePS = shader("scenePS","ps_5_0");
        const auto skyVS = shader("skyVS","vs_5_0"), skyPS = shader("skyPS","ps_5_0");
        const auto shadowVS = shader("shadowVS","vs_5_0");
        if (!sceneVS || !scenePS || !skyVS || !skyPS || !shadowVS
            || FAILED(device_->CreateVertexShader(sceneVS->GetBufferPointer(),sceneVS->GetBufferSize(),nullptr,&vertex_))
            || FAILED(device_->CreatePixelShader(scenePS->GetBufferPointer(),scenePS->GetBufferSize(),nullptr,&pixel_))
            || FAILED(device_->CreateVertexShader(skyVS->GetBufferPointer(),skyVS->GetBufferSize(),nullptr,&skyVertex_))
            || FAILED(device_->CreatePixelShader(skyPS->GetBufferPointer(),skyPS->GetBufferSize(),nullptr,&skyPixel_))
            || FAILED(device_->CreateVertexShader(shadowVS->GetBufferPointer(),shadowVS->GetBufferSize(),nullptr,&shadowVertex_)))
            return false;
        static_assert(sizeof(wf::PlayVertex) == 48);
        const D3D11_INPUT_ELEMENT_DESC layout[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32_FLOAT,0,36,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,40,D3D11_INPUT_PER_VERTEX_DATA,0}
        };
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(View);
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_ANISOTROPIC;
        sampler.MaxAnisotropy = 8;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        const bool sceneReady = SUCCEEDED(device_->CreateInputLayout(layout,5,sceneVS->GetBufferPointer(),sceneVS->GetBufferSize(),&layout_))
            && SUCCEEDED(device_->CreateBuffer(&constants,nullptr,&constants_))
            && SUCCEEDED(device_->CreateRasterizerState(&raster,&raster_))
            && SUCCEEDED(device_->CreateDepthStencilState(&depth,&skyDepth_))
            && SUCCEEDED(device_->CreateSamplerState(&sampler,&baseColorSampler_));
        if (!sceneReady)
            return false;
        const std::uint32_t white = 0xffffffff;
        if (!createTexture(1,1,&white,white_))
            return false;
        raster.DepthBias = 1;
        raster.SlopeScaledDepthBias = 1.3f;
        return SUCCEEDED(device_->CreateRasterizerState(&raster,&shadowRaster_)) && createShadow();
    }

    // Decodes a PNG or JPEG base-color map. Returns its index, or -1 when it cannot be used.
    int addTexture(const std::vector<std::uint8_t> &bytes)
    {
        if (bytes.empty() || bytes.size() > wf::MeshMaterial::maxTextureBytes)
            return -1;
        if (!imaging_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(&imaging_))))
            return -1;
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(bytes.data(),UINT(bytes.size())));
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT width = 0, height = 0;
        if (!stream || FAILED(imaging_->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder))
            || FAILED(decoder->GetFrame(0,&frame)) || FAILED(frame->GetSize(&width,&height))
            || !width || !height || width > 8192 || height > 8192
            || FAILED(imaging_->CreateFormatConverter(&converter))
            || FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,
                                            WICBitmapPaletteTypeCustom)))
            return -1;
        std::vector<std::uint32_t> pixels(std::size_t(width)*height);
        if (FAILED(converter->CopyPixels(nullptr,width*4,UINT(pixels.size()*4),reinterpret_cast<BYTE *>(pixels.data()))))
            return -1;
        ComPtr<ID3D11ShaderResourceView> view;
        if (!createTexture(width,height,pixels.data(),view))
            return -1;
        textures_.push_back(view);
        return int(textures_.size()-1);
    }

    bool draw(const std::vector<wf::PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
              UINT width,UINT height,bool sky,bool verify,UINT sync)
    {
        if (!resize(width,height))
            return false;
        context_->UpdateSubresource(constants_.Get(),0,nullptr,&view,0,0);
        context_->VSSetConstantBuffers(0,1,constants_.GetAddressOf());
        context_->PSSetConstantBuffers(0,1,constants_.GetAddressOf());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (!vertices.empty()) {
            const auto bytes = vertices.size()*sizeof(wf::PlayVertex);
            if (bytes > 240000000)
                return false;
            if (bytes > capacity_) {
                D3D11_BUFFER_DESC buffer{};
                buffer.ByteWidth = UINT(bytes+bytes/4);
                buffer.Usage = D3D11_USAGE_DYNAMIC;
                buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(device_->CreateBuffer(&buffer,nullptr,vertices_.ReleaseAndGetAddressOf())))
                    return false;
                capacity_ = buffer.ByteWidth;
            }
            D3D11_MAPPED_SUBRESOURCE upload{};
            if (FAILED(context_->Map(vertices_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&upload)))
                return false;
            std::memcpy(upload.pData,vertices.data(),bytes);
            context_->Unmap(vertices_.Get(),0);
            const UINT stride = sizeof(wf::PlayVertex), offset = 0;
            context_->IASetVertexBuffers(0,1,vertices_.GetAddressOf(),&stride,&offset);
        }
        ID3D11ShaderResourceView *empty = nullptr;
        context_->PSSetShaderResources(0,1,&empty);
        context_->OMSetRenderTargets(0,nullptr,shadowDepth_.Get());
        context_->ClearDepthStencilView(shadowDepth_.Get(),D3D11_CLEAR_DEPTH,1,0);
        if (view.lighting.y > 0 && !vertices.empty()) {
            const D3D11_VIEWPORT shadowViewport{0,0,float(shadowSize),float(shadowSize),0,1};
            context_->RSSetViewports(1,&shadowViewport);
            context_->RSSetState(shadowRaster_.Get());
            context_->OMSetDepthStencilState(nullptr,0);
            context_->IASetInputLayout(layout_.Get());
            context_->VSSetShader(shadowVertex_.Get(),nullptr,0);
            context_->PSSetShader(nullptr,nullptr,0);
            context_->Draw(UINT(vertices.size()),0);
        }
        const float clear[]{0.05f,0.06f,0.07f,1};
        context_->OMSetRenderTargets(1,target_.GetAddressOf(),depth_.Get());
        context_->ClearRenderTargetView(target_.Get(),clear);
        context_->ClearDepthStencilView(depth_.Get(),D3D11_CLEAR_DEPTH,1,0);
        context_->PSSetShaderResources(0,1,shadowView_.GetAddressOf());
        context_->PSSetSamplers(0,1,shadowSampler_.GetAddressOf());
        context_->PSSetSamplers(1,1,baseColorSampler_.GetAddressOf());
        const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        context_->RSSetViewports(1,&viewport);
        context_->RSSetState(raster_.Get());
        if (sky) {
            context_->OMSetDepthStencilState(skyDepth_.Get(),0);
            context_->IASetInputLayout(nullptr);
            context_->VSSetShader(skyVertex_.Get(),nullptr,0);
            context_->PSSetShader(skyPixel_.Get(),nullptr,0);
            context_->Draw(3,0);
        }
        if (!vertices.empty()) {
            context_->IASetInputLayout(layout_.Get());
            context_->OMSetDepthStencilState(nullptr,0);
            context_->VSSetShader(vertex_.Get(),nullptr,0);
            context_->PSSetShader(pixel_.Get(),nullptr,0);
            for (const auto &range : ranges) {
                const bool textured = range.texture >= 0 && std::size_t(range.texture) < textures_.size();
                context_->PSSetShaderResources(1,1,textured ? textures_[range.texture].GetAddressOf() : white_.GetAddressOf());
                context_->Draw(range.count,range.start);
            }
        }
        return (!verify || verifyFrame()) && SUCCEEDED(swap_->Present(sync,0));
    }

    const std::wstring &adapterName() const { return adapterName_; }

private:
    static constexpr UINT shadowSize = 1536;

    bool createTexture(UINT width,UINT height,const std::uint32_t *pixels,ComPtr<ID3D11ShaderResourceView> &view)
    {
        D3D11_TEXTURE2D_DESC image{};
        image.Width = width;
        image.Height = height;
        image.MipLevels = width > 1 || height > 1 ? 0 : 1;
        image.ArraySize = image.SampleDesc.Count = 1;
        image.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        image.BindFlags = D3D11_BIND_SHADER_RESOURCE | (image.MipLevels ? 0 : D3D11_BIND_RENDER_TARGET);
        image.MiscFlags = image.MipLevels ? 0 : D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device_->CreateTexture2D(&image,nullptr,&texture))
            || FAILED(device_->CreateShaderResourceView(texture.Get(),nullptr,&view)))
            return false;
        context_->UpdateSubresource(texture.Get(),0,nullptr,pixels,width*4,0);
        if (!image.MipLevels)
            context_->GenerateMips(view.Get());
        return true;
    }

    bool createShadow()
    {
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC image{};
        image.Width = image.Height = shadowSize;
        image.MipLevels = image.ArraySize = image.SampleDesc.Count = 1;
        image.Format = DXGI_FORMAT_R32_TYPELESS;
        image.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC depth{};
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        return SUCCEEDED(device_->CreateTexture2D(&image,nullptr,&texture))
            && SUCCEEDED(device_->CreateDepthStencilView(texture.Get(),&depth,&shadowDepth_))
            && SUCCEEDED(device_->CreateShaderResourceView(texture.Get(),&view,&shadowView_))
            && SUCCEEDED(device_->CreateSamplerState(&sampler,&shadowSampler_));
    }

    bool verifyFrame()
    {
        ComPtr<ID3D11Texture2D> back, readback;
        D3D11_TEXTURE2D_DESC info{};
        if (FAILED(swap_->GetBuffer(0,IID_PPV_ARGS(&back))))
            return false;
        back->GetDesc(&info);
        info.Width = info.Height = 1;
        info.BindFlags = info.MiscFlags = 0;
        info.Usage = D3D11_USAGE_STAGING;
        info.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&info,nullptr,&readback)))
            return false;
        const D3D11_BOX sample{0,0,0,1,1,1};
        context_->CopySubresourceRegion(readback.Get(),0,0,0,0,back.Get(),0,&sample);
        D3D11_MAPPED_SUBRESOURCE data{};
        if (FAILED(context_->Map(readback.Get(),0,D3D11_MAP_READ,0,&data)))
            return false;
        const auto *rgba = static_cast<const unsigned char *>(data.pData);
        const bool rendered = rgba[0]+rgba[1]+rgba[2] > 120;
        context_->Unmap(readback.Get(),0);
        return rendered;
    }

    bool resize(UINT width,UINT height)
    {
        if (target_ && width == width_ && height == height_)
            return true;
        context_->OMSetRenderTargets(0,nullptr,nullptr);
        target_.Reset();
        depth_.Reset();
        if (FAILED(swap_->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0)))
            return false;
        ComPtr<ID3D11Texture2D> back, depthTexture;
        D3D11_TEXTURE2D_DESC depth{};
        depth.Width = width;
        depth.Height = height;
        depth.MipLevels = depth.ArraySize = depth.SampleDesc.Count = 1;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        width_ = width;
        height_ = height;
        return SUCCEEDED(swap_->GetBuffer(0,IID_PPV_ARGS(&back)))
            && SUCCEEDED(device_->CreateRenderTargetView(back.Get(),nullptr,&target_))
            && SUCCEEDED(device_->CreateTexture2D(&depth,nullptr,&depthTexture))
            && SUCCEEDED(device_->CreateDepthStencilView(depthTexture.Get(),nullptr,&depth_));
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11DepthStencilView> depth_, shadowDepth_;
    ComPtr<ID3D11ShaderResourceView> shadowView_, white_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures_;
    ComPtr<ID3D11SamplerState> shadowSampler_, baseColorSampler_;
    ComPtr<ID3D11VertexShader> vertex_, skyVertex_, shadowVertex_;
    ComPtr<ID3D11PixelShader> pixel_, skyPixel_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11RasterizerState> raster_, shadowRaster_;
    ComPtr<ID3D11DepthStencilState> skyDepth_;
    ComPtr<ID3D11Buffer> constants_, vertices_;
    ComPtr<IWICImagingFactory> imaging_;
    std::wstring adapterName_;
    UINT capacity_ = 0, width_ = 0, height_ = 0;
};

class Scene {
public:
    explicit Scene(wf::PlayableScene playable) : playable_(std::move(playable)),
        spawn_(player().position), chunks_(playable_.simulation.terrains.size())
    {
        auto avatar = wf::makeCube();
        avatar.materials[0].color = {0.91f,0.54f,0.19f,1};
        wf::PlayMesh mesh{int(playable_.player),wf::makePlayVertices(avatar)};
        for (auto &v : mesh.vertices)
            for (int axis = 0; axis < 3; ++axis)
                v.position[axis] *= player().halfSize[axis]*2;
        playable_.meshes.push_back(std::move(mesh));
        playable_.gameplay.start(playable_.simulation);
    }

    // Returns how many base-color maps could not be decoded.
    int uploadTextures(Renderer &renderer)
    {
        int failed = 0;
        meshTextures_.clear();
        for (const auto &mesh : playable_.meshes) {
            const int texture = renderer.addTexture(mesh.baseColorTexture);
            failed += !mesh.baseColorTexture.empty() && texture < 0;
            meshTextures_.push_back(texture);
        }
        textured_ = int(std::ranges::count_if(meshTextures_,[](int texture) { return texture >= 0; }));
        return failed;
    }

    void tick(float yaw,int forward,int sideways,bool jump,bool interact)
    {
        playable_.gameplay.update(playable_.simulation,playable_.player,interact);
        wf::drivePlayer(player(),spawn_,yaw,forward,sideways,jump,jumpBuffer_);
        playable_.simulation.step();
    }

    wf::BoxBody &player() { return playable_.simulation.bodies[playable_.player]; }
    const wf::PlayableScene &playable() const { return playable_; }
    bool sky() const { return playable_.sky; }
    int texturedMeshes() const { return textured_; }

    View view(float yaw,float pitch,float aspect,float time)
    {
        View view{};
        const auto target = XMVectorAdd(vector(player().position),XMVectorSet(0,0,0.35f,0));
        const auto offset = XMVectorSet(std::cos(yaw)*std::cos(pitch),std::sin(yaw)*std::cos(pitch),std::sin(pitch),0);
        const auto eye = XMVectorAdd(target,XMVectorScale(offset,7.5f));
        const auto projection = XMMatrixLookAtRH(eye,target,XMVectorSet(0,0,1,0))
            * XMMatrixPerspectiveFovRH(XM_PIDIV4,aspect,0.05f,1800);
        XMStoreFloat4x4(&view.viewProjection,projection);
        XMStoreFloat4x4(&view.inverseProjection,XMMatrixInverse(nullptr,projection));
        view.eye = parameter(point(eye),0);
        view.horizon = parameter(playable_.horizon,playable_.fogFalloff);
        view.zenith = parameter(playable_.zenith,playable_.fogHeight);
        view.fog = parameter(playable_.fog,playable_.fogDensity);
        view.sun = parameter(point(XMVector3Normalize(XMVectorSet(-0.45f,-0.7f,1,0))),1);
        bool sunSet = false;
        for (const auto &light : playable_.lights) {
            if (light.directional && !sunSet) {
                view.sun = parameter(light.vector,light.intensity);
                sunSet = true;
                continue;
            }
            if (view.eye.w < 16 && light.intensity > 0) {
                view.lights[int(view.eye.w)] = parameter(light.vector,light.directional ? -light.intensity : light.intensity);
                ++view.eye.w;
            }
        }
        const auto sun = XMVector3Normalize(XMVectorSet(view.sun.x,view.sun.y,view.sun.z,0));
        constexpr float shadowRange = 48;
        const auto lightEye = XMVectorAdd(target,XMVectorScale(sun,shadowRange*2));
        const auto lightUp = std::abs(view.sun.z) > 0.98f ? XMVectorSet(0,1,0,0) : XMVectorSet(0,0,1,0);
        XMStoreFloat4x4(&view.lightProjection,XMMatrixLookAtRH(lightEye,target,lightUp)
            * XMMatrixOrthographicRH(shadowRange*2,shadowRange*2,0.05f,shadowRange*4));
        view.fogCenterRadius = parameter(playable_.localFogCenter,playable_.localFogRadius);
        view.lighting = {playable_.localFogDensity,playable_.shadows && view.sun.w > 0 ? 1.0f : 0.0f,time,0.00045f};
        return view;
    }

    const std::vector<wf::PlayVertex> &vertices()
    {
        vertices_.clear();
        ranges_.clear();
        const auto mark = [this](std::size_t start,int texture) {
            const auto count = vertices_.size()-start;
            if (!count)
                return;
            if (!ranges_.empty() && ranges_.back().texture == texture
                && ranges_.back().start+ranges_.back().count == UINT(start))
                ranges_.back().count += UINT(count);
            else
                ranges_.push_back({UINT(start),UINT(count),texture});
        };
        const auto &world = playable_.simulation;
        for (std::size_t index = 0; index < playable_.meshes.size(); ++index) {
            const auto &mesh = playable_.meshes[index];
            const auto start = vertices_.size();
            const int texture = index < meshTextures_.size() ? meshTextures_[index] : -1;
            auto origin = vector(mesh.lodOrigin);
            if (!mesh.lodVertices.empty() && mesh.body >= 0) {
                const auto &body = world.bodies[mesh.body];
                const auto &q = body.rotation;
                origin = XMVectorAdd(XMVector3Rotate(origin,XMVectorSet(q[0],q[1],q[2],q[3])),vector(body.position));
            }
            const bool distant = !mesh.lodVertices.empty() && XMVectorGetX(XMVector3LengthSq(
                XMVectorSubtract(origin,vector(player().position)))) >= mesh.lodDistance*mesh.lodDistance;
            const auto &vertices = distant ? mesh.lodVertices : mesh.vertices;
            if (mesh.body < 0) {
                vertices_.insert(vertices_.end(),vertices.begin(),vertices.end());
                mark(start,texture);
                continue;
            }
            const auto &body = world.bodies[mesh.body];
            const auto &q = body.rotation;
            const auto rotation = XMVectorSet(q[0],q[1],q[2],q[3]);
            for (auto v : vertices) {
                v.position = point(XMVectorAdd(XMVector3Rotate(vector(v.position),rotation),vector(body.position)));
                v.normal = point(XMVector3Rotate(vector(v.normal),rotation));
                vertices_.push_back(v);
            }
            mark(start,texture);
        }
        const auto terrainStart = vertices_.size();
        for (std::size_t i = 0; i < world.terrains.size(); ++i) {
            const auto &terrain = world.terrains[i];
            const auto &position = player().position;
            constexpr int side = wf::Heightfield::chunks;
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x) {
                    auto &chunk = chunks_[i][y*side+x];
                    if (!terrain.field.chunkInRange(x,y,position[0]-terrain.position[0],position[1]-terrain.position[1],120)) {
                        if (!chunk.empty())
                            std::vector<wf::PlayVertex>().swap(chunk);
                        continue;
                    }
                    if (chunk.empty()) {
                        chunk = wf::makePlayVertices(terrain.field.chunk(x,y),&terrain.field);
                        const auto foliage = playable_.foliage[i];
                        const auto trees = wf::makePlayVertices(terrain.field.foliage(x,y,foliage.density,foliage.seed),nullptr,foliage.wind);
                        chunk.insert(chunk.end(),trees.begin(),trees.end());
                        for (auto &v : chunk)
                            for (int axis = 0; axis < 3; ++axis)
                                v.position[axis] += terrain.position[axis];
                    }
                    vertices_.insert(vertices_.end(),chunk.begin(),chunk.end());
                }
        }
        mark(terrainStart,-1);
        return vertices_;
    }
    const std::vector<DrawRange> &ranges() const { return ranges_; }

private:
    wf::PlayableScene playable_;
    wf::Point3 spawn_;
    std::vector<std::array<std::vector<wf::PlayVertex>,wf::Heightfield::chunks*wf::Heightfield::chunks>> chunks_;
    std::vector<wf::PlayVertex> vertices_;
    std::vector<DrawRange> ranges_;
    std::vector<int> meshTextures_;
    int textured_ = 0;
    int jumpBuffer_ = 0;
};

LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

std::filesystem::path chooseScene()
{
    wchar_t file[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.lpstrFile = file;
    picker.nMaxFile = DWORD(std::size(file));
    picker.lpstrFilter = L"WorldForge Playable Scene (*.wfplay)\0*.wfplay\0";
    picker.lpstrTitle = L"Open a WorldForge playable scene";
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameW(&picker) ? std::filesystem::path(file) : std::filesystem::path();
}

// Frame time samples and results of an automated street run.
struct StreetRun {
    std::vector<double> frameMs, cpuMs;
    float seconds = 0, missionSeconds = 0;
    bool mission = false, traffic = false, enteredShop = false;
    std::vector<std::string> objectives;

    static double percentile(std::vector<double> values,double fraction)
    {
        if (values.empty())
            return 0;
        std::ranges::sort(values);
        return values[std::min(values.size()-1,std::size_t(fraction*double(values.size()-1)+0.5))];
    }
    bool write(const std::filesystem::path &file,const Renderer &renderer,const Scene &scene) const
    {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory));
        const auto average = [](const std::vector<double> &values) {
            return values.empty() ? 0.0 : std::accumulate(values.begin(),values.end(),0.0)/double(values.size());
        };
        std::ofstream out(file);
        out << "{\n  \"adapter\": \"" << escape(utf8(renderer.adapterName())) << "\",\n"
            << "  \"missionComplete\": " << (mission ? "true" : "false") << ",\n"
            << "  \"enteredShop\": " << (enteredShop ? "true" : "false") << ",\n"
            << "  \"trafficLooped\": " << (traffic ? "true" : "false") << ",\n"
            << "  \"texturedMeshes\": " << scene.texturedMeshes() << ",\n"
            << "  \"simulatedSeconds\": " << seconds << ",\n"
            << "  \"missionSeconds\": " << missionSeconds << ",\n"
            << "  \"frames\": " << frameMs.size() << ",\n"
            << "  \"frameMs\": {\"average\": " << average(frameMs) << ", \"p95\": " << percentile(frameMs,0.95)
            << ", \"p99\": " << percentile(frameMs,0.99) << ", \"max\": " << percentile(frameMs,1) << "},\n"
            << "  \"cpuMs\": {\"average\": " << average(cpuMs) << ", \"p95\": " << percentile(cpuMs,0.95)
            << ", \"max\": " << percentile(cpuMs,1) << "},\n"
            << "  \"workingSetMiB\": " << double(memory.WorkingSetSize)/1048576
            << ",\n  \"peakWorkingSetMiB\": " << double(memory.PeakWorkingSetSize)/1048576
            << ",\n  \"privateMiB\": " << double(memory.PrivateUsage)/1048576 << ",\n  \"objectives\": [";
        for (std::size_t i = 0; i < objectives.size(); ++i)
            out << (i ? ", " : "") << '"' << escape(objectives[i]) << '"';
        out << "]\n}\n";
        return bool(out);
    }
};
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show)
{
    int argc = 0;
    auto **args = CommandLineToArgvW(GetCommandLineW(),&argc);
    const bool software = argc == 3 && std::wstring_view(args[1]) == L"--smoke-test-warp";
    const bool smoke = software || (argc == 3 && std::wstring_view(args[1]) == L"--smoke-test");
    const bool street = argc == 4 && std::wstring_view(args[1]) == L"--verify-street";
    const bool scripted = smoke || street;
    const auto file = argc >= 2 ? std::filesystem::path(args[scripted ? 2 : 1]) : chooseScene();
    const auto reportFile = street ? std::filesystem::path(args[3]) : std::filesystem::path();
    LocalFree(args);
    if (file.empty())
        return 0;
    const auto fail = [scripted](const wchar_t *message) {
        if (!scripted)
            MessageBoxW(nullptr,message,L"WorldForge Player",MB_ICONERROR);
        return 1;
    };
    if (FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))
        return fail(L"Could not start Windows imaging.");
    std::ifstream input(file);
    auto playable = wf::readPlayable(input);
    if (!playable)
        return fail(L"The playable scene could not be read. Export it from WorldForge Editor.");
    Scene scene(std::move(*playable));
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    windowClass.lpszClassName = L"WorldForgePlayer";
    if (!RegisterClassW(&windowClass))
        return fail(L"Could not register the Player window.");
    const std::wstring controls = L"WASD Move   Space Jump   E Interact   Right Mouse Orbit   Esc Exit";
    const auto window = CreateWindowExW(0,windowClass.lpszClassName,(L"WorldForge Player   |   " + controls).c_str(),
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,smoke ? 400 : 1280,smoke ? 280 : 720,
        nullptr,nullptr,instance,nullptr);
    Renderer renderer;
    if (!window || !renderer.start(window,software))
        return fail(L"Could not start the Direct3D 11 renderer.");
    if (scene.uploadTextures(renderer) > 0)
        return fail(L"A base-color map in this scene could not be decoded.");
    ShowWindow(window,scripted ? SW_SHOWNOACTIVATE : show);
    using Clock = std::chrono::steady_clock;
    const auto launched = Clock::now();
    auto previous = launched;
    double accumulator = 0;
    float yaw = -0.85f, pitch = 0.37f;
    bool jumpPressed = false, jumpPending = false, orbiting = false;
    bool interactPressed = false, interactPending = false;
    bool testJumped = false;
    const auto initialPosition = scene.player().position;
    POINT lastMouse{};
    int frames = 0;
    std::string objective;
    StreetRun run;
    wf::Autopilot pilot;
    float pilotYaw = 0;
    for (;;) {
        const auto frameStart = Clock::now();
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if (message.message == WM_QUIT)
                return int(message.wParam);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        RECT client{};
        GetClientRect(window,&client);
        if (!client.right || !client.bottom || IsIconic(window)) {
            Sleep(16);
            previous = Clock::now();
            continue;
        }
        const bool focused = GetForegroundWindow() == window;
        const auto down = [focused](int key) { return focused && (GetAsyncKeyState(key)&0x8000) != 0; };
        POINT mouse{};
        GetCursorPos(&mouse);
        const bool orbit = down(VK_RBUTTON);
        if (orbit && orbiting) {
            yaw -= float(mouse.x-lastMouse.x)*0.008f;
            pitch = std::clamp(pitch+float(mouse.y-lastMouse.y)*0.008f,-0.15f,1.4f);
        }
        orbiting = orbit;
        lastMouse = mouse;
        const bool jump = down(VK_SPACE);
        jumpPending |= jump && !jumpPressed;
        jumpPressed = jump;
        const bool interactKey = down('E');
        interactPending |= interactKey && !interactPressed;
        interactPressed = interactKey;
        if (!focused)
            jumpPending = interactPending = false;
        const auto now = Clock::now();
        accumulator += scripted ? 1.0/60 : std::min(0.10,std::chrono::duration<double>(now-previous).count());
        previous = now;
        while (accumulator >= wf::WorldSimulation::stepSeconds) {
            if (street) {
                bool interact = false;
                const bool walk = pilot.drive(scene.playable().simulation,scene.playable().player,
                                              scene.playable().gameplay,pilotYaw,interact);
                scene.tick(pilotYaw,int(walk),0,false,interact);
                yaw = pilotYaw;
                run.seconds += wf::WorldSimulation::stepSeconds;
            } else {
                const bool testJump = smoke && !testJumped && scene.player().grounded;
                scene.tick(yaw,smoke ? int(frames >= 25) : int(down('W'))-int(down('S')),
                    int(down('D'))-int(down('A')),jumpPending || testJump,interactPending);
                testJumped |= testJump && scene.player().velocity[2] > 1;
            }
            jumpPending = interactPending = false;
            accumulator -= wf::WorldSimulation::stepSeconds;
        }
        const auto &gameplay = scene.playable().gameplay;
        if (const auto next = gameplay.objective(); next != objective) {
            objective = next;
            run.objectives.push_back(objective);
            SetWindowTextW(window,(L"WorldForge Player   |   " + (objective.empty() ? controls : wide(objective)
                + L"   |   " + controls)).c_str());
        }
        const auto view = scene.view(yaw,pitch,float(client.right)/client.bottom,std::chrono::duration<float>(now-launched).count());
        const auto &vertices = scene.vertices();
        const auto beforePresent = Clock::now();
        if (!renderer.draw(vertices,scene.ranges(),view,UINT(client.right),UINT(client.bottom),scene.sky(),
                           smoke && scene.sky() && frames == 0,street ? 0 : 1))
            return fail(L"The Player could not render the current scene.");
        if (smoke && ++frames == 90) {
            const auto &position = scene.player().position;
            return testJumped && std::hypot(position[0]-initialPosition[0],position[1]-initialPosition[1]) > 1 ? 0 : 2;
        }
        if (street) {
            const auto end = Clock::now();
            if (++frames > 30) {
                run.frameMs.push_back(std::chrono::duration<double,std::milli>(end-frameStart).count());
                run.cpuMs.push_back(std::chrono::duration<double,std::milli>(beforePresent-frameStart).count());
            }
            const auto &position = scene.player().position;
            for (const auto &mission : gameplay.missions)
                run.enteredShop |= mission.state != wf::Mission::State::Waiting
                    && std::hypot(position[0]-mission.target[0],position[1]-mission.target[1]) <= mission.targetRadius;
            if (!run.mission && gameplay.missionsComplete()) {
                run.mission = true;
                run.missionSeconds = run.seconds;
            }
            run.traffic = gameplay.trafficLooped();
            if ((run.mission && run.traffic) || run.seconds > 180) {
                const bool written = run.write(reportFile,renderer,scene);
                return written && run.mission && run.traffic && run.enteredShop ? 0 : 3;
            }
        }
    }
}
