#include "Engine/World/PlayableScene.h"
#include "Player/PlayerRender.h"
#include "Player/WorldShaders.h"
#include "Player/ReferenceScene.h"
#include "Engine/Render/CameraProjection.h"
#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/SkyLight.h"
#ifdef WORLDFORGE_PLAYER_VULKAN
#include "Player/VulkanRenderer.h"
#endif

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
#include <limits>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
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

using wf::player::View;
using wf::player::MaterialDraw;
using wf::player::DrawRange;
using wf::player::Image;
using wf::player::Renderer;
using wf::player::RenderStats;
using wf::player::ReferenceCamera;
using wf::player::writeReference;

struct MaterialConstants {
    XMFLOAT4 emissive, factors;
};

// Decodes a PNG or JPEG to RGBA8. Returns an empty image when it cannot be used.
Image decodeImage(const std::vector<std::uint8_t> &bytes)
{
    static ComPtr<IWICImagingFactory> imaging;
    Image image;
    if (bytes.empty() || bytes.size() > wf::MeshMaterial::maxTextureBytes)
        return image;
    if (!imaging && FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging))))
        return image;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(),UINT(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0, height = 0;
    if (!stream || FAILED(imaging->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder))
        || FAILED(decoder->GetFrame(0,&frame)) || FAILED(frame->GetSize(&width,&height))
        || !width || !height || width > 8192 || height > 8192
        || FAILED(imaging->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,
                                        WICBitmapPaletteTypeCustom)))
        return image;
    std::vector<std::uint32_t> pixels(std::size_t(width)*height);
    if (FAILED(converter->CopyPixels(nullptr,width*4,UINT(pixels.size()*4),reinterpret_cast<BYTE *>(pixels.data()))))
        return image;
    return {width,height,std::move(pixels)};
}

bool savePng(const std::filesystem::path &file,const Image &image)
{
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (image.pixels.size() != std::size_t(image.width)*image.height || image.pixels.empty()
        || FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))
        || FAILED(imaging->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE))
        || FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))
        || FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))
        || FAILED(encoder->CreateNewFrame(&frame,nullptr)) || FAILED(frame->Initialize(nullptr))
        || FAILED(frame->SetSize(image.width,image.height)) || FAILED(frame->SetPixelFormat(&format)))
        return false;
    // WIC writes RGBA as 32bppRGBA when the encoder accepts it; otherwise convert to BGRA.
    std::vector<std::uint32_t> pixels = image.pixels;
    if (format != GUID_WICPixelFormat32bppRGBA) {
        if (format != GUID_WICPixelFormat32bppBGRA)
            return false;
        for (auto &pixel : pixels)
            pixel = (pixel & 0xff00ff00u) | ((pixel & 0xffu) << 16) | ((pixel >> 16) & 0xffu);
    }
    return SUCCEEDED(frame->WritePixels(image.height,image.width*4,UINT(pixels.size()*4),reinterpret_cast<BYTE *>(pixels.data())))
        && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

XMFLOAT4 parameter(wf::Point3 p,float w) { return {p[0],p[1],p[2],w}; }

// The shared AgX tables come first, so the HLSL and the editor GLSL use the same data.
const std::string &shaderSource()
{
    static const std::string source = "static const float agxCurve[129] = {\n"
        + wf::color::shaderFloatList(wf::color::agxCurve) + "};\nstatic const float agxMatrices[18] = {\n"
        + wf::color::shaderFloatList(wf::color::agxMatrices) + "};\n" + worldShaders;
    return source;
}

ComPtr<ID3DBlob> shader(const char *entry,const char *profile)
{
    ComPtr<ID3DBlob> bytecode, errors;
    const auto &source = shaderSource();
    if (FAILED(D3DCompile(source.data(),source.size(),"WorldForgePlayer",nullptr,nullptr,entry,profile,
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

// Direct3D 11 fallback for computers without a Vulkan driver, and for the WARP software smoke test.
class D3D11Renderer final : public Renderer {
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
        const auto displayVS = shader("displayVS","vs_5_0"), displayPS = shader("displayPS","ps_5_0");
        if (!sceneVS || !scenePS || !skyVS || !skyPS || !shadowVS || !displayVS || !displayPS
            || FAILED(device_->CreateVertexShader(displayVS->GetBufferPointer(),displayVS->GetBufferSize(),nullptr,&displayVertex_))
            || FAILED(device_->CreatePixelShader(displayPS->GetBufferPointer(),displayPS->GetBufferSize(),nullptr,&displayPixel_))
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
        constants.ByteWidth = sizeof(MaterialConstants);
        if (FAILED(device_->CreateBuffer(&constants,nullptr,&materialConstants_)))
            return false;
        const std::uint32_t white = 0xffffffff;
        if (!createTexture(1,1,&white,white_))
            return false;
        raster.DepthBias = 1;
        raster.SlopeScaledDepthBias = 1.3f;
        return SUCCEEDED(device_->CreateRasterizerState(&raster,&shadowRaster_)) && createShadow();
    }

    int addTexture(const Image &image) override
    {
        ComPtr<ID3D11ShaderResourceView> view;
        if (image.pixels.empty() || !createTexture(image.width,image.height,image.pixels.data(),view))
            return -1;
        textures_.push_back(view);
        return int(textures_.size()-1);
    }

    bool draw(const std::vector<wf::PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
              UINT width,UINT height,bool sky,bool verify,UINT sync) override
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
        // The scene renders linear light into a 16-bit float target; the display pass maps it to the window.
        const auto background = wf::color::srgbToLinear({0.05f,0.06f,0.07f});
        const float clear[]{background[0],background[1],background[2],1};
        ID3D11ShaderResourceView *noScene = nullptr;
        context_->PSSetShaderResources(6,1,&noScene);
        context_->OMSetRenderTargets(1,sceneTarget_.GetAddressOf(),depth_.Get());
        context_->ClearRenderTargetView(sceneTarget_.Get(),clear);
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
            context_->PSSetConstantBuffers(1,1,materialConstants_.GetAddressOf());
            for (const auto &range : ranges) {
                std::array<ID3D11ShaderResourceView *,5> images;
                for (std::size_t i = 0; i < images.size(); ++i) {
                    const int texture = range.material.textures[i];
                    images[i] = texture >= 0 ? textures_[texture].Get() : white_.Get();
                }
                const auto &material = range.material;
                const MaterialConstants values{parameter(material.emissive,0),
                    {material.metallic,material.roughness,float(material.advanced),float(material.textures[1] >= 0)}};
                context_->UpdateSubresource(materialConstants_.Get(),0,nullptr,&values,0,0);
                context_->PSSetShaderResources(1,UINT(images.size()),images.data());
                context_->Draw(range.count,range.start);
            }
        }
        context_->OMSetRenderTargets(1,target_.GetAddressOf(),nullptr);
        context_->OMSetDepthStencilState(skyDepth_.Get(),0);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(displayVertex_.Get(),nullptr,0);
        context_->PSSetShader(displayPixel_.Get(),nullptr,0);
        context_->PSSetShaderResources(6,1,sceneLight_.GetAddressOf());
        context_->Draw(3,0);
        context_->PSSetShaderResources(6,1,&noScene);
        return (!verify || verifyFrame()) && SUCCEEDED(swap_->Present(sync,0));
    }

    const std::wstring &adapterName() const override { return adapterName_; }
    const char *api() const override { return "Direct3D 11"; }

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
        sceneTarget_.Reset();
        sceneLight_.Reset();
        if (FAILED(swap_->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0)))
            return false;
        ComPtr<ID3D11Texture2D> back, depthTexture;
        D3D11_TEXTURE2D_DESC depth{};
        depth.Width = width;
        depth.Height = height;
        depth.MipLevels = depth.ArraySize = depth.SampleDesc.Count = 1;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_TEXTURE2D_DESC scene = depth;
        scene.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        scene.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> sceneTexture;
        width_ = width;
        height_ = height;
        return SUCCEEDED(swap_->GetBuffer(0,IID_PPV_ARGS(&back)))
            && SUCCEEDED(device_->CreateRenderTargetView(back.Get(),nullptr,&target_))
            && SUCCEEDED(device_->CreateTexture2D(&depth,nullptr,&depthTexture))
            && SUCCEEDED(device_->CreateDepthStencilView(depthTexture.Get(),nullptr,&depth_))
            && SUCCEEDED(device_->CreateTexture2D(&scene,nullptr,&sceneTexture))
            && SUCCEEDED(device_->CreateRenderTargetView(sceneTexture.Get(),nullptr,&sceneTarget_))
            && SUCCEEDED(device_->CreateShaderResourceView(sceneTexture.Get(),nullptr,&sceneLight_));
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11DepthStencilView> depth_, shadowDepth_;
    ComPtr<ID3D11ShaderResourceView> shadowView_, white_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures_;
    ComPtr<ID3D11SamplerState> shadowSampler_, baseColorSampler_;
    ComPtr<ID3D11VertexShader> vertex_, skyVertex_, shadowVertex_, displayVertex_;
    ComPtr<ID3D11PixelShader> pixel_, skyPixel_, displayPixel_;
    ComPtr<ID3D11RenderTargetView> sceneTarget_;
    ComPtr<ID3D11ShaderResourceView> sceneLight_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11RasterizerState> raster_, shadowRaster_;
    ComPtr<ID3D11DepthStencilState> skyDepth_;
    ComPtr<ID3D11Buffer> constants_, materialConstants_, vertices_;
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
    ~Scene() { native_.stop(playable_.simulation,playable_.nativeBodies,playable_.player,logNative); }
    bool stageNative(const std::filesystem::path &path,std::string &error)
    {
        return native_.stage(path,error,playable_.nativeInstances);
    }
    std::uint64_t nativeSteps() const { return native_.steps(); }

    // Returns how many surface maps could not be decoded. Pass decoded to keep the images, indexed like the
    // renderer's textures.
    int uploadTextures(Renderer &renderer,std::vector<Image> *decoded = nullptr)
    {
        int failed = 0;
        fixedMeshLayout_ = std::ranges::none_of(playable_.meshes,[](const wf::PlayMesh &mesh) { return !mesh.lodVertices.empty(); });
        meshOffsets_.clear();
        materials_.clear();
        textured_ = 0;
        surfaceMaps_ = 0;
        // Playable files embed maps in each material. Reuse the decoded GPU texture for identical bytes.
        std::unordered_map<std::string_view,int> uploaded;
        for (const auto &mesh : playable_.meshes) {
            MaterialDraw material;
            material.advanced = mesh.advancedSurface;
            material.metallic = mesh.metallic;
            material.roughness = mesh.roughness;
            material.clearCoat = mesh.clearCoat;
            material.clearCoatRoughness = mesh.clearCoatRoughness;
            material.layers = mesh.layers;
            material.surfaceAlpha = mesh.surfaceAlpha;
            material.emissive = mesh.emissive;
            for (std::size_t i = 0; i < material.textures.size(); ++i) {
                const auto &bytes = i == 0 ? mesh.baseColorTexture : mesh.surfaceMaps[i-1];
                if (bytes.empty())
                    continue;
                const std::string_view source(reinterpret_cast<const char *>(bytes.data()),bytes.size());
                const auto [entry,newTexture] = uploaded.try_emplace(source,-1);
                if (newTexture) {
                    auto image = decodeImage(bytes);
                    entry->second = renderer.addTexture(image);
                    if (decoded && entry->second >= 0) {
                        const auto index = std::size_t(entry->second);
                        decoded->resize(std::max(decoded->size(),index+1));
                        (*decoded)[index] = std::move(image);
                    }
                }
                material.textures[i] = entry->second;
                failed += material.textures[i] < 0;
                surfaceMaps_ += i > 0 && material.textures[i] >= 0;
            }
            textured_ += material.textures[0] >= 0;
            materials_.push_back(material);
        }
        return failed;
    }

    void tick(float yaw,int forward,int sideways,bool jump,bool interact)
    {
        clipTime_ += wf::WorldSimulation::stepSeconds;
        playable_.gameplay.update(playable_.simulation,playable_.player,interact);
        const bool controls = !playable_.gameplay.activeShot();
        wf::drivePlayer(player(),spawn_,yaw,controls ? forward : 0,controls ? sideways : 0,controls && jump,jumpBuffer_);
        if (native_.loaded()) {
            std::uint32_t input = 0;
            if (controls) {
                if (forward > 0) input |= WF_GAME_FORWARD;
                if (forward < 0) input |= WF_GAME_BACK;
                if (sideways > 0) input |= WF_GAME_RIGHT;
                if (sideways < 0) input |= WF_GAME_LEFT;
                if (jump) input |= WF_GAME_JUMP;
                if (interact) input |= WF_GAME_INTERACT;
            }
            native_.tick(playable_.simulation,playable_.nativeBodies,playable_.player,input,logNative);
        }
        playable_.simulation.step();
    }

    // The realism check compares lighting and materials without fog, which Cycles would need a volume for.
    // The Cycles reference has no fog and no lens: compare the scene's light alone.
    void clearReferenceEffects()
    {
        playable_.fogDensity = 0;
        playable_.localFogRadius = 0;
        playable_.localFogDensity = 0;
        playable_.weather = {};
        playable_.bloom = false;
        playable_.depthOfField = playable_.motionBlur = false;
    }

    bool setSunElevation(float elevation)
    {
        const auto sun = std::ranges::find(playable_.lights,true,&wf::PlayLight::directional);
        if (sun == playable_.lights.end())
            return false;
        const auto azimuth = std::atan2(sun->vector[1],sun->vector[0]);
        const auto angle = XMConvertToRadians(elevation);
        sun->vector = {std::cos(azimuth)*std::cos(angle),std::sin(azimuth)*std::cos(angle),std::sin(angle)};
        skyLight_.reset();
        return true;
    }

    wf::BoxBody &player() { return playable_.simulation.bodies[playable_.player]; }
    const wf::PlayableScene &playable() const { return playable_; }
    bool sky() const { return playable_.sky; }
    int texturedMeshes() const { return textured_; }
    int surfaceMaps() const { return surfaceMaps_; }
    bool clipMoved() const { return clipMoved_; }
    bool clipLooped() const { return clipLooped_; }

    View view(float yaw,float pitch,float aspect,float time)
    {
        auto target = XMVectorAdd(vector(player().position),XMVectorSet(0,0,0.35f,0));
        const auto offset = XMVectorSet(std::cos(yaw)*std::cos(pitch),std::sin(yaw)*std::cos(pitch),std::sin(pitch),0);
        auto eye = XMVectorAdd(target,XMVectorScale(offset,7.5f));
        if (const auto *shot = playable_.gameplay.activeShot()) {
            target = vector(shot->target);
            eye = vector(wf::shotPosition(*shot,1-shot->remaining/shot->seconds));
        }
        return viewFrom(eye,target,aspect,time);
    }

    // A camera at eye looking at target, as the editor's camera preview uses for comparisons.
    View viewFrom(FXMVECTOR eye,FXMVECTOR target,float aspect,float time)
    {
        View view{};
        const auto direction = XMVector3Normalize(XMVectorSubtract(eye,target));
        const auto up = std::abs(XMVectorGetZ(direction)) > 0.99995f ? XMVectorSet(0,1,0,0) : XMVectorSet(0,0,1,0);
        const auto projection = XMMatrixLookAtRH(eye,target,up)
            * XMMatrixPerspectiveFovRH(wf::camera::verticalFovRadians,aspect,wf::camera::nearPlane,wf::camera::farPlane);
        XMStoreFloat4x4(&view.viewProjection,projection);
        XMStoreFloat4x4(&view.inverseProjection,XMMatrixInverse(nullptr,projection));
        view.eye = parameter(point(eye),0);
        view.horizon = parameter(playable_.horizon,playable_.fogFalloff);
        view.zenith = parameter(playable_.zenith,playable_.fogHeight);
        view.fog = parameter(playable_.fog,playable_.fogDensity);
        view.sun = parameter(point(XMVector3Normalize(XMVectorSet(-0.45f,-0.7f,1,0))),1);
        bool sunSet = false;
        sceneLights_.clear();
        for (const auto &light : playable_.lights) {
            if (light.directional && !sunSet) {
                view.sun = parameter(light.vector,light.intensity);
                sunSet = true;
                continue;
            }
            if (light.intensity > 0)
                sceneLights_.push_back({{light.vector[0],light.vector[1],light.vector[2]},light.intensity,light.color,light.directional,
                                        light.spotDirection,light.spotAngle,light.spotBlend});
            if (view.eye.w < 16 && light.intensity > 0) {
                const int lightIndex = int(view.eye.w);
                view.lights[lightIndex] = parameter(light.vector,light.directional ? -light.intensity : light.intensity);
                view.lightColors[lightIndex] = parameter(light.color,0);
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
        view.display = {wf::color::exposureScale(playable_.exposure),
            playable_.viewTransform == wf::color::ViewTransform::AgX ? 1.0f : 0.0f,playable_.autoExposure ? 1.0f : 0.0f,
            playable_.exposureCompensation};
        view.sky = {wf::color::skyAmbientScale(playable_.horizon,playable_.zenith),playable_.temporalAntiAliasing ? 1.0f : 0.0f,
            playable_.ambientOcclusion ? 1.0f : 0.0f,playable_.physicalSky ? 1.0f : 0.0f};
        // The scene's sky and sun do not change while it plays, so its sky light is computed once.
        if (!skyLight_) {
            const std::array towardSun{view.sun.x,view.sun.y,view.sun.z};
            skyLight_ = playable_.physicalSky ? wf::color::physicalSkyLight(towardSun,view.sun.w)
                                              : wf::color::skyLight(playable_.horizon,playable_.zenith,towardSun,view.sun.w);
            ++skyRevision_;
        }
        view.authoredSun = view.sun.w;
        view.bloom = playable_.bloom ? wf::color::bloomStrength : 0.0f;
        view.lens = {playable_.depthOfField ? 1.0f : 0.0f,playable_.focusDistance,playable_.fNumber,wf::camera::verticalFovRadians};
        view.shutter = {playable_.motionBlur ? 1.0f : 0.0f,playable_.shutter,playable_.reflections ? 1.0f : 0.0f,
                       playable_.contactShadows ? 1.0f : 0.0f};
        view.probeGi = playable_.probeGi;
        view.movingGeometry = std::ranges::any_of(playable_.meshes,[](const wf::PlayMesh &mesh) {
            return mesh.body >= 0 || mesh.clip.has_value();
        }) ? 1u : 0u;
        view.screenSpaceGi = playable_.screenSpaceGi;
        view.reflectionProbes = playable_.reflectionProbes;
        if (fixedMeshLayout_ && !meshOffsets_.empty()) {
            view.cachedMeshVertices = meshOffsets_.back();
            view.movingVertices = &movingVertices_;
        }
        // The physical sky's sun reaches the ground dimmer and warmer than overhead.
        const auto &sunLight = skyLight_->sunLight;
        const float sunLuminance = 0.2126f*sunLight[0]+0.7152f*sunLight[1]+0.0722f*sunLight[2];
        const float clouds = playable_.physicalSky ? playable_.weather.cloudsAt(clipTime_) : 0;
        view.weather = {playable_.physicalSky ? playable_.weather.haze : 0,clouds,0,0};
        view.sun.w = sunLuminance*wf::cloudSunTransmission(clouds);
        view.lighting.y = playable_.shadows && view.sun.w > 0 ? 1.0f : 0.0f;
        view.sunTint = sunLuminance > 0 ? XMFLOAT4{sunLight[0]/sunLuminance,sunLight[1]/sunLuminance,sunLight[2]/sunLuminance,0}
                                        : XMFLOAT4{1,1,1,0};
        view.skyEnvironment = &skyLight_->environment;
        view.skyRevision = skyRevision_;
        view.sceneLights = &sceneLights_;
        for (std::size_t i = 0; i < 9; ++i) {
            const float scale = wf::cloudSkyScale(clouds,i == 0);
            const float directional = wf::cloudSkyScale(clouds,false);
            view.skyIrradianceBand4[i] = {directional*skyLight_->irradianceBand4[i][0],directional*skyLight_->irradianceBand4[i][1],
                                          directional*skyLight_->irradianceBand4[i][2],0};
            view.skyIrradiance[i] = {scale*skyLight_->irradiance[i][0],scale*skyLight_->irradiance[i][1],
                                    scale*skyLight_->irradiance[i][2],0};
        }
        view.skyHorizon = parameter(skyLight_->horizon,skyLight_->sunDiscStrength*wf::cloudSunTransmission(clouds));
        view.skyZenith = parameter(skyLight_->zenith,skyLight_->sunHorizon);
        view.skyGround = parameter(skyLight_->ground,0);
        return view;
    }

    const std::vector<wf::PlayVertex> &vertices()
    {
        const bool cached = fixedMeshLayout_ && !meshOffsets_.empty();
        if (cached) {
            vertices_.resize(meshOffsets_.back());
            ranges_.resize(meshRangeCount_);
        } else {
            vertices_.clear();
            ranges_.clear();
        }
        movingVertices_.clear();
        const auto mark = [this](std::size_t start,const MaterialDraw &material,bool separate = false) {
            const auto count = vertices_.size()-start;
            if (!count)
                return;
            if (!separate && !ranges_.empty() && ranges_.back().material == material
                && ranges_.back().start+ranges_.back().count == UINT(start))
                ranges_.back().count += UINT(count);
            else
                ranges_.push_back({UINT(start),UINT(count),material});
        };
        const auto &world = playable_.simulation;
        for (std::size_t index = 0; index < playable_.meshes.size(); ++index) {
            const auto &mesh = playable_.meshes[index];
            const auto start = cached ? meshOffsets_[index] : vertices_.size();
            if (fixedMeshLayout_ && !cached)
                meshOffsets_.push_back(start);
            if (cached && mesh.body < 0 && !mesh.clip)
                continue;
            const auto material = index < materials_.size() ? materials_[index] : MaterialDraw{};
            auto origin = vector(mesh.lodOrigin);
            if (!mesh.lodVertices.empty() && mesh.body >= 0) {
                const auto &body = world.bodies[mesh.body];
                const auto &q = body.rotation;
                origin = XMVectorAdd(XMVector3Rotate(origin,XMVectorSet(q[0],q[1],q[2],q[3])),vector(body.position));
            }
            const bool distant = !mesh.lodVertices.empty() && XMVectorGetX(XMVector3LengthSq(
                XMVectorSubtract(origin,vector(player().position)))) >= mesh.lodDistance*mesh.lodDistance;
            const auto &vertices = distant ? mesh.lodVertices : mesh.vertices;
            if (mesh.body < 0 && !mesh.clip) {
                vertices_.insert(vertices_.end(),vertices.begin(),vertices.end());
                mark(start,material);
                continue;
            }
            const auto *body = mesh.body >= 0 ? &world.bodies[mesh.body] : nullptr;
            if (fixedMeshLayout_ && !vertices.empty()) {
                if (!movingVertices_.empty() && movingVertices_.back().first+movingVertices_.back().count == start)
                    movingVertices_.back().count += vertices.size();
                else
                    movingVertices_.push_back({start,vertices.size()});
            }
            const auto rotation = body ? XMVectorSet(body->rotation[0],body->rotation[1],body->rotation[2],body->rotation[3])
                : XMQuaternionIdentity();
            const auto phase = mesh.clip ? mesh.clip->phase(clipTime_) : wf::MeshClip::Sample{};
            if (mesh.clip && clipTime_ >= mesh.clip->duration())
                clipLooped_ = true;
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                auto v = vertices[i];
                if (mesh.clip) {
                    const auto sampled = mesh.clip->vertex(phase,i);
                    clipMoved_ |= sampled.position != mesh.clip->samples[i].position;
                    v.position = sampled.position;
                    v.normal = point(XMVector3Normalize(vector(sampled.normal)));
                }
                if (body) {
                    v.position = point(XMVectorAdd(XMVector3Rotate(vector(v.position),rotation),vector(body->position)));
                    v.normal = point(XMVector3Rotate(vector(v.normal),rotation));
                }
                if (cached)
                    vertices_[start+i] = v;
                else
                    vertices_.push_back(v);
            }
            if (!cached)
                mark(start,material);
        }
        const auto terrainStart = vertices_.size();
        if (fixedMeshLayout_ && !cached) {
            meshOffsets_.push_back(terrainStart);
            meshRangeCount_ = ranges_.size();
        }
        for (const auto *beacons : {&playable_.gameplay.beacons,&playable_.gameplay.behaviorBeacons})
            for (const auto &beacon : *beacons) {
                if (beacon.remaining <= 0)
                    continue;
                const auto pulse = wf::makePlayVertices(wf::makeBeaconMesh(beacon));
                vertices_.insert(vertices_.end(),pulse.begin(),pulse.end());
            }
        for (const auto &patrol : playable_.gameplay.patrols) {
            const auto mesh = wf::makePlayVertices(wf::makePatrolMesh(patrol));
            vertices_.insert(vertices_.end(),mesh.begin(),mesh.end());
        }
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
        mark(terrainStart,{},fixedMeshLayout_);
        return vertices_;
    }
    const std::vector<DrawRange> &ranges() const { return ranges_; }

private:
    static void logNative(std::string_view message) noexcept
    {
        std::array<char,1026> line{};
        const auto size = std::min(message.size(),line.size()-2);
        std::copy_n(message.data(),size,line.begin());
        line[size] = '\n';
        OutputDebugStringA(line.data());
    }
    wf::PlayableScene playable_;
    wf::NativeGameplay native_;
    std::optional<wf::color::SkyLight> skyLight_;
    std::uint64_t skyRevision_ = 0;
    std::vector<wf::lights::Light> sceneLights_;
    wf::Point3 spawn_;
    std::vector<std::array<std::vector<wf::PlayVertex>,wf::Heightfield::chunks*wf::Heightfield::chunks>> chunks_;
    std::vector<wf::PlayVertex> vertices_;
    std::vector<DrawRange> ranges_;
    std::vector<std::size_t> meshOffsets_;
    std::vector<wf::player::VertexUpload> movingVertices_;
    std::size_t meshRangeCount_ = 0;
    bool fixedMeshLayout_ = false;
    std::vector<MaterialDraw> materials_;
    int surfaceMaps_ = 0;
    double clipTime_ = 0;
    bool clipMoved_ = false, clipLooped_ = false;
    int textured_ = 0;
    int jumpBuffer_ = 0;
};

LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    if (message == WM_CTLCOLORSTATIC) {
        SetTextColor(reinterpret_cast<HDC>(wParam),RGB(245,241,252));
        SetBkColor(reinterpret_cast<HDC>(wParam),RGB(25,19,38));
        SetDCBrushColor(reinterpret_cast<HDC>(wParam),RGB(25,19,38));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
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
    float maxLaunchRise = 0, maxNativeBodyRise = 0;
    std::map<std::string,float> nativeBodyRises;
    bool mission = false, traffic = false, enteredShop = false;
    bool missionHud = false, gameplayVerified = true;
    wf::GameplayTrace trace;
    std::vector<std::string> objectives;
    // GPU time per pass (RenderStats::gpuMs), summed over the measured frames.
    std::array<double,RenderStats::passCount> gpuMs{};

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
            << "  \"renderer\": \"" << renderer.api() << "\",\n"
            << "  \"missionComplete\": " << (mission ? "true" : "false") << ",\n"
            << "  \"missionHud\": " << (missionHud ? "true" : "false") << ",\n"
            << "  \"beaconEnded\": " << (trace.beaconActive && trace.beaconEnded ? "true" : "false") << ",\n"
            << "  \"shotEnded\": " << (trace.shotActive && trace.shotEnded ? "true" : "false") << ",\n"
            << "  \"patrolReturned\": " << (trace.patrolWaited && trace.patrolReturned ? "true" : "false") << ",\n"
            << "  \"enteredShop\": " << (enteredShop ? "true" : "false") << ",\n"
            << "  \"trafficLooped\": " << (traffic ? "true" : "false") << ",\n"
            << "  \"texturedMeshes\": " << scene.texturedMeshes() << ",\n"
            << "  \"surfaceMaps\": " << scene.surfaceMaps() << ",\n"
            << "  \"clipMoved\": " << (scene.clipMoved() ? "true" : "false") << ",\n"
            << "  \"clipLooped\": " << (scene.clipLooped() ? "true" : "false") << ",\n"
            << "  \"behaviorsFired\": " << std::ranges::count(scene.playable().gameplay.behaviors,
                wf::BehaviorRule::Result::Fired,&wf::BehaviorRule::result) << ",\n"
            << "  \"maxLaunchRiseMeters\": " << maxLaunchRise << ",\n"
            << "  \"nativeFixedUpdates\": " << scene.nativeSteps() << ",\n"
            << "  \"maxNativeBodyRiseMeters\": " << maxNativeBodyRise << ",\n"
            << "  \"nativeBodyRises\": {";
        bool firstBody = true;
        for (const auto &[id,rise] : nativeBodyRises) {
            out << (firstBody ? "" : ", ") << '"' << escape(id) << "\": " << rise;
            firstBody = false;
        }
        out << "},\n  \"simulatedSeconds\": " << seconds << ",\n"
            << "  \"missionSeconds\": " << missionSeconds << ",\n"
            << "  \"frames\": " << frameMs.size() << ",\n"
            << "  \"frameMs\": {\"average\": " << average(frameMs) << ", \"p95\": " << percentile(frameMs,0.95)
            << ", \"p99\": " << percentile(frameMs,0.99) << ", \"max\": " << percentile(frameMs,1) << "},\n"
            << "  \"gpuMs\": {";
        const char *passNames[RenderStats::passCount]{"sunShadows","skyMaps","pointShadows","scene","postProcess"};
        for (std::size_t pass = 0; pass < gpuMs.size(); ++pass)
            out << (pass ? ", " : "") << '"' << passNames[pass] << "\": " << gpuMs[pass]/double(std::max<std::size_t>(frameMs.size(),1));
        out << "},\n"
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

// The city-scale benchmark (--verify-district): a fixed camera route through the scene, first along its outer ring road
// at eye height, then circling over the rooftops toward the center. It records frame times, what each frame submitted
// and memory, for each half of the route and overall.
struct DistrictRun {
    static constexpr int warmUpFrames = 30, routeFrames = 1500;
    float inset = 5, rooftop = 60;
    std::array<float,3> minimum{}, maximum{};
    std::vector<double> frameMs, cpuMs, streetMs, rooftopMs;
    std::uint64_t sceneDraws = 0, depthDraws = 0, sceneTriangles = 0, depthTriangles = 0;
    std::uint64_t rejectedOpaqueDraws = 0, rejectedOpaqueTriangles = 0, occludedOpaqueDraws = 0;
    std::uint64_t pointShadowFaces = 0;
    std::size_t compactedShadowFrames = 0;
    std::uint64_t meshletShaderGroups = 0, meshletIndexedDraws = 0;
    std::size_t promotedTextures = 0, peakTextureDetail = 0;
    uint32_t longestTextureWait = 0;
    double longestTextureStall = 0;
    std::uint32_t peakSceneDraws = 0;
    // GPU time per pass (RenderStats::gpuMs), summed over the street and rooftop halves of the route.
    std::array<double,RenderStats::passCount> streetGpu{}, rooftopGpu{};
    std::size_t streetFrames = 0, rooftopFrames = 0;

    // The camera at fraction t of the route, and where it looks.
    std::pair<XMVECTOR,XMVECTOR> camera(float t) const
    {
        const float x0 = minimum[0]+inset, x1 = maximum[0]-inset, y0 = minimum[1]+inset, y1 = maximum[1]-inset;
        const XMVECTOR center = XMVectorSet((minimum[0]+maximum[0])/2,(minimum[1]+maximum[1])/2,0,0);
        if (t < 0.6f) {
            // Once around the ring road, counterclockwise, looking down the road ahead.
            const float perimeter = 2*(x1-x0)+2*(y1-y0);
            float along = t/0.6f*perimeter;
            const auto at = [&](float distance) {
                distance = std::fmod(distance,perimeter);
                if (distance < x1-x0) return XMVectorSet(x0+distance,y0,1.7f,0);
                distance -= x1-x0;
                if (distance < y1-y0) return XMVectorSet(x1,y0+distance,1.7f,0);
                distance -= y1-y0;
                if (distance < x1-x0) return XMVectorSet(x1-distance,y1,1.7f,0);
                return XMVectorSet(x0,y1-(distance-(x1-x0)),1.7f,0);
            };
            return {at(along),XMVectorAdd(at(along+12),XMVectorSet(0,0,-0.2f,0))};
        }
        // Climbing over the rooftops while circling the center, which keeps the whole district in view.
        const float s = (t-0.6f)/0.4f;
        const float radius = 0.45f*std::max(maximum[0]-minimum[0],maximum[1]-minimum[1]);
        const float angle = -XM_PIDIV2+s*XM_2PI;
        const float height = 1.7f+(rooftop-1.7f)*std::min(1.0f,s*4);
        return {XMVectorSet(XMVectorGetX(center)+radius*std::cos(angle),XMVectorGetY(center)+radius*std::sin(angle),height,0),
                center};
    }
    bool write(const std::filesystem::path &file,const Renderer &renderer,const Scene &scene) const
    {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory));
        const auto average = [](const std::vector<double> &values) {
            return values.empty() ? 0.0 : std::accumulate(values.begin(),values.end(),0.0)/double(values.size());
        };
        const auto percentile = StreetRun::percentile;
        const double frames = double(std::max<std::size_t>(frameMs.size(),1));
        const auto gpuJson = [](const std::array<double,RenderStats::passCount> &totals,std::size_t count) {
            const char *names[RenderStats::passCount]{"sunShadows","skyMaps","pointShadows","scene","postProcess"};
            std::ostringstream json;
            json << '{';
            for (std::size_t pass = 0; pass < totals.size(); ++pass)
                json << (pass ? ", " : "") << '"' << names[pass] << "\": " << totals[pass]/double(std::max<std::size_t>(count,1));
            json << '}';
            return json.str();
        };
        std::size_t vertices = 0;
        for (const auto &mesh : scene.playable().meshes)
            vertices += mesh.vertices.size();
        std::ofstream out(file);
        out << "{\n  \"adapter\": \"" << escape(utf8(renderer.adapterName())) << "\",\n"
            << "  \"renderer\": \"" << renderer.api() << "\",\n"
            << "  \"frames\": " << frameMs.size() << ",\n"
            << "  \"meshes\": " << scene.playable().meshes.size() << ",\n"
            << "  \"lights\": " << scene.playable().lights.size() << ",\n"
            << "  \"vertices\": " << vertices << ",\n"
            << "  \"frameMs\": {\"average\": " << average(frameMs) << ", \"p95\": " << percentile(frameMs,0.95)
            << ", \"p99\": " << percentile(frameMs,0.99) << ", \"max\": " << percentile(frameMs,1) << "},\n"
            << "  \"streetMs\": {\"average\": " << average(streetMs) << ", \"p95\": " << percentile(streetMs,0.95) << "},\n"
            << "  \"rooftopMs\": {\"average\": " << average(rooftopMs) << ", \"p95\": " << percentile(rooftopMs,0.95) << "},\n"
            << "  \"cpuMs\": {\"average\": " << average(cpuMs) << ", \"p95\": " << percentile(cpuMs,0.95)
            << ", \"max\": " << percentile(cpuMs,1) << "},\n"
            << "  \"sceneDraws\": {\"average\": " << double(sceneDraws)/frames << ", \"max\": " << peakSceneDraws << "},\n"
            << "  \"sceneTriangles\": " << double(sceneTriangles)/frames << ",\n"
            << "  \"visibleSceneDraws\": " << (double(sceneDraws)-double(rejectedOpaqueDraws))/frames << ",\n"
            << "  \"visibleSceneTriangles\": " << (double(sceneTriangles)-double(rejectedOpaqueTriangles))/frames << ",\n"
            << "  \"rejectedOpaqueDraws\": " << double(rejectedOpaqueDraws)/frames << ",\n"
            << "  \"occludedOpaqueDraws\": " << double(occludedOpaqueDraws)/frames << ",\n"
            << "  \"rejectedOpaqueTriangles\": " << double(rejectedOpaqueTriangles)/frames << ",\n"
            << "  \"pointShadowFacesPerFrame\": " << double(pointShadowFaces)/frames << ",\n"
            << "  \"compactedShadowFrames\": " << compactedShadowFrames << ",\n"
            << "  \"meshletShaderGroups\": " << meshletShaderGroups << ", \"meshletIndexedDraws\": " << meshletIndexedDraws << ",\n"
            << "  \"textureStreaming\": {\"promotions\": " << promotedTextures << ", \"longestWaitFrames\": " << longestTextureWait
            << ", \"peakDetailBytes\": " << peakTextureDetail << ", \"longestStallMs\": " << longestTextureStall << "},\n"
            << "  \"depthDraws\": " << double(depthDraws)/frames << ",\n"
            << "  \"depthTriangles\": " << double(depthTriangles)/frames << ",\n"
            << "  \"gpuMs\": {\"street\": " << gpuJson(streetGpu,streetFrames) << ", \"rooftop\": "
            << gpuJson(rooftopGpu,rooftopFrames) << "},\n"
            << "  \"workingSetMiB\": " << double(memory.WorkingSetSize)/1048576
            << ",\n  \"peakWorkingSetMiB\": " << double(memory.PeakWorkingSetSize)/1048576
            << ",\n  \"privateMiB\": " << double(memory.PrivateUsage)/1048576 << "\n}\n";
        return bool(out);
    }
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show)
{
    int argc = 0;
    auto **args = CommandLineToArgvW(GetCommandLineW(),&argc);
    const bool directDraws = argc > 1 && std::wstring_view(args[argc-1]) == L"--direct";
    if (directDraws)
        --argc;
    std::filesystem::path module;
    if (argc >= 4 && std::wstring_view(args[argc-2]) == L"--module") {
        module = args[argc-1];
        argc -= 2;
    }
    const bool software = argc == 3 && std::wstring_view(args[1]) == L"--smoke-test-warp";
    const bool smoke = software || (argc == 3 && std::wstring_view(args[1]) == L"--smoke-test");
    const bool packagedStreet = argc == 3 && std::wstring_view(args[1]) == L"--verify-packaged-street";
    const bool street = packagedStreet || (argc == 4 && std::wstring_view(args[1]) == L"--verify-street");
    // --verify-district <scene> <report> <inset> <rooftop> flies the city-scale benchmark route (DistrictRun): the ring road
    // runs inset meters inside the scene's bounds, and the camera then circles at rooftop meters.
    const bool districtRun = argc == 6 && std::wstring_view(args[1]) == L"--verify-district";
    // --capture <scene> <png> <width> <height> <eye x y z> <target x y z> renders one frame off screen.
    // --reference takes the same arguments with a folder in place of the PNG: it writes the frame without fog as
    // engine.png, plus the scene description that Tests/CyclesReference.py renders with Cycles.
    const bool reference = argc == 12 && std::wstring_view(args[1]) == L"--reference";
    // --verify-sunrise uses the same camera arguments and writes a meter sweep and key frames to a folder.
    const bool sunrise = argc == 12 && std::wstring_view(args[1]) == L"--verify-sunrise";
    const bool sunset = argc == 12 && std::wstring_view(args[1]) == L"--verify-sunset";
    const bool capture = sunrise || sunset || reference || (argc == 12 && std::wstring_view(args[1]) == L"--capture");
    const bool scripted = smoke || street || capture || districtRun;
    std::filesystem::path capturePath;
    UINT captureWidth = 0, captureHeight = 0;
    std::array<float,6> captureCamera{};
    if (capture) {
        capturePath = args[3];
        captureWidth = UINT(std::clamp(_wtoi(args[4]),16,4096));
        captureHeight = UINT(std::clamp(_wtoi(args[5]),16,4096));
        for (int i = 0; i < 6; ++i)
            captureCamera[i] = float(_wtof(args[6+i]));
    }
    auto file = argc >= 2 && !packagedStreet ? std::filesystem::path(args[scripted ? 2 : 1]) : std::filesystem::path();
    const auto reportFile = street ? std::filesystem::path(args[packagedStreet ? 2 : 3])
        : districtRun ? std::filesystem::path(args[3]) : std::filesystem::path();
    DistrictRun district;
    if (districtRun) {
        district.inset = float(std::clamp(_wtof(args[4]),0.0,1000.0));
        district.rooftop = float(std::clamp(_wtof(args[5]),2.0,2000.0));
    }
    LocalFree(args);
    if (argc < 2 || packagedStreet) {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr,executable,DWORD(std::size(executable)));
        if (length && length < DWORD(std::size(executable))) {
            const auto folder = std::filesystem::path(executable).parent_path();
            std::error_code ignored;
            if (std::filesystem::is_regular_file(folder / "Game.wfplay",ignored)) {
                file = folder / "Game.wfplay";
                module = folder / "Gameplay.dll";
            }
        }
        if (file.empty() && !packagedStreet)
            file = chooseScene();
    }
    if (file.empty())
        return packagedStreet ? 1 : 0;
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
    if (districtRun) {
        // The route follows the bounds of the static geometry.
        district.minimum = {std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
        district.maximum = {std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest()};
        for (const auto &mesh : scene.playable().meshes)
            if (mesh.body < 0)
                for (const auto &vertex : mesh.vertices)
                    for (int axis = 0; axis < 3; ++axis) {
                        district.minimum[axis] = std::min(district.minimum[axis],vertex.position[axis]);
                        district.maximum[axis] = std::max(district.maximum[axis],vertex.position[axis]);
                    }
        if (district.minimum[0] > district.maximum[0])
            return fail(L"The district has no static geometry to fly through.");
    }
    if (!module.empty()) {
        std::string error;
        if (!scene.stageNative(module,error))
            return fail(wide(error).c_str());
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    windowClass.lpszClassName = L"WorldForgePlayer";
    if (!RegisterClassW(&windowClass))
        return fail(L"Could not register the Player window.");
    const std::wstring controls = L"WASD Move   Space Jump   E Interact   Right Mouse Orbit   Esc Exit";
    const auto window = CreateWindowExW(0,windowClass.lpszClassName,(L"WorldForge Player   |   " + controls).c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,smoke ? 400 : 1280,smoke ? 280 : 720,
        nullptr,nullptr,instance,nullptr);
    if (!window)
        return fail(L"Could not create the Player window.");
    // Vulkan is the renderer. Direct3D 11 is the fallback without a Vulkan driver, for the WARP smoke
    // test, or when WORLDFORGE_PLAYER_RENDERER=d3d11.
    std::unique_ptr<Renderer> renderer;
    wchar_t requested[16]{};
    GetEnvironmentVariableW(L"WORLDFORGE_PLAYER_RENDERER",requested,DWORD(std::size(requested)));
    const bool preferD3D11 = software || std::wstring_view(requested) == L"d3d11";
#ifdef WORLDFORGE_PLAYER_VULKAN
    std::wstring vulkanError;
    if (!preferD3D11)
        renderer = wf::player::createVulkanRenderer(window,vulkanError);
    if (!renderer && !preferD3D11)
        OutputDebugStringW((L"WorldForge Player: Vulkan unavailable, using Direct3D 11. " + vulkanError + L"\n").c_str());
#endif
    if (!renderer && !capture) {
        auto fallback = std::make_unique<D3D11Renderer>();
        if (fallback->start(window,software))
            renderer = std::move(fallback);
    }
    if (!renderer)
        return fail(L"Could not start the renderer. Update the graphics driver.");
    std::vector<Image> textures;
    if (scene.uploadTextures(*renderer,reference ? &textures : nullptr) > 0)
        return fail(L"A material texture in this scene could not be decoded.");
    if (capture) {
        const auto eye = XMVectorSet(captureCamera[0],captureCamera[1],captureCamera[2],0);
        const auto target = XMVectorSet(captureCamera[3],captureCamera[4],captureCamera[5],0);
        if (reference)
            scene.clearReferenceEffects();
        const auto &vertices = scene.vertices();
        const float aspect = float(captureWidth)/float(captureHeight);
        if (sunrise || sunset) {
            std::error_code error;
            std::filesystem::create_directories(capturePath,error);
            std::ofstream report(capturePath / L"meter.csv");
            if (error || !report)
                return 4;
            report << "Elevation,Log2Luminance\n";
            constexpr int steps = 90;
            for (int frame = 0; frame <= steps; ++frame) {
                const int sample = sunrise ? frame : steps-frame;
                const float elevation = float(sample-30)/5;
                if (!scene.setSunElevation(elevation))
                    return 4;
                const auto shown = scene.viewFrom(eye,target,aspect,0);
                float meter = std::numeric_limits<float>::quiet_NaN();
                Image frameImage;
                if (!renderer->capture(vertices,scene.ranges(),shown,captureWidth,captureHeight,scene.sky(),frameImage,&meter)
                    || !std::isfinite(meter))
                    return 4;
                report << elevation << ',' << meter << '\n';
                if ((sample == 0 || sample == 30 || sample == steps)
                    && !savePng(capturePath / (std::to_wstring(frame)+L".png"),frameImage))
                    return 4;
            }
            return report ? 0 : 4;
        }
        auto view = scene.viewFrom(eye,target,aspect,0);
        view.drawCulling = !directDraws;
        Image image;
        if (!renderer->capture(vertices,scene.ranges(),view,captureWidth,captureHeight,scene.sky(),image))
            return 4;
        if (!reference)
            return savePng(capturePath,image) ? 0 : 4;
        std::error_code ignored;
        std::filesystem::create_directories(capturePath,ignored);
        for (std::size_t i = 0; i < textures.size(); ++i)
            if (!textures[i].pixels.empty() && !savePng(capturePath / (L"texture-" + std::to_wstring(i) + L".png"),textures[i]))
                return 4;
        const ReferenceCamera camera{{captureCamera[0],captureCamera[1],captureCamera[2]},
            {captureCamera[3],captureCamera[4],captureCamera[5]},wf::camera::verticalFovRadians,captureWidth,captureHeight};
        std::string error;
        if (!savePng(capturePath / L"engine.png",image)
            || !writeReference(capturePath,vertices,scene.ranges(),view,camera,scene.sky(),0,error)) {
            OutputDebugStringA(("WorldForge Player: " + error + "\n").c_str());
            return 4;
        }
        return 0;
    }
    HWND missionHud = nullptr;
    if (scene.playable().gameplay.showMissionHud && !scene.playable().gameplay.missions.empty()) {
        missionHud = CreateWindowExW(0,L"STATIC",L"",WS_CHILD | SS_LEFT | SS_NOPREFIX,
            16,16,1,1,window,nullptr,instance,nullptr);
        if (missionHud)
            SendMessageW(missionHud,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE);
    }
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
    LONG lastHudWidth = 0;
    StreetRun run;
    std::vector<std::pair<std::size_t,float>> launchTargets;
    struct NativeTarget { std::size_t body; float startHeight; float *rise; };
    std::vector<NativeTarget> nativeTargets;
    if (street)
        for (const auto &rule : scene.playable().gameplay.behaviors)
            if (rule.action == wf::BehaviorRule::Action::LaunchUpward)
                launchTargets.emplace_back(rule.target,scene.playable().simulation.bodies[rule.target].position[2]);
    if (street && !module.empty())
        for (const auto &binding : scene.playable().nativeBodies) {
            const auto &body = scene.playable().simulation.bodies[binding.index];
            if (body.dynamic)
                nativeTargets.push_back({binding.index,body.position[2],&run.nativeBodyRises[binding.id]});
        }
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
                for (const auto &[body,startHeight] : launchTargets)
                    run.maxLaunchRise = std::max(run.maxLaunchRise,
                        scene.playable().simulation.bodies[body].position[2]-startHeight);
                for (const auto &[body,startHeight,rise] : nativeTargets) {
                    *rise = std::max(*rise,scene.playable().simulation.bodies[body].position[2]-startHeight);
                    run.maxNativeBodyRise = std::max(run.maxNativeBodyRise,*rise);
                }
                yaw = pilotYaw;
                run.seconds += wf::WorldSimulation::stepSeconds;
            } else {
                const bool testJump = smoke && !testJumped && scene.player().grounded;
                // The district benchmark flies its own camera; the player stands still.
                const bool keys = !districtRun;
                scene.tick(yaw,smoke ? int(frames >= 25) : keys ? int(down('W'))-int(down('S')) : 0,
                    keys ? int(down('D'))-int(down('A')) : 0,keys && (jumpPending || testJump),keys && interactPending);
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
            if (missionHud) {
                SetWindowTextW(missionHud,wide(objective).c_str());
                ShowWindow(missionHud,objective.empty() ? SW_HIDE : SW_SHOWNOACTIVATE);
            }
        }
        if (missionHud && client.right != lastHudWidth) {
            lastHudWidth = client.right;
            SetWindowPos(missionHud,HWND_TOP,16,16,std::clamp<int>(client.right-32,1,460),38,SWP_NOACTIVATE);
        }
        const float aspect = float(client.right)/client.bottom;
        const float seconds = std::chrono::duration<float>(now-launched).count();
        const float routeFraction = std::clamp(float(frames-DistrictRun::warmUpFrames)/float(DistrictRun::routeFrames),0.0f,1.0f);
        const auto &vertices = scene.vertices();
        auto view = districtRun
            ? scene.viewFrom(district.camera(routeFraction).first,district.camera(routeFraction).second,aspect,seconds)
            : scene.view(yaw,pitch,aspect,seconds);
        view.drawCulling = !directDraws;
        const auto beforePresent = Clock::now();
        if (!renderer->draw(vertices,scene.ranges(),view,UINT(client.right),UINT(client.bottom),scene.sky(),
                           smoke && scene.sky() && frames == 0,street || districtRun ? 0 : 1))
            return fail(L"The Player could not render the current scene.");
        if (districtRun) {
            const auto end = Clock::now();
            const auto stats = renderer->stats();
            district.promotedTextures += stats.texturesPromoted;
            district.longestTextureWait = std::max(district.longestTextureWait,stats.textureWaitFrames);
            district.peakTextureDetail = std::max(district.peakTextureDetail,stats.textureDetailBytes);
            district.longestTextureStall = std::max(district.longestTextureStall,stats.textureStallMs);
            if (++frames > DistrictRun::warmUpFrames) {
                const double frameMs = std::chrono::duration<double,std::milli>(end-frameStart).count();
                district.frameMs.push_back(frameMs);
                (routeFraction < 0.6f ? district.streetMs : district.rooftopMs).push_back(frameMs);
                district.cpuMs.push_back(std::chrono::duration<double,std::milli>(beforePresent-frameStart).count());
                district.sceneDraws += stats.sceneDraws;
                district.depthDraws += stats.depthDraws;
                district.sceneTriangles += stats.sceneTriangles;
                district.rejectedOpaqueDraws += stats.rejectedOpaqueDraws;
                district.occludedOpaqueDraws += stats.occludedOpaqueDraws;
                district.rejectedOpaqueTriangles += stats.rejectedOpaqueTriangles;
                district.pointShadowFaces += stats.pointShadowFaces;
                district.compactedShadowFrames += stats.compactedShadowDraws ? 1 : 0;
                district.meshletShaderGroups += stats.meshletShaderGroups;
                district.meshletIndexedDraws += stats.meshletIndexedDraws;
                district.depthTriangles += stats.depthTriangles;
                district.peakSceneDraws = std::max(district.peakSceneDraws,stats.sceneDraws);
                const bool onStreet = routeFraction < 0.6f;
                auto &gpu = onStreet ? district.streetGpu : district.rooftopGpu;
                for (std::size_t pass = 0; pass < gpu.size(); ++pass)
                    gpu[pass] += stats.gpuMs[pass];
                ++(onStreet ? district.streetFrames : district.rooftopFrames);
            }
            if (frames >= DistrictRun::warmUpFrames+DistrictRun::routeFrames)
                return district.write(reportFile,*renderer,scene) ? 0 : 3;
        }
        if (smoke && ++frames == 90) {
            const auto &position = scene.player().position;
            return testJumped && std::hypot(position[0]-initialPosition[0],position[1]-initialPosition[1]) > 1 ? 0 : 2;
        }
        if (street) {
            run.missionHud |= missionHud && IsWindowVisible(missionHud);
            run.trace.observe(gameplay);
            run.gameplayVerified = run.trace.complete(gameplay);
            const auto end = Clock::now();
            if (++frames > 30) {
                run.frameMs.push_back(std::chrono::duration<double,std::milli>(end-frameStart).count());
                run.cpuMs.push_back(std::chrono::duration<double,std::milli>(beforePresent-frameStart).count());
                const auto stats = renderer->stats();
                for (std::size_t pass = 0; pass < run.gpuMs.size(); ++pass)
                    run.gpuMs[pass] += stats.gpuMs[pass];
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
            if ((run.mission && run.traffic && run.gameplayVerified) || run.seconds > 180) {
                const bool written = run.write(reportFile,*renderer,scene);
                return written && run.mission && run.traffic && run.enteredShop && run.gameplayVerified ? 0 : 3;
            }
        }
    }
}
